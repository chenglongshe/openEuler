// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2022-2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include "bnxt_re.h"
#include "bnxt.h"
#include "bnxt_hdbr.h"
#include "hdbr.h"

static void bnxt_re_hdbr_wait_hw_read_complete(struct bnxt_re_dev *rdev, int group)
{
	/*
	 * TODO: We need a deterministic signal/event/operation here to make sure
	 * HW doesn't read host memory of DB copy region. Then we could go ahead
	 * to free the page safely.
	 */
}

static void bnxt_re_hdbr_free_pg_task(struct work_struct *work)
{
	struct bnxt_re_hdbr_free_pg_work *wk =
			container_of(work, struct bnxt_re_hdbr_free_pg_work, work);

	bnxt_re_hdbr_wait_hw_read_complete(wk->rdev, wk->group);

	/*
	 * TODO: As a temporary solution of preventing HW access a freed page, we'll
	 * not free the page. Instead, we put it into reusable free page list.
	 * dma_free_coherent(&wk->rdev->en_dev->pdev->dev, wk->size, wk->pg->kptr,
	 * wk->pg->da);
	 * kfree(wk->pg);
	 */
	mutex_lock(&wk->rdev->hdbr_fpg_lock);
	list_add_tail(&wk->pg->pg_node, &wk->rdev->hdbr_fpgs);
	mutex_unlock(&wk->rdev->hdbr_fpg_lock);

	kfree(wk);
}

static struct hdbr_pg *hdbr_reuse_page(struct bnxt_re_dev *rdev)
{
	struct hdbr_pg *pg = NULL;

	mutex_lock(&rdev->hdbr_fpg_lock);
	if (!list_empty(&rdev->hdbr_fpgs)) {
		pg = list_first_entry(&rdev->hdbr_fpgs, struct hdbr_pg, pg_node);
		list_del(&pg->pg_node);
	}
	mutex_unlock(&rdev->hdbr_fpg_lock);

	return pg;
}

/*
 * This function allocates a 4K page as DB copy app page, and link it to the
 * main kernel table which is managed by L2 driver.
 *
 * Inside RoCE DB copy app page, DBs are grouped by group type.
 *     DBC_GROUP_SQ  : grp_size = 1,
 *		       offset 0: SQ producer index doorbell
 *     DBC_GROUP_RQ  : grp_size = 1,
 *		       offset 0: RQ producer index doorbell
 *     DBC_GROUP_SRQ : grp_size = 3,
 *		       offset 0: SRQ producer index doorbell
 *		       offset 1: SRQ_ARMENA (must before SRQ_ARM)
 *		       offset 2: SRQ_ARM
 *     DBC_GROUP_CQ  : grp_size = 4,
 *		       offset 0: CQ consumer index doorbell
 *		       offset 1: CQ_ARMENA (must before CQ_ARMALL/SE)
 *		       offset 2: CQ_ARMALL/CQ_ARMASE (share slot)
 *		       offset 3: CUTOFF_ACK
 */
static struct hdbr_pg *hdbr_alloc_page(struct bnxt_re_dev *rdev, int group, u16 pi)
{
	struct bnxt_hdbr_ktbl *ktbl;
	struct hdbr_pg *pg;
	int rc;

	ktbl = rdev->en_dev->hdbr_info->ktbl[group];
	if (!ktbl)
		return NULL;
	pg = hdbr_reuse_page(rdev);
	if (pg) {
		u64 *kptr = pg->kptr;
		dma_addr_t da = pg->da;

		memset(pg, 0, sizeof(*pg));
		memset(kptr, 0, PAGE_SIZE_4K);
		pg->kptr = kptr;
		pg->da = da;
	} else {
		pg = kzalloc(sizeof(*pg), GFP_KERNEL);
		if (!pg)
			return NULL;
		pg->kptr = dma_alloc_coherent(&rdev->en_dev->pdev->dev, PAGE_SIZE_4K, &pg->da,
					      GFP_KERNEL | __GFP_ZERO);
		if (!pg->kptr)
			goto alloc_err;
	}
	pg->grp_size = bnxt_re_hdbr_group_size(group);
	pg->first_avail = 0;
	pg->first_empty = 0;
	pg->size = PAGE_SIZE_4K / HDBR_DB_SIZE / pg->grp_size;
	pg->blk_avail = pg->size;
	/* Register this page to main kernel table in L2 driver */
	rc = bnxt_hdbr_reg_apg(ktbl, pg->da, &pg->ktbl_idx, pi);
	if (rc)
		goto reg_page_err;

	return pg;

reg_page_err:
	dma_free_coherent(&rdev->en_dev->pdev->dev, PAGE_SIZE_4K, pg->kptr, pg->da);

alloc_err:
	kfree(pg);

	return NULL;
}

static void hdbr_dealloc_page(struct bnxt_re_dev *rdev, struct hdbr_pg *pg, int group)
{
	struct bnxt_hdbr_ktbl *ktbl = rdev->en_dev->hdbr_info->ktbl[group];
	struct bnxt_re_hdbr_free_pg_work *wk;

	if (!ktbl) {
		dev_err(rdev_to_dev(rdev), "L2 driver has no support for unreg page!");
		return;
	}
	/* Unregister this page from main kernel table in L2 driver */
	bnxt_hdbr_unreg_apg(ktbl, pg->ktbl_idx);

	/* Free page and structure memory in background */
	wk = kzalloc(sizeof(*wk), GFP_ATOMIC);
	if (!wk) {
		dev_err(rdev_to_dev(rdev), "Failed to allocate wq for freeing page!");
		return;
	}
	wk->rdev = rdev;
	wk->pg = pg;
	wk->size = PAGE_SIZE_4K;
	wk->group = group;
	INIT_WORK(&wk->work, bnxt_re_hdbr_free_pg_task);
	queue_work(rdev->hdbr_wq, &wk->work);
}

static __le64 *hdbr_claim_slot(struct hdbr_pg *pg)
{
	int i, n, idx;

	n = pg->grp_size;
	idx = pg->first_avail * n;
	for (i = 0; i < n; i++)
		pg->kptr[idx + i] = cpu_to_le64(DBC_VALUE_INIT);
	pg->blk_avail--;

	/* Update indice for next */
	if (pg->first_avail == pg->first_empty) {
		pg->first_avail++;
		pg->first_empty++;
		if (pg->first_empty < pg->size)
			pg->kptr[pg->first_empty * n] = cpu_to_le64(DBC_VALUE_LAST);
	} else {
		while (++pg->first_avail < pg->first_empty) {
			if (!pg->kptr[pg->first_avail * n])
				break;
		}
	}
	return pg->kptr + idx;
}

static void hdbr_clear_slot(struct hdbr_pg *pg, int pos)
{
	int i;

	for (i = 0; i < pg->grp_size; i++)
		pg->kptr[pos * pg->grp_size + i] = 0;
	pg->blk_avail++;
	if (pos < pg->first_avail)
		pg->first_avail = pos;
}

static void bnxt_re_hdbr_db_unreg(struct bnxt_re_dev *rdev, int group,
				  struct bnxt_qplib_db_info *dbinfo)
{
	struct bnxt_re_hdbr_app *app;
	struct hdbr_pg_lst *plst;
	struct hdbr_pg *pg;
	bool found = false;
	int ktbl_idx;
	__le64 *dbc;

	if (group >= DBC_GROUP_MAX)
		return;
	app = dbinfo->app;
	ktbl_idx = dbinfo->ktbl_idx;
	dbc = dbinfo->dbc;
	if (!app || !dbc) {
		dev_err(rdev_to_dev(rdev), "Invalid unreg db params, app=0x%px, ktbl_idx=%d,"
			" dbc=0x%px\n", app, ktbl_idx, dbc);
		return;
	}

	plst = &app->pg_lst[group];
	mutex_lock(&plst->lst_lock);
	list_for_each_entry(pg, &plst->pg_head, pg_node) {
		if (pg->ktbl_idx == ktbl_idx) {
			int pos;

			pos = ((u64)dbc - (u64)pg->kptr) / HDBR_DB_SIZE / pg->grp_size;
			hdbr_clear_slot(pg, pos);
			plst->blk_avail++;
			found = true;
			break;
		}
	}

	/* Additionally, free the page if it is empty. */
	if (found && pg->blk_avail == pg->size) {
		plst->blk_avail -= pg->blk_avail;
		list_del(&pg->pg_node);
		hdbr_dealloc_page(rdev, pg, group);
	}

	mutex_unlock(&plst->lst_lock);

	dbinfo->app = NULL;
	dbinfo->ktbl_idx = 0;
	dbinfo->dbc = NULL;

	if (!found)
		dev_err(rdev_to_dev(rdev), "Fatal: DB copy not found\n");
}

void bnxt_re_hdbr_db_unreg_srq(struct bnxt_re_dev *rdev, struct bnxt_re_srq *srq)
{
	struct bnxt_qplib_db_info *dbinfo = &srq->qplib_srq.dbinfo;

	bnxt_re_hdbr_db_unreg(rdev, DBC_GROUP_SRQ, dbinfo);
}

void bnxt_re_hdbr_db_unreg_qp(struct bnxt_re_dev *rdev, struct bnxt_re_qp *qp)
{
	struct bnxt_qplib_db_info *dbinfo;

	dbinfo = &qp->qplib_qp.sq.dbinfo;
	bnxt_re_hdbr_db_unreg(rdev, DBC_GROUP_SQ, dbinfo);
	if (qp->qplib_qp.srq)
		return;
	dbinfo = &qp->qplib_qp.rq.dbinfo;
	bnxt_re_hdbr_db_unreg(rdev, DBC_GROUP_RQ, dbinfo);
}

void bnxt_re_hdbr_db_unreg_cq(struct bnxt_re_dev *rdev, struct bnxt_re_cq *cq)
{
	struct bnxt_qplib_db_info *dbinfo = &cq->qplib_cq.dbinfo;

	bnxt_re_hdbr_db_unreg(rdev, DBC_GROUP_CQ, dbinfo);
}

static __le64 *bnxt_re_hdbr_db_reg(struct bnxt_re_dev *rdev, struct bnxt_re_hdbr_app *app,
				   int group, int *ktbl_idx, u16 pi)
{
	struct hdbr_pg_lst *plst;
	struct hdbr_pg *pg;
	__le64 *dbc = NULL;

	if (group >= DBC_GROUP_MAX)
		return NULL;

	plst = &app->pg_lst[group];
	mutex_lock(&plst->lst_lock);
	if (plst->blk_avail == 0) {
		pg = hdbr_alloc_page(rdev, group, pi);
		if (!pg)
			goto exit;
		list_add(&pg->pg_node, &plst->pg_head);
		plst->blk_avail += pg->blk_avail;
	}
	list_for_each_entry(pg, &plst->pg_head, pg_node) {
		if (pg->blk_avail > 0) {
			dbc = hdbr_claim_slot(pg);
			*ktbl_idx = pg->ktbl_idx;
			plst->blk_avail--;
			break;
		}
	}

exit:
	mutex_unlock(&plst->lst_lock);
	return dbc;
}

int bnxt_re_hdbr_db_reg_srq(struct bnxt_re_dev *rdev, struct bnxt_re_srq *srq,
			    struct bnxt_re_ucontext *cntx, struct bnxt_re_srq_resp *resp)
{
	struct bnxt_qplib_db_info *dbinfo;
	struct bnxt_re_hdbr_app *app;
	u16 pi = 0;

	dbinfo = &srq->qplib_srq.dbinfo;
	if (cntx) {
		app = cntx->hdbr_app;
		pi = (u16)cntx->dpi.dpi;
	} else {
		app = container_of(rdev->hdbr_privileged, struct bnxt_re_hdbr_app, lst);
	}

	dbinfo->dbc = bnxt_re_hdbr_db_reg(rdev, app, DBC_GROUP_SRQ, &dbinfo->ktbl_idx, pi);
	if (!dbinfo->dbc)
		return -ENOMEM;
	dbinfo->app = app;
	dbinfo->dbc_dt = 0;
	if (resp)
		resp->hdbr_kaddr = (__u64)dbinfo->dbc;
	return 0;
}

int bnxt_re_hdbr_db_reg_qp(struct bnxt_re_dev *rdev, struct bnxt_re_qp *qp,
			   struct bnxt_re_pd *pd, struct bnxt_re_qp_resp *resp)
{
	struct bnxt_re_ucontext *cntx = NULL;
	struct ib_ucontext *context = NULL;
	struct bnxt_qplib_db_info *dbinfo;
	struct bnxt_re_hdbr_app *app;
	u16 pi = 0;

	if (pd) {
		context = pd->ib_pd.uobject->context;
		cntx = to_bnxt_re(context, struct bnxt_re_ucontext, ib_uctx);
	}
	if (cntx) {
		app = cntx->hdbr_app;
		pi = (u16)cntx->dpi.dpi;
	} else {
		app = container_of(rdev->hdbr_privileged, struct bnxt_re_hdbr_app, lst);
	}

	/* sq */
	dbinfo = &qp->qplib_qp.sq.dbinfo;
	dbinfo->dbc = bnxt_re_hdbr_db_reg(rdev, app, DBC_GROUP_SQ, &dbinfo->ktbl_idx, pi);
	if (!dbinfo->dbc)
		return -ENOMEM;
	dbinfo->app = app;
	if (*rdev->hdbr_dt)
		dbinfo->dbc_dt = 1;
	else
		dbinfo->dbc_dt = 0;
	if (resp) {
		resp->hdbr_kaddr_sq = (__u64)dbinfo->dbc;
		resp->hdbr_dt = (__u32)dbinfo->dbc_dt;
	}

	if (qp->qplib_qp.srq)
		return 0;

	/* rq */
	dbinfo = &qp->qplib_qp.rq.dbinfo;
	dbinfo->dbc = bnxt_re_hdbr_db_reg(rdev, app, DBC_GROUP_RQ, &dbinfo->ktbl_idx, pi);
	if (!dbinfo->dbc) {
		bnxt_re_hdbr_db_unreg_qp(rdev, qp);
		return -ENOMEM;
	}
	dbinfo->app = app;
	dbinfo->dbc_dt = 0;
	if (resp)
		resp->hdbr_kaddr_rq = (__u64)dbinfo->dbc;

	return 0;
}

int bnxt_re_hdbr_db_reg_cq(struct bnxt_re_dev *rdev, struct bnxt_re_cq *cq,
			   struct bnxt_re_ucontext *cntx, struct bnxt_re_cq_resp *resp,
			   struct bnxt_re_cq_req *ureq)
{
	struct bnxt_qplib_db_info *dbinfo;
	struct bnxt_re_hdbr_app *app;
	u16 pi = 0;

	dbinfo = &cq->qplib_cq.dbinfo;
	if (cntx) {
		app = cntx->hdbr_app;
		pi = (u16)cntx->dpi.dpi;
	} else {
		app = container_of(rdev->hdbr_privileged, struct bnxt_re_hdbr_app, lst);
	}

	dbinfo->dbc = bnxt_re_hdbr_db_reg(rdev, app, DBC_GROUP_CQ, &dbinfo->ktbl_idx, pi);
	if (!dbinfo->dbc)
		return -ENOMEM;
	dbinfo->app = app;
	dbinfo->dbc_dt = 0;
	if (resp && ureq && ureq->comp_mask & BNXT_RE_COMP_MASK_CQ_REQ_HAS_HDBR_KADDR) {
		resp->hdbr_kaddr = (__u64)dbinfo->dbc;
		resp->comp_mask |= BNXT_RE_COMP_MASK_CQ_HAS_HDBR_KADDR;
	}
	return 0;
}

struct bnxt_re_hdbr_app *bnxt_re_hdbr_alloc_app(struct bnxt_re_dev *rdev, bool user)
{
	struct bnxt_re_hdbr_app *app;
	int group;

	app = kzalloc(sizeof(*app), GFP_KERNEL);
	if (!app) {
		dev_err(rdev_to_dev(rdev), "hdbr app alloc failed!");
		return NULL;
	}
	INIT_LIST_HEAD(&app->lst);
	for (group = DBC_GROUP_SQ; group < DBC_GROUP_MAX; group++) {
		app->pg_lst[group].group = group;
		INIT_LIST_HEAD(&app->pg_lst[group].pg_head);
		app->pg_lst[group].blk_avail = 0;
		mutex_init(&app->pg_lst[group].lst_lock);
	}

	if (user) {
		mutex_lock(&rdev->hdbr_lock);
		list_add(&app->lst, &rdev->hdbr_apps);
		mutex_unlock(&rdev->hdbr_lock);
	}

	return app;
}

void bnxt_re_hdbr_dealloc_app(struct bnxt_re_dev *rdev, struct bnxt_re_hdbr_app *app)
{
	struct list_head *head;
	struct hdbr_pg *pg;
	int group;

	for (group = DBC_GROUP_SQ; group < DBC_GROUP_MAX; group++) {
		head = &app->pg_lst[group].pg_head;
		while (!list_empty(head)) {
			pg = list_first_entry(head, struct hdbr_pg, pg_node);
			list_del(&pg->pg_node);
			hdbr_dealloc_page(rdev, pg, group);
		}
	}

	kfree(app);
}

int bnxt_re_hdbr_init(struct bnxt_re_dev *rdev)
{
	struct bnxt_re_hdbr_app *drv;

	/* HDBR init for normal apps */
	INIT_LIST_HEAD(&rdev->hdbr_apps);

	if (rdev->en_dev->hdbr_info->hdbr_enabled) {
		rdev->hdbr_enabled = true;
		rdev->chip_ctx->modes.hdbr_enabled = true;
	} else {
		rdev->hdbr_enabled = false;
		return 0;
	}

	/* Init free page list */
	mutex_init(&rdev->hdbr_fpg_lock);
	INIT_LIST_HEAD(&rdev->hdbr_fpgs);

	rdev->hdbr_wq = create_singlethread_workqueue("bnxt_re_hdbr_wq");
	if (!rdev->hdbr_wq)
		return -ENOMEM;

	mutex_init(&rdev->hdbr_lock);
	rdev->hdbr_dt = &rdev->en_dev->hdbr_info->debug_trace;

	/* HDBR init for driver app */
	drv = bnxt_re_hdbr_alloc_app(rdev, false);
	if (!drv) {
		destroy_workqueue(rdev->hdbr_wq);
		rdev->hdbr_wq = NULL;
		return -ENOMEM;
	}
	rdev->hdbr_privileged = &drv->lst;

	return 0;
}

void bnxt_re_hdbr_uninit(struct bnxt_re_dev *rdev)
{
	struct bnxt_re_hdbr_app *app;
	struct list_head *head;
	struct hdbr_pg *pg;

	if (!rdev->hdbr_enabled)
		return;

	/* Uninitialize normal apps */
	mutex_lock(&rdev->hdbr_lock);
	head = &rdev->hdbr_apps;
	while (!list_empty(head)) {
		app = list_first_entry(head, struct bnxt_re_hdbr_app, lst);
		list_del(&app->lst);
		bnxt_re_hdbr_dealloc_app(rdev, app);
	}
	mutex_unlock(&rdev->hdbr_lock);

	/* Uninitialize driver app */
	if (rdev->hdbr_privileged) {
		app = container_of(rdev->hdbr_privileged, struct bnxt_re_hdbr_app, lst);
		bnxt_re_hdbr_dealloc_app(rdev, app);
		rdev->hdbr_privileged = NULL;
	}

	if (rdev->hdbr_wq) {
		flush_workqueue(rdev->hdbr_wq);
		destroy_workqueue(rdev->hdbr_wq);
		rdev->hdbr_wq = NULL;
	}

	/*
	 * At this point, all app pages are flushed into free page list.
	 * Dealloc all free pages.
	 */
	mutex_lock(&rdev->hdbr_fpg_lock);
	head = &rdev->hdbr_fpgs;
	while (!list_empty(head)) {
		pg = list_first_entry(head, struct hdbr_pg, pg_node);
		list_del(&pg->pg_node);
		dma_free_coherent(&rdev->en_dev->pdev->dev, PAGE_SIZE_4K, pg->kptr, pg->da);
		kfree(pg);
	}
	mutex_unlock(&rdev->hdbr_fpg_lock);
}

static void bnxt_re_hdbr_pages_dump(struct hdbr_pg_lst *plst)
{
	struct hdbr_pg *pg;
	__le64 *dbc_ptr;
	int i, cnt = 0;

	mutex_lock(&plst->lst_lock);
	list_for_each_entry(pg, &plst->pg_head, pg_node) {
		pr_info("page cnt    = %d\n", cnt);
		pr_info("kptr        = 0x%016llX\n", (u64)pg->kptr);
		pr_info("dma         = 0x%016llX\n", pg->da);
		pr_info("grp_size    = %d\n", pg->grp_size);
		pr_info("first_avail = %d\n", pg->first_avail);
		pr_info("first_empty = %d\n", pg->first_empty);
		pr_info("blk_avail   = %d\n", pg->blk_avail);
		pr_info("ktbl_idx    = %d\n", pg->ktbl_idx);
		dbc_ptr = pg->kptr;
		if (!dbc_ptr) {
			pr_info("Page content not available\n");
			break;
		}
		for (i = 0; i < 512; i++) {
			if (i > 0 && i < 511 && !dbc_ptr[i])
				continue;
			pr_info("page[%d][%3d] 0x%016llX\n", cnt, i, le64_to_cpu(dbc_ptr[i]));
		}
		cnt++;
	}
	mutex_unlock(&plst->lst_lock);
}

char *bnxt_re_hdbr_user_dump(struct bnxt_re_dev *rdev, int group)
{
	struct list_head *head = &rdev->hdbr_apps;
	struct bnxt_re_hdbr_app *app;
	int cnt = 0;
	char *buf;

	mutex_lock(&rdev->hdbr_lock);
	list_for_each_entry(app, head, lst)
		cnt++;
	buf = kasprintf(GFP_KERNEL, "Total apps    = %d\n", cnt);

	/* Page content dump to dmesg console */
	pr_info("====== Dumping %s user apps DB copy page info ======\n%s", rdev->dev_name, buf);
	cnt = 0;
	list_for_each_entry(app, head, lst) {
		struct hdbr_pg_lst *plst;

		plst = &app->pg_lst[group];
		pr_info("App cnt       = %d\n", cnt);
		pr_info("group         = %d\n", plst->group);
		pr_info("blk_avail     = %d\n",	plst->blk_avail);
		bnxt_re_hdbr_pages_dump(plst);
		cnt++;
	}
	mutex_unlock(&rdev->hdbr_lock);

	return buf;
}

char *bnxt_re_hdbr_driver_dump(char *dev_name, struct list_head *head, int group)
{
	struct bnxt_re_hdbr_app *app;
	struct hdbr_pg_lst *plst;
	char *buf;

	app = container_of(head, struct bnxt_re_hdbr_app, lst);
	plst = &app->pg_lst[group];

	/* Structure data to debugfs console */
	buf = kasprintf(GFP_KERNEL,
			"group         = %d\n"
			"blk_avail     = %d\n",
			plst->group,
			plst->blk_avail);

	/* Page content dump to dmesg console */
	pr_info("====== Dumping %s driver DB copy page info ======\n%s", dev_name, buf);
	bnxt_re_hdbr_pages_dump(plst);

	return buf;
}

char *bnxt_re_hdbr_dump(struct bnxt_re_dev *rdev, int group, bool user)
{
	struct list_head *lst;

	if (user) {
		lst = &rdev->hdbr_apps;
		if (list_empty(lst))
			goto no_data;
		return bnxt_re_hdbr_user_dump(rdev, group);
	}

	lst = rdev->hdbr_privileged;
	if (!lst)
		goto no_data;
	return bnxt_re_hdbr_driver_dump(rdev->dev_name, lst, group);

no_data:
	return kasprintf(GFP_KERNEL, "No data available!\n");
}
