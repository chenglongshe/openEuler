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

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_hdbr.h"

/*
 * Map DB type to DB copy group type
 */
int bnxt_hdbr_get_grp(u64 db_val)
{
	db_val &= DBC_TYPE_MASK;

	switch (db_val) {
	case DBR_TYPE_SQ:
		return DBC_GROUP_SQ;

	case DBR_TYPE_RQ:
		return DBC_GROUP_RQ;

	case DBR_TYPE_SRQ:
	case DBR_TYPE_SRQ_ARM:
	case DBR_TYPE_SRQ_ARMENA:
		return DBC_GROUP_SRQ;

	case DBR_TYPE_CQ:
	case DBR_TYPE_CQ_ARMSE:
	case DBR_TYPE_CQ_ARMALL:
	case DBR_TYPE_CQ_ARMENA:
	case DBR_TYPE_CQ_CUTOFF_ACK:
		return DBC_GROUP_CQ;

	default:
		break;
	}

	return DBC_GROUP_MAX;
}

/*
 * Caller of this function is debugfs knob. It dumps the kernel memory table
 * main structure value to caller.
 * Additionally, dump page content to dmesg. Since we may have many pages, it
 * is too large to output to debugfs.
 */
char *bnxt_hdbr_ktbl_dump(struct bnxt_hdbr_ktbl *ktbl)
{
	struct bnxt_hdbr_kslt *slot;
	char *buf;
	int i, j;

	if (!ktbl) {
		buf = kasprintf(GFP_KERNEL, "ktbl is NULL\n");
		return buf;
	}

	/* Structure data to debugfs console */
	buf = kasprintf(GFP_KERNEL,
			"group_type    = %d\n"
			"first_avail   = %d\n"
			"first_empty   = %d\n"
			"last_entry    = %d\n"
			"slot_avail    = %d\n"
			"num_4k_pages  = %d\n"
			"daddr         = 0x%016llX\n"
			"link_slot     = 0x%016llX\n",
			ktbl->group_type,
			ktbl->first_avail,
			ktbl->first_empty,
			ktbl->last_entry,
			ktbl->slot_avail,
			ktbl->num_4k_pages,
			ktbl->daddr,
			(u64)ktbl->link_slot);

	/* Page content dump to dmesg console */
	pr_info("====== Dumping ktbl info ======\n%s", buf);
	for (i = 0; i < ktbl->num_4k_pages; i++) {
		slot = ktbl->pages[i];
		pr_info("ktbl->pages[%d]: 0x%016llX\n", i, (u64)slot);
		for (j = 0; j < 256; j++) {
			if (j && j < 255 && !slot[j].flags && !slot[j].memptr)
				continue;
			pr_info("pages[%2d][%3d], 0x%016llX, 0x%016llX\n",
				i, j, le64_to_cpu(slot[j].flags),
				le64_to_cpu(slot[j].memptr));
		}
	}

	return buf;
}

/*
 * This function is called during L2 driver context memory allocation time.
 * It is on the path of nic open.
 * The initialization is allocating the memory for main data structure and
 * setup initial values.
 * pg_ptr and da are pointing to the first page allocated in
 * bnxt_setup_ctxm_pg_tbls()
 */
int bnxt_hdbr_ktbl_init(struct bnxt *bp, int group, void *pg_ptr, dma_addr_t da)
{
	struct bnxt_hdbr_ktbl *ktbl;
	int i;

	ktbl = kzalloc(sizeof(*ktbl), GFP_KERNEL);
	if (!ktbl)
		return -ENOMEM;

	memset(pg_ptr, 0, PAGE_SIZE_4K);
	ktbl->pdev = bp->pdev;
	spin_lock_init(&ktbl->hdbr_kmem_lock);
	ktbl->group_type = group;
	ktbl->first_avail = 0;
	ktbl->first_empty = 0;
	ktbl->last_entry = -1; /* There isn't last entry at first */
	ktbl->slot_avail = NSLOT_PER_4K_PAGE;
	ktbl->num_4k_pages = 1;
	ktbl->pages[0] = pg_ptr;
	ktbl->daddr = da;
	ktbl->link_slot = pg_ptr + PAGE_SIZE_4K - DBC_KERNEL_ENTRY_SIZE;
	for (i = 1; i < ktbl->num_4k_pages; i++) {
		pg_ptr += PAGE_SIZE_4K;
		ktbl->pages[i] = pg_ptr;
		da += PAGE_SIZE_4K;
		bnxt_hdbr_set_link(ktbl->link_slot, da);
		ktbl->link_slot += PAGE_SIZE_4K;
	}

	/* Link to main bnxt structure */
	bp->hdbr_info.ktbl[group] = ktbl;

	return 0;
}

/*
 * This function is called during L2 driver context memory free time. It is on
 * the path of nic close.
 */
void bnxt_hdbr_ktbl_uninit(struct bnxt *bp, int group)
{
	struct bnxt_hdbr_ktbl *ktbl;
	struct bnxt_hdbr_kslt *slot;
	dma_addr_t da;
	void *ptr;
	int i;

	/* Tear off from bp structure first */
	ktbl = bp->hdbr_info.ktbl[group];
	bp->hdbr_info.ktbl[group] = NULL;
	if (!ktbl)
		return;

	/* Free attached pages(first page will be freed by bnxt_free_ctx_pg_tbls() */
	for (i = ktbl->num_4k_pages - 1; i >= 1; i--) {
		ptr = ktbl->pages[i];
		slot = ktbl->pages[i - 1] + PAGE_SIZE_4K - DBC_KERNEL_ENTRY_SIZE;
		da = (dma_addr_t)le64_to_cpu(slot->memptr);
		dma_free_coherent(&bp->pdev->dev, PAGE_SIZE_4K, ptr, da);
	}

	/* Free the control structure at last */
	kfree(ktbl);
}

/*
 * This function is called when dbnxt_hdbr_reg_apg() run out of memory slots.
 * hdbr_kmem_lock is held in caller, so it is safe to alter the kernel page
 * chain.
 */
static int bnxt_hdbr_alloc_ktbl_pg(struct bnxt_hdbr_ktbl *ktbl)
{
	dma_addr_t da;
	void *ptr;

	/* Development stage guard */
	if (ktbl->num_4k_pages >= MAX_KMEM_4K_PAGES) {
		pr_err("Must fix: need more than MAX_KMEM_4K_PAGES\n");
		return -ENOMEM;
	}

	/* Alloc one page */
	ptr = dma_alloc_coherent(&ktbl->pdev->dev, PAGE_SIZE_4K, &da, GFP_KERNEL | __GFP_ZERO);
	if (!ptr)
		return -ENOMEM;

	/* Chain up with existing pages */
	ktbl->pages[ktbl->num_4k_pages] = ptr;
	bnxt_hdbr_set_link(ktbl->link_slot, da);
	ktbl->link_slot = ptr + PAGE_SIZE_4K - DBC_KERNEL_ENTRY_SIZE;
	ktbl->num_4k_pages += 1;
	ktbl->slot_avail += NSLOT_PER_4K_PAGE;

	return 0;
}

/*
 * This function is called when L2 driver, RoCE driver or RoCE driver on
 * behalf of rocelib need to register its application memory page.
 * Each application memory page is linked in kernel memory table with a
 * 16 bytes memory slot.
 */
int bnxt_hdbr_reg_apg(struct bnxt_hdbr_ktbl *ktbl, dma_addr_t ap_da, int *idx, u16 pi)
{
	struct bnxt_hdbr_kslt *slot;
	int rc = 0;

	spin_lock(&ktbl->hdbr_kmem_lock);

	/* Add into kernel talbe */
	if (ktbl->slot_avail == 0) {
		rc = bnxt_hdbr_alloc_ktbl_pg(ktbl);
		if (rc)
			goto exit;
	}

	/* Fill up the new entry */
	slot = get_slot(ktbl, ktbl->first_avail);
	bnxt_hdbr_set_slot(slot, ap_da, pi, ktbl->first_avail == ktbl->first_empty);
	*idx = ktbl->first_avail;
	ktbl->slot_avail--;

	/* Clear last flag of previous and advance first_avail index */
	if (ktbl->first_avail == ktbl->first_empty) {
		if (ktbl->last_entry >= 0) {
			slot = get_slot(ktbl, ktbl->last_entry);
			slot->flags &= cpu_to_le64(~FLAG_BIT_LAST);
		}
		ktbl->last_entry = ktbl->first_avail;
		ktbl->first_avail++;
		ktbl->first_empty++;
	} else {
		while (++ktbl->first_avail < ktbl->first_empty) {
			slot = get_slot(ktbl, ktbl->first_avail);
			if (slot->flags & cpu_to_le64(FLAG_BIT_VALID))
				continue;
			break;
		}
	}

exit:
	spin_unlock(&ktbl->hdbr_kmem_lock);
	return rc;
}
EXPORT_SYMBOL(bnxt_hdbr_reg_apg);

/*
 * This function is called when L2 driver, RoCE driver or RoCE driver on
 * behalf of rocelib need to unregister its application memory page.
 * The corresponding memory slot need to be cleared.
 * Kernel memory table will reuse that slot for later application page.
 */
void bnxt_hdbr_unreg_apg(struct bnxt_hdbr_ktbl *ktbl, int idx)
{
	struct bnxt_hdbr_kslt *slot;

	spin_lock(&ktbl->hdbr_kmem_lock);
	if (idx == ktbl->last_entry) {
		/* Find the new last_entry index, and mark last */
		while (--ktbl->last_entry >= 0) {
			slot = get_slot(ktbl, ktbl->last_entry);
			if (slot->flags & cpu_to_le64(FLAG_BIT_VALID))
				break;
		}
		if (ktbl->last_entry >= 0) {
			slot = get_slot(ktbl, ktbl->last_entry);
			slot->flags |= cpu_to_le64(FLAG_BIT_LAST);
		}
	}

	/* unregister app page entry */
	bnxt_hdbr_clear_slot(get_slot(ktbl, idx));

	/* update first_avail index to lower possible */
	if (idx < ktbl->first_avail)
		ktbl->first_avail = idx;
	ktbl->slot_avail++;
	spin_unlock(&ktbl->hdbr_kmem_lock);
}
EXPORT_SYMBOL(bnxt_hdbr_unreg_apg);

/*
 * Map L2 ring type to DB copy group type
 */
int bnxt_hdbr_r2g(u32 ring_type)
{
	switch (ring_type) {
	case HWRM_RING_ALLOC_TX:
		return DBC_GROUP_SQ;

	case HWRM_RING_ALLOC_RX:
	case HWRM_RING_ALLOC_AGG:
		return DBC_GROUP_SRQ;

	case HWRM_RING_ALLOC_CMPL:
		return DBC_GROUP_CQ;

	default:
		break;
	}

	return DBC_GROUP_MAX;
}

/*
 * This function is called during L2 driver context memory allocation time.
 * It is on the path of nic open.
 * The initialization is allocating one page per group as L2 driver's
 * doorbell copy memory region.
 * Initial study shows that one page would be big enough to hold one group
 * type of doorbell copies for L2. (512 Tx/Rx rings or 170 CQ rings).
 * Not like the RoCE DB, L2 driver DB are allocated at NIC open, no dynamic
 * change during NIC up time.
 *
 * Inside L2 DB copy app page, DBs are grouped by group type.
 *     DBC_GROUP_SQ  : grp_size = 1,
 *		       offset 0: SQ producer index doorbell
 *     DBC_GROUP_RQ  : grp_size = 1,
 *		       offset 0: (L2 doesn't use this group.)
 *     DBC_GROUP_SRQ : grp_size = 1,
 *		       offset 0: SRQ producer index doorbell
 *     DBC_GROUP_CQ  : grp_size = 3,
 *		       offset 0: CQ consumer index doorbell
 *		       offset 1: CQ_ARMALL/CQ_ARMASE (share slot)
 *		       offset 2: CUTOFF_ACK
 */
int bnxt_hdbr_l2_init(struct bnxt *bp, int group)
{
	struct bnxt_hdbr_l2_pg *app_pg;
	int rc;

	app_pg = kzalloc(sizeof(*app_pg), GFP_KERNEL);
	if (!app_pg)
		return -ENOMEM;

	app_pg->ptr = dma_alloc_coherent(&bp->pdev->dev, PAGE_SIZE_4K, &app_pg->da,
					 GFP_KERNEL | __GFP_ZERO);
	if (!app_pg->ptr) {
		kfree(app_pg);
		return -ENOMEM;
	}
	app_pg->grp_size = (group == DBC_GROUP_CQ) ? 3 : 1;
	app_pg->size = PAGE_SIZE_4K / HDBR_DB_SIZE / app_pg->grp_size;
	app_pg->first_avail = 0;
	app_pg->first_empty = 0;
	app_pg->ptr[0] = cpu_to_le64(DBC_VALUE_LAST);
	app_pg->blk_avail = app_pg->size;
	spin_lock_init(&app_pg->pg_lock);

	/* Register to kernel table */
	rc = bnxt_hdbr_reg_apg(bp->hdbr_info.ktbl[group], app_pg->da, &app_pg->ktbl_idx, 0);
	if (rc) {
		dma_free_coherent(&bp->pdev->dev, PAGE_SIZE_4K, app_pg->ptr, app_pg->da);
		kfree(app_pg);
		return rc;
	}

	/* Link to main bnxt structure */
	bp->hdbr_pg[group] = app_pg;

	return 0;
}

/*
 * This function is called during L2 driver context memory free time. It is on
 * the path of nic close.
 */
void bnxt_hdbr_l2_uninit(struct bnxt *bp, int group)
{
	struct bnxt_hdbr_l2_pg *pg;

	/* Cut off from main structure */
	pg = bp->hdbr_pg[group];
	bp->hdbr_pg[group] = NULL;

	if (!pg)
		return;

	/* Unregister from kernel table */
	bnxt_hdbr_unreg_apg(bp->hdbr_info.ktbl[group], pg->ktbl_idx);

	/* Free memory up */
	dma_free_coherent(&bp->pdev->dev, PAGE_SIZE_4K, pg->ptr, pg->da);
	kfree(pg);
}

/*
 * This function is called when a new db is created.
 * It finds a memoty slot in the DB copy application page, and return the
 * address.
 * Not all DB type need a copy, for those DB types don't need a copy, we
 * simply return NULL.
 */
__le64 *bnxt_hdbr_reg_db(struct bnxt *bp, int group)
{
	struct bnxt_hdbr_l2_pg *pg;
	int i, n, idx;

	if (group >= DBC_GROUP_MAX)
		return NULL;

	pg = bp->hdbr_pg[group];
	if (!pg)
		return NULL;

	/* TODO: If one page is not enough */
	if (pg->blk_avail <= 0) {
		dev_err_once(&bp->pdev->dev, "No slot available for DB copy\n");
		return NULL;
	}

	spin_lock(&pg->pg_lock);

	n = pg->grp_size;
	idx = pg->first_avail * n; /* This is what we'll return */
	for (i = 0; i < n; i++)
		pg->ptr[idx + i] = cpu_to_le64(DBC_VALUE_INIT);
	pg->blk_avail--;

	/* Update indice for next reg */
	if (pg->first_avail == pg->first_empty) {
		pg->first_avail++;
		pg->first_empty++;
		if (pg->first_empty < pg->size)
			pg->ptr[pg->first_empty * n] = cpu_to_le64(DBC_VALUE_LAST);
	} else {
		while (++pg->first_avail < pg->first_empty) {
			if (!pg->ptr[pg->first_avail * n])
				break;
		}
	}

	spin_unlock(&pg->pg_lock);

	return &pg->ptr[idx];
}

/*
 * This function is called when a DB is destroyed. It free up the memoty slot,
 * and slot will be reused later.
 * Note: In L2 driver, we actually don't destroy DB one by one. Instead, DBs are
 * destroyed along with ring freed.
 * See function bnxt_hdbr_reset_l2pgs().
 */
void bnxt_hdbr_unreg_db(struct bnxt *bp, int group, __le64 *db)
{
	struct bnxt_hdbr_l2_pg *pg = bp->hdbr_pg[group];
	int i, n, pos;

	if (!pg)
		return;

	n = pg->grp_size;
	pos = ((u64)db - (u64)pg->ptr) / HDBR_DB_SIZE / n;

	if (pos >= pg->size) {
		dev_err(&bp->pdev->dev, "DB copy index %d is out boundary %llu\n",
			pos, pg->size);
		return;
	}

	spin_lock(&pg->pg_lock);

	for (i = 0; i < n; i++)
		pg->ptr[pos * n + i] = 0;
	pg->blk_avail++;
	if (pos < pg->first_avail)
		pg->first_avail = pos;

	spin_unlock(&pg->pg_lock);
}

/*
 * This function is called when all L2 rings are freed.
 * Driver is still running, but rings are freed, so that all DB copy slots should be
 * reclaimed for later newly created rings' DB.
 */
void bnxt_hdbr_reset_l2pgs(struct bnxt *bp)
{
	struct bnxt_hdbr_l2_pg *pg;
	int group;

	for (group = DBC_GROUP_SQ; group < DBC_GROUP_MAX; group++) {
		pg = bp->hdbr_pg[group];
		if (!pg)
			continue;

		spin_lock(&pg->pg_lock);
		pg->first_avail = 0;
		pg->first_empty = 0;
		memset(pg->ptr, 0, PAGE_SIZE_4K);
		pg->ptr[0] = cpu_to_le64(DBC_VALUE_LAST);
		pg->blk_avail = pg->size;
		spin_unlock(&pg->pg_lock);
	}
}

/*
 * Caller of this function is debugfs knob. It dumps the main structure value
 * of L2 driver DB copy region to caller.
 * Additionally, dump page content to dmesg. Since we may have many pages, it
 * is too large to output to debugfs.
 */
char *bnxt_hdbr_l2pg_dump(struct bnxt_hdbr_l2_pg *app_pg)
{
	char *buf;
	int i;

	if (!app_pg) {
		buf = kasprintf(GFP_KERNEL, "app_pg is NULL\n");
		return buf;
	}

	/* Structure data to debugfs console */
	buf = kasprintf(GFP_KERNEL,
			"kernel addr   = 0x%016llX\n"
			"dma addr      = 0x%016llX\n"
			"size          = %llu\n"
			"grp_size      = %d\n"
			"first_avail   = %d\n"
			"first_empty   = %d\n"
			"blk_avail     = %d\n"
			"Kernel index  = %d\n",
			(u64)app_pg->ptr,
			app_pg->da,
			app_pg->size,
			app_pg->grp_size,
			app_pg->first_avail,
			app_pg->first_empty,
			app_pg->blk_avail,
			app_pg->ktbl_idx);

	/* Page content dump to dmesg console */
	pr_info("====== Dumping page info ======\n%s", buf);
	for (i = 0; i < 512; i++) {
		if (i && i < 511 && !app_pg->ptr[i])
			continue;
		pr_info("page[0][%3d] 0x%016llX\n", i, le64_to_cpu(app_pg->ptr[i]));
	}

	return buf;
}
