// SPDX-License-Identifier: GPL-2.0+
/*
 * Support cgroup for xpu device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
 *
 * Author: Konstantin Meskhidze <konstantin.meskhidze@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/err.h>
#include <linux/cgroup.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/xsched.h>

enum xcu_file_type {
	XCU_FILE_PERIOD_MS,
	XCU_FILE_QUOTA_MS,
	XCU_FILE_SHARES,
};

static struct xsched_group root_xsched_group;
struct xsched_group *root_xcg = &root_xsched_group;
static bool root_cg_inited;

static struct xsched_group *old_xcg;
static DECLARE_WAIT_QUEUE_HEAD(xcg_attach_wq);
static bool attach_in_progress;
static DEFINE_MUTEX(xcg_mutex);

static const char xcu_sched_name[XSCHED_TYPE_NUM][4] = {
	[XSCHED_TYPE_RT] = "rt",
	[XSCHED_TYPE_CFS] = "cfs"
};

void xcu_cg_init_common(struct xsched_group *xcg)
{
	spin_lock_init(&xcg->lock);
	INIT_LIST_HEAD(&xcg->members);
	INIT_LIST_HEAD(&xcg->children_groups);
}

static void xcu_cfs_root_cg_init(void)
{
	int id;
	struct xsched_cu *xcu;

	for_each_active_xcu(xcu, id) {
		root_xcg->perxcu_priv[id].xcu_id = id;
		root_xcg->perxcu_priv[id].self = root_xcg;
		root_xcg->perxcu_priv[id].cfs_rq = &xcu->xrq.cfs;
		root_xcg->perxcu_priv[id].xse.cfs.weight = 1;
	}

	root_xcg->sched_type = XSCHED_TYPE_DFLT;
}

/**
 * xcu_cfs_cg_init() - Initialize xsched_group cfs runqueues and bw control.
 * @xcg: new xsched_cgroup
 * @parent_xg: parent's group
 *
 * One xsched_group can host many processes with contexts on different devices.
 * Function creates xsched_entity for every XCU, and places it in runqueue
 * of parent group. Create new cfs rq for xse inside group.
 */
static int xcu_cfs_cg_init(struct xsched_group *xcg,
				struct xsched_group *parent_xg)
{
	int id = 0, err, i;
	struct xsched_cu *xcu;
	struct xsched_rq_cfs *sub_cfs_rq;

	if (unlikely(!root_cg_inited)) {
		xcu_cfs_root_cg_init();
		root_cg_inited = true;
	}

	for_each_active_xcu(xcu, id) {
		xcg->perxcu_priv[id].xcu_id = id;
		xcg->perxcu_priv[id].self = xcg;

		sub_cfs_rq = kzalloc(sizeof(struct xsched_rq_cfs), GFP_KERNEL);
		if (!sub_cfs_rq) {
			XSCHED_ERR("Fail to alloc cfs runqueue on xcu %d\n", id);
			err = -ENOMEM;
			goto alloc_error;
		}
		xcg->perxcu_priv[id].cfs_rq = sub_cfs_rq;
		xcg->perxcu_priv[id].cfs_rq->ctx_timeline = RB_ROOT_CACHED;

		xcg->perxcu_priv[id].xse.is_group = true;
		xcg->perxcu_priv[id].xse.xcu = xcu;
		xcg->perxcu_priv[id].xse.class = &fair_xsched_class;

		/* Put new empty groups to the right in parent's rbtree: */
		xcg->perxcu_priv[id].xse.cfs.xruntime = XSCHED_TIME_INF;
		xcg->perxcu_priv[id].xse.cfs.weight =
			XSCHED_CFS_ENTITY_WEIGHT_DFLT;
		xcg->perxcu_priv[id].xse.parent_grp = parent_xg;

		mutex_lock(&xcu->xcu_lock);
		enqueue_ctx(&xcg->perxcu_priv[id].xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
	}

	xcg->shares_cfg = XSCHED_CFG_SHARE_DFLT;
	xcu_grp_shares_update(parent_xg);

	return 0;

alloc_error:
	for (i = 0; i < id; i++)
		kfree(xcg->perxcu_priv[i].cfs_rq);
	return err;
}

static void xcu_cfs_cg_deinit(struct xsched_group *xcg)
{
	uint32_t id;
	struct xsched_cu *xcu;

	for_each_active_xcu(xcu, id) {
		mutex_lock(&xcu->xcu_lock);
		dequeue_ctx(&xcg->perxcu_priv[id].xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
		kfree(xcg->perxcu_priv[id].cfs_rq);
	}
	xcu_grp_shares_update(xcg->parent);
}

/**
 * xcu_cg_init() - Initialize non-root xsched_group structure.
 * @xcg: new xsched_cgroup
 * @parent_xg: parent's group
 */
static int xcu_cg_init(struct xsched_group *xcg,
				struct xsched_group *parent_xg)
{
	xcu_cg_init_common(xcg);
	xcg->parent = parent_xg;
	list_add_tail(&xcg->group_node, &parent_xg->children_groups);
	xcg->sched_type = parent_xg->sched_type;

	switch (xcg->sched_type) {
	case XSCHED_TYPE_CFS:
		return xcu_cfs_cg_init(xcg, parent_xg);
	default:
		pr_info("xcu_cgroup: init RT group css=0x%lx\n",
		       (uintptr_t)&xcg->css);
		break;
	}

	return 0;
}

inline struct xsched_group *xcu_cg_from_css(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct xsched_group, css) : NULL;
}

/**
 * xcu_css_alloc() - Allocate and init xcu cgroup.
 * @parent_css: css of parent xcu cgroup
 *
 * Called from kernel/cgroup.c with cgroup_lock() held.
 * First called in subsys initialization to create root xcu cgroup, when
 * XCUs haven't been initialized yet. Func used on every new cgroup creation,
 * on second call to set root xsched_group runqueue.
 *
 * Return: pointer of new xcu cgroup css on success, -ENOMEM otherwise.
 */
static struct cgroup_subsys_state *
xcu_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct xsched_group *parent_xg;
	struct xsched_group *xg;
	int err;

	if (!parent_css)
		return &root_xsched_group.css;

	xg = kzalloc(sizeof(*xg), GFP_KERNEL);
	if (!xg)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&xcg_mutex);
	parent_xg = xcu_cg_from_css(parent_css);
	err = xcu_cg_init(xg, parent_xg);
	mutex_unlock(&xcg_mutex);
	if (err) {
		kfree(xg);
		XSCHED_ERR("Fail to alloc new xcu group %s\n", __func__);
		return ERR_PTR(err);
	}

	return &xg->css;
}

static void xcu_css_free(struct cgroup_subsys_state *css)
{
	struct xsched_group *xcg;

	mutex_lock(&xcg_mutex);
	xcg = xcu_cg_from_css(css);
	if (xcg->parent != NULL) {
		switch (xcg->sched_type) {
		case XSCHED_TYPE_CFS:
			xcu_cfs_cg_deinit(xcg);
			break;
		default:
			pr_info("xcu_cgroup: deinit RT group css=0x%lx\n",
			       (uintptr_t)&xcg->css);
			break;
		}
	}
	list_del(&xcg->group_node);
	mutex_unlock(&xcg_mutex);

	kfree(xcg);
}

int xcu_css_online(struct cgroup_subsys_state *css)
{
	return 0;
}

static void xcu_css_offline(struct cgroup_subsys_state *css)
{
	;
}

static void xsched_group_xse_attach(struct xsched_group *xg,
				struct xsched_entity *xse)
{
	spin_lock(&xg->lock);
	list_add_tail(&xse->group_node, &xg->members);
	spin_unlock(&xg->lock);
	xse->parent_grp = xg;
}

void xsched_group_xse_detach(struct xsched_entity *xse)
{
	struct xsched_group *xcg = xse->parent_grp;

	spin_lock(&xcg->lock);
	list_del(&xse->group_node);
	spin_unlock(&xcg->lock);
}

static int xcu_task_can_attach(struct task_struct *task,
			struct xsched_group *old, struct xsched_group *dst)
{
	struct xsched_entity *xse;
	bool has_xse = false;

	spin_lock(&old->lock);
	list_for_each_entry(xse, &old->members, group_node) {
		if (xse->owner_pid == task_pid_nr(task)) {
			has_xse = true;
			break;
		}
	}
	spin_unlock(&old->lock);

	return has_xse ? -EINVAL : 0;
}

static int xcu_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css, *old_css;
	struct xsched_group *dst_xcg;
	int ret = 0;

	mutex_lock(&xcg_mutex);
	cgroup_taskset_for_each(task, dst_css, tset) {
		old_css = task_css(task, xcu_cgrp_id);
		dst_xcg = xcu_cg_from_css(dst_css);
		old_xcg = xcu_cg_from_css(old_css);
		ret = xcu_task_can_attach(task, old_xcg, dst_xcg);
		if (ret)
			break;
	}
	if (!ret)
		attach_in_progress = true;
	mutex_unlock(&xcg_mutex);
	return ret;
}

static void xcu_cancel_attach(struct cgroup_taskset *tset)
{
	mutex_lock(&xcg_mutex);
	attach_in_progress = false;
	wake_up(&xcg_attach_wq);
	mutex_unlock(&xcg_mutex);
}

void xcu_move_task(struct task_struct *task, struct xsched_group *old_xcg,
			struct xsched_group *new_xcg)
{
	struct xsched_entity *xse, *tmp;
	struct xsched_cu *xcu;

	spin_lock(&old_xcg->lock);
	list_for_each_entry_safe(xse, tmp, &old_xcg->members, group_node) {
		if (xse->owner_pid != task_pid_nr(task))
			continue;

		xcu = xse->xcu;
		BUG_ON(old_xcg != xse->parent_grp);

		/* delete from the old_xcg */
		list_del(&xse->group_node);

		mutex_lock(&xcu->xcu_lock);
		/* dequeue from the current runqueue */
		dequeue_ctx(xse, xcu);
		/* attach to the new_xcg */
		xsched_group_xse_attach(new_xcg, xse);
		/* enqueue to the runqueue in new_xcg */
		enqueue_ctx(xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
	}
	spin_unlock(&old_xcg->lock);
}

static void xcu_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *css;

	mutex_lock(&xcg_mutex);
	cgroup_taskset_for_each(task, css, tset) {
		xcu_move_task(task, old_xcg, xcu_cg_from_css(css));
	}
	attach_in_progress = false;
	wake_up(&xcg_attach_wq);
	mutex_unlock(&xcg_mutex);
}

/**
 * xsched_group_inherit() - Attach new entity to task's xsched_group.
 * @task: task_struct
 * @xse: xsched entity
 *
 * Called in xsched context initialization to attach xse to task's group
 * and inherit its xse scheduling class and bandwidth control policy.
 *
 * Return: Zero on success.
 */
int xsched_group_inherit(struct task_struct *task, struct xsched_entity *xse)
{
	struct cgroup_subsys_state *css;
	struct xsched_group *xg;

retry:
	wait_event(xcg_attach_wq, !attach_in_progress);

	mutex_lock(&xcg_mutex);
	if (attach_in_progress) {
		mutex_unlock(&xcg_mutex);
		goto retry;
	}
	xse->owner_pid = task_pid_nr(task);
	css = task_get_css(task, xcu_cgrp_id);
	xg = xcu_cg_from_css(css);
	xsched_group_xse_attach(xg, xse);
	css_put(css);
	mutex_unlock(&xcg_mutex);

	return 0;
}

static int xcu_sched_show(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct xsched_group *xg = xcu_cg_from_css(css);

	seq_printf(sf, "%s\n", xcu_sched_name[xg->sched_type]);
	return 0;
}

/**
 * xcu_cg_set_sched() - Set scheduling type for group.
 * @xg: xsched group
 * @type: scheduler type
 *
 * Scheduler type can be changed if task is child of root group
 * and haven't got scheduling entities.
 *
 * Return: Zero on success or -EINVAL
 */
int xcu_cg_set_sched(struct xsched_group *xg, int type)
{
	if (type == xg->sched_type)
		return 0;

	if (xg->parent != root_xcg)
		return -EINVAL;

	if (!list_empty(&xg->members))
		return -EBUSY;

	if (xg->sched_type == XSCHED_TYPE_CFS)
		xcu_cfs_cg_deinit(xg);

	xg->sched_type = type;
	if (type != XSCHED_TYPE_CFS)
		return 0;

	/* type is XSCHED_TYPE_CFS */
	return xcu_cfs_cg_init(xg, xg->parent);
}

static ssize_t xcu_sched_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of);
	struct xsched_group *xg = xcu_cg_from_css(css);
	char type_name[4];
	int type = -1;

	ssize_t ret = sscanf(buf, "%3s", type_name);

	if (ret < 1)
		return -EINVAL;

	for (type = 0; type < XSCHED_TYPE_NUM; type++) {
		if (!strcmp(type_name, xcu_sched_name[type]))
			break;
	}

	if (type == XSCHED_TYPE_NUM)
		return -EINVAL;

	if (!list_empty(&css->children))
		return -EBUSY;

	mutex_lock(&xcg_mutex);
	ret = xcu_cg_set_sched(xg, type);
	mutex_unlock(&xcg_mutex);

	return (ret) ? ret : nbytes;
}

static s64 xcu_read_s64(struct cgroup_subsys_state *css, struct cftype *cft)
{
	s64 ret = 0;
	struct xsched_group *xcucg = xcu_cg_from_css(css);

	spin_lock(&xcucg->lock);
	switch (cft->private) {
	case XCU_FILE_SHARES:
		ret = xcucg->shares_cfg;
		break;
	default:
		break;
	}
	spin_unlock(&xcucg->lock);
	return ret;
}

static inline u64 gcd(u64 a, u64 b)
{
	while (a != 0 && b != 0) {
		if (a > b)
			a %= b;
		else
			b %= a;
	}
	return (a) ? a : b;
}

void xcu_grp_shares_update(struct xsched_group *xg)
{
	int id;
	struct xsched_cu *xcu;
	struct xsched_group *xgi, *parent = xg;
	u64 sh_sum = 0, sh_gcd = 0, w_gcd = 0, sh_prod_red = 1;

	spin_lock(&parent->lock);
	list_for_each_entry((xgi), &(parent)->children_groups, group_node) {
		if ((xgi)->sched_type == XSCHED_TYPE_CFS)
			sh_gcd = gcd(sh_gcd, xgi->shares_cfg);
	}

	list_for_each_entry((xgi), &(parent)->children_groups, group_node) {
		if ((xgi)->sched_type == XSCHED_TYPE_CFS) {
			sh_sum += xgi->shares_cfg;
			xgi->shares_cfg_red = div_u64(xgi->shares_cfg, sh_gcd);

			if ((sh_prod_red % xgi->shares_cfg_red) != 0)
				sh_prod_red *= xgi->shares_cfg_red;
		}
	}

	parent->children_shares_sum = sh_sum;
	list_for_each_entry((xgi), &(parent)->children_groups, group_node) {
		if ((xgi)->sched_type == XSCHED_TYPE_CFS) {
			xgi->weight = div_u64(sh_prod_red, xgi->shares_cfg_red);
			w_gcd = gcd(w_gcd, xgi->weight);
		}
	}

	list_for_each_entry((xgi), &(parent)->children_groups, group_node) {
		if ((xgi)->sched_type == XSCHED_TYPE_CFS) {
			xgi->weight = div_u64(xgi->weight, w_gcd);
			for_each_active_xcu(xcu, id) {
				mutex_lock(&xcu->xcu_lock);
				xgi->perxcu_priv[id].xse.cfs.weight = xgi->weight;
				mutex_unlock(&xcu->xcu_lock);
			}
		}
	}
	spin_unlock(&parent->lock);
}

static int xcu_write_s64(struct cgroup_subsys_state *css, struct cftype *cft,
			s64 val)
{
	int ret = 0;
	struct xsched_group *xcucg = xcu_cg_from_css(css);

	spin_lock(&xcucg->lock);
	switch (cft->private) {
	case XCU_FILE_SHARES:
		if (val <= 0) {
			ret = -EINVAL;
			break;
		}
		xcucg->shares_cfg = val;
		xcu_grp_shares_update(xcucg->parent);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	spin_unlock(&xcucg->lock);

	return ret;
}

static int xcu_stat(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct xsched_group *xcucg = xcu_cg_from_css(css);

	u64 nr_throttled = 0;
	u64 throttled_time = 0;
	u64 exec_runtime = 0;

	int xcu_id;
	struct xsched_cu *xcu;

	if (xcucg->sched_type == XSCHED_TYPE_RT) {
		seq_printf(sf, "RT group stat is not supported\n");
		return 0;
	}

	for_each_active_xcu(xcu, xcu_id) {
		nr_throttled += xcucg->perxcu_priv[xcu_id].nr_throttled;
		throttled_time += xcucg->perxcu_priv[xcu_id].throttled_time;
		exec_runtime +=
			xcucg->perxcu_priv[xcu_id].xse.cfs.sum_exec_runtime;
	}

	seq_printf(sf, "exec_runtime:	%llu\n", exec_runtime);
	seq_printf(sf, "shares cfg:	%llu/%llu x%u\n", xcucg->shares_cfg,
		   xcucg->parent->children_shares_sum, xcucg->weight);

	return 0;
}

static struct cftype xcu_cg_files[] = {
	{
		.name = "shares",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = xcu_read_s64,
		.write_s64 = xcu_write_s64,
		.private = XCU_FILE_SHARES,
	},
	{
		.name = "stat",
		.seq_show = xcu_stat,
	},
	{
		.name = "sched",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = xcu_sched_show,
		.write = xcu_sched_write,
	},
	{} /* terminate */
};

struct cgroup_subsys xcu_cgrp_subsys = {
	.css_alloc = xcu_css_alloc,
	.css_online = xcu_css_online,
	.css_offline = xcu_css_offline,
	.css_free = xcu_css_free,
	.can_attach = xcu_can_attach,
	.cancel_attach = xcu_cancel_attach,
	.attach = xcu_attach,
	.dfl_cftypes = xcu_cg_files,
	.legacy_cftypes = xcu_cg_files,
	.early_init = false,
};
