// SPDX-License-Identifier: GPL-2.0+
/*
 * Core kernel scheduler code for XPU device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
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
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>
#include <linux/xsched.h>
#include <uapi/linux/sched/types.h>

int num_active_xcu;
spinlock_t xcu_mgr_lock;
extern struct xsched_group *root_xcg;

/* Xsched XCU array and bitmask that represents which XCUs
 * are present and online.
 */
DECLARE_BITMAP(xcu_online_mask, XSCHED_NR_CUS);
struct xsched_cu *xsched_cu_mgr[XSCHED_NR_CUS];

/* Storage list for contexts. */
struct list_head xsched_ctx_list;
DEFINE_MUTEX(xsched_ctx_list_mutex);

static DEFINE_MUTEX(revmap_mutex);
static DEFINE_HASHTABLE(ctx_revmap, XCU_HASH_ORDER);

static void put_prev_ctx(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	lockdep_assert_held(&xcu->xcu_lock);

	xse->class->put_prev_ctx(xse);
	xse->last_exec_runtime = 0;
	atomic_set(&xse->submitted_one_kick, 0);
	XSCHED_DEBUG("Put current xse %d @ %s\n", xse->tgid, __func__);
}

static size_t select_work_def(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	int kick_count;
	struct vstream_info *vs;
	unsigned int sum_exec_time = 0;
	size_t kicks_submitted = 0;
	struct vstream_metadata *vsm;
	int not_empty;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("Before decrement XSE kick_count=%u @ %s\n",
		kick_count, __func__);

	if (kick_count == 0) {
		XSCHED_WARN("Try to select xse that has 0 kicks @ %s\n",
			__func__);
		return 0;
	}

	do {
		not_empty = 0;
		for_each_vstream_in_ctx(vs, xse->ctx) {
			spin_lock(&vs->stream_lock);
			vsm = xsched_vsm_fetch_first(vs);
			spin_unlock(&vs->stream_lock);
			if (vsm) {
				list_add_tail(&vsm->node, &xcu->vsm_list);

				sum_exec_time += vsm->exec_time;
				kicks_submitted++;
				xsched_dec_pending_kicks_xse(xse);
				XSCHED_DEBUG(
					"vs id = %d Kick submit exec_time %u sq_tail %u sqe_num %u sq_id %u @ %s\n",
					vs->id, vsm->exec_time, vsm->sq_tail,
					vsm->sqe_num, vsm->sq_id, __func__);
				not_empty++;
			}
		}
	} while ((sum_exec_time < XSCHED_CFS_MIN_TIMESLICE) && (not_empty));

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("After decrement XSE kick_count=%d @ %s\n",
		    kick_count, __func__);

	xse->total_scheduled += kicks_submitted;

	return kicks_submitted;
}

static struct xsched_entity *__raw_pick_next_ctx(struct xsched_cu *xcu)
{
	const struct xsched_class *class;
	struct xsched_entity *next = NULL;
	size_t scheduled;

	lockdep_assert_held(&xcu->xcu_lock);
	for_each_xsched_class(class) {
		next = class->pick_next_ctx(xcu);
		if (next) {
			scheduled = class->select_work ?
				class->select_work(xcu, next) : select_work_def(xcu, next);

			XSCHED_DEBUG("xse %d scheduled=%zu total=%zu @ %s\n",
				next->tgid, scheduled, next->total_scheduled, __func__);
			break;
		}
	}

	return next;
}

void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	lockdep_assert_held(&xcu->xcu_lock);

	if (xse_integrity_check(xse)) {
		XSCHED_ERR("Fail to check xse integrity @ %s\n", __func__);
		return;
	}

	if (!xse->on_rq) {
		xse->on_rq = true;
		xse->class->enqueue_ctx(xse, xcu);
		XSCHED_DEBUG("Enqueue xse %d @ %s\n", xse->tgid, __func__);
	}
}

void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	lockdep_assert_held(&xcu->xcu_lock);

	if (xse_integrity_check(xse)) {
		XSCHED_ERR("Fail to check xse integrity @ %s\n", __func__);
		return;
	}

	if (xse->on_rq) {
		xse->class->dequeue_ctx(xse);
		xse->on_rq = false;
		XSCHED_DEBUG("Dequeue xse %d @ %s\n", xse->tgid, __func__);
	}
}

static int delete_ctx(struct xsched_context *ctx)
{
	struct xsched_cu *xcu = ctx->xse.xcu;
	struct xsched_entity *curr_xse = xcu->xrq.curr_xse;
	struct xsched_entity *xse = &ctx->xse;

	if (xse_integrity_check(xse)) {
		XSCHED_ERR("Fail to check xse integrity @ %s\n", __func__);
		return -EINVAL;
	}

	if (!xse->xcu) {
		XSCHED_ERR("Try to delete ctx that is not attached to xcu @ %s\n",
			__func__);
		return -EINVAL;
	}

	/* Wait till context has been submitted. */
	while (atomic_read(&xse->kicks_pending_ctx_cnt)) {
		XSCHED_DEBUG("Deleting ctx %d, xse->kicks_pending_ctx_cnt=%d @ %s\n",
			xse->tgid, atomic_read(&xse->kicks_pending_ctx_cnt),
			__func__);
		usleep_range(100, 200);
	}

	if (atomic_read(&xse->kicks_pending_ctx_cnt)) {
		XSCHED_ERR("Deleting ctx %d that has pending kicks left @ %s\n",
			xse->tgid, __func__);
		return -EINVAL;
	}

	mutex_lock(&xcu->xcu_lock);
	if (curr_xse == xse)
		xcu->xrq.curr_xse = NULL;

	dequeue_ctx(xse, xcu);
	mutex_unlock(&xcu->xcu_lock);
	XSCHED_DEBUG("Deleting ctx %d, pending kicks left=%d @ %s\n", xse->tgid,
		atomic_read(&xse->kicks_pending_ctx_cnt), __func__);

	xsched_group_xse_detach(xse);

	return 0;
}

/* Frees a given vstream and also frees and dequeues it's context
 * if a given vstream is the last and only vstream attached to it's
 * corresponding context object.
 */
void xsched_task_free(struct kref *kref)
{
	struct xsched_context *ctx;
	vstream_info_t *vs, *tmp;

	ctx = container_of(kref, struct xsched_context, kref);

	/* Wait till xse dequeues */
	while (READ_ONCE(ctx->xse.on_rq))
		usleep_range(100, 200);

	mutex_lock(&xsched_ctx_list_mutex);
	list_for_each_entry_safe(vs, tmp, &ctx->vstream_list, ctx_node) {
		list_del(&vs->ctx_node);
		kfree(vs->data);
		kfree(vs);
	}

	delete_ctx(ctx);
	list_del(&ctx->ctx_node);
	mutex_unlock(&xsched_ctx_list_mutex);

	kfree(ctx);
}

int ctx_bind_to_xcu(vstream_info_t *vstream_info, struct xsched_context *ctx)
{
	struct ctx_devid_revmap_data *revmap_data;
	struct xsched_cu *xcu_found = NULL;
	uint32_t type = XCU_TYPE_XPU;

	/* Find XCU history. */
	hash_for_each_possible(ctx_revmap, revmap_data, hash_node,
				(unsigned long)ctx->dev_id) {
		if (revmap_data && revmap_data->group) {
			/* Bind ctx to group xcu.*/
			ctx->xse.xcu = revmap_data->group->xcu;
			return 0;
		}
	}

	revmap_data = kzalloc(sizeof(struct ctx_devid_revmap_data), GFP_KERNEL);
	if (revmap_data == NULL) {
		XSCHED_ERR("Revmap_data is NULL @ %s\n", __func__);
		return -ENOMEM;
	}

	xcu_found = xcu_find(&type, ctx->dev_id, vstream_info->channel_id);
	if (!xcu_found)
		return -EINVAL;

	/* Bind ctx to an XCU from channel group. */
	revmap_data->group = xcu_found->group;
	ctx->xse.xcu = xcu_found;
	vstream_info->xcu = xcu_found;
	revmap_data->dev_id = vstream_info->dev_id;
	XSCHED_DEBUG("Ctx bind to xcu %u @ %s\n", xcu_found->id, __func__);

	hash_add(ctx_revmap, &revmap_data->hash_node,
		 (unsigned long)ctx->dev_id);

	return 0;
}

int vstream_bind_to_xcu(vstream_info_t *vstream_info)
{
	struct xsched_cu *xcu_found = NULL;
	uint32_t type = XCU_TYPE_XPU;

	xcu_found = xcu_find(&type, vstream_info->dev_id, vstream_info->channel_id);
	if (!xcu_found)
		return -EINVAL;

	/* Bind vstream to a xcu. */
	vstream_info->xcu = xcu_found;
	XSCHED_DEBUG("XCU bound to a vstream: type=%u, dev_id=%u, chan_id=%u.\n",
		type, vstream_info->dev_id, vstream_info->channel_id);

	return 0;
}

struct xsched_cu *xcu_find(uint32_t *type,
				uint32_t dev_id, uint32_t channel_id)
{
	struct xcu_group *group = NULL;
	uint32_t local_type = *type;

	/* Find xcu by type. */
	group = xcu_group_find(xcu_group_root, local_type);
	if (group == NULL) {
		XSCHED_ERR("Fail to find type group.\n");
		return NULL;
	}

	/* Find device id group. */
	group = xcu_group_find(group, dev_id);
	if (group == NULL) {
		XSCHED_ERR("Fail to find device group.\n");
		return NULL;
	}
	/* Find channel id group. */
	group = xcu_group_find(group, channel_id);
	if (group == NULL) {
		XSCHED_ERR("Fail to find channel group.\n");
		return NULL;
	}

	*type = local_type;
	XSCHED_DEBUG("XCU found: type=%u, dev_id=%u, chan_id=%u.\n",
		local_type, dev_id, channel_id);

	return group->xcu;
}

int xsched_xse_set_class(struct xsched_entity *xse)
{
#ifdef CONFIG_CGROUP_XCU
	xsched_group_inherit(current, xse);
#endif
	switch (xse->parent_grp->sched_type) {
	case XSCHED_TYPE_RT:
		xse->class = &rt_xsched_class;
		XSCHED_DEBUG("Context is in RT class %s\n", __func__);
		break;
	case XSCHED_TYPE_CFS:
		xse->class = &fair_xsched_class;
		XSCHED_DEBUG("Context is in CFS class %s\n", __func__);
		break;
	default:
		XSCHED_ERR("Xse has incorrect class @ %s\n", __func__);
		return -EINVAL;
	}
	return 0;
}

int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs)
{
	int err = 0;
	struct xsched_entity *xse = &ctx->xse;

	atomic_set(&xse->kicks_pending_ctx_cnt, 0);
	atomic_set(&xse->submitted_one_kick, 0);

	xse->total_scheduled = 0;
	xse->total_submitted = 0;
	xse->last_exec_runtime = 0;
	xse->task_type = GET_VS_TASK_TYPE(vs);
	xse->fd = ctx->fd;
	xse->tgid = ctx->tgid;

	err = ctx_bind_to_xcu(vs, ctx);
	if (err) {
		XSCHED_ERR(
			"Couldn't find valid xcu for vstream %u dev_id %u @ %s\n",
			vs->id, vs->dev_id, __func__);
		return -EINVAL;
	}

	xse->ctx = ctx;
	if (likely(vs->xcu != NULL))
		xse->xcu = vs->xcu;

	err = xsched_xse_set_class(xse);
	if (err) {
		XSCHED_ERR("Failed to set xse class @ %s\n", __func__);
		return err;
	}

	if (xse_is_cfs(xse)) {
		xse->cfs.sum_exec_runtime = 0;
	}

	if (xse_is_rt(xse)) {
		xse->rt.state = XSE_PREPARE;
		xse->rt.flag = XSE_TIF_NONE;
		xse->rt.prio = GET_VS_TASK_PRIO_RT(vs);
		xse->rt.kick_slice = XSCHED_RT_KICK_SLICE;

		/* XSE priority is being decreased by 1 here because
		 * in libucc priority counter starts from 1 while in the
		 * kernel counter starts with 0.
		 *
		 * This inconsistency has to be solve in libucc in the
		 * future rather that having this confusing decrement to
		 * priority inside the kernel.
		 */
		if (xse->rt.prio > 0)
			xse->rt.prio -= 1;

		INIT_LIST_HEAD(&xse->rt.list_node);
	}
	WRITE_ONCE(xse->on_rq, false);

	spin_lock_init(&xse->xse_lock);
	return err;
}

/*
 * A function for submitting stream's commands (sending commands to a XCU).
 */
static int xsched_proc(struct xsched_cu *xcu, struct vstream_info *vs,
		       struct vstream_metadata *vsm)
{
	struct xcu_op_handler_params params;
	struct xsched_entity *xse;

	XSCHED_CALL_STUB();

	xse = &vs->ctx->xse;

	/* Init input parameters for xcu_run and xcu_wait callbacks. */
	params.group = xcu->group;

	/* Increase process time by abstract kick handling time. */
	xse->last_exec_runtime += vsm->exec_time;

	XSCHED_DEBUG("Process vsm sq_tail %d exec_time %u sqe_num %d sq_id %d@ %s\n",
		    vsm->sq_tail, vsm->exec_time, vsm->sqe_num, vsm->sq_id, __func__);
	submit_kick(vs, &params, vsm);

	xse->total_submitted++;

	XSCHED_DEBUG("xse %d total_submitted = %lu @ %s\n",
		    xse->tgid, xse->total_submitted, __func__);

	XSCHED_EXIT_STUB();
	return 0;
}

static int __xsched_submit(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	struct vstream_metadata *vsm, *tmp;
	unsigned int submit_exec_time = 0;
	size_t kicks_submitted = 0;
	unsigned long wait_us;

	XSCHED_DEBUG("%s called for xse %d on xcu %u\n",
		__func__, xse->tgid, xcu->id);

	list_for_each_entry_safe(vsm, tmp, &xcu->vsm_list, node) {
		xsched_proc(xcu, vsm->parent, vsm);
		submit_exec_time += vsm->exec_time;
		kicks_submitted++;
	}

	INIT_LIST_HEAD(&xcu->vsm_list);

	mutex_unlock(&xcu->xcu_lock);

	wait_us = div_u64(submit_exec_time, NSEC_PER_USEC);
	XSCHED_DEBUG("XCU kicks_submitted=%lu wait_us=%lu @ %s\n",
		    kicks_submitted, wait_us, __func__);

	if (wait_us > 0) {
		/* Sleep shift not larger than 12.5% */
		usleep_range(wait_us, wait_us + (wait_us >> 3));
	}

	mutex_lock(&xcu->xcu_lock);

	return kicks_submitted;
}

static inline bool should_preempt(struct xsched_entity *xse)
{
	return xse->class->check_preempt(xse);
}

static int xsched_schedule(void *input_xcu)
{
	struct xsched_cu *xcu = input_xcu;
	int err = 0;
	struct xsched_entity *curr_xse = NULL;
	struct xsched_entity *next_xse = NULL;

	while (!kthread_should_stop()) {
		mutex_unlock(&xcu->xcu_lock);
		wait_event_interruptible(xcu->wq_xcu_idle,
					 xcu->xrq.cfs.nr_running || xcu->xrq.rt.nr_running);
		XSCHED_DEBUG("%s: rt nr_running = %u, cfs nr_running = %u\n",
			__func__, xcu->xrq.rt.nr_running, xcu->xrq.cfs.nr_running);

		mutex_lock(&xcu->xcu_lock);
		if (!xsched_check_pending_kicks_xcu(xcu)) {
			XSCHED_WARN("%s: No pending kicks on xcu %u\n", __func__, xcu->id);
			continue;
		}

		next_xse = __raw_pick_next_ctx(xcu);
		if (!next_xse) {
			XSCHED_WARN("%s: Couldn't find next xse on xcu %u\n", __func__, xcu->id);
			continue;
		}

		xcu->xrq.curr_xse = next_xse;

		if (__xsched_submit(xcu, next_xse) == 0)
			continue;

		curr_xse = xcu->xrq.curr_xse;
		if (curr_xse) { /* if not deleted yet */
			put_prev_ctx(curr_xse);
			if (!atomic_read(&curr_xse->kicks_pending_ctx_cnt)) {
				dequeue_ctx(curr_xse, xcu);
				XSCHED_DEBUG(
					"%s: Dequeue xse %d due to zero kicks on xcu %u\n",
					__func__, curr_xse->tgid, xcu->id);
				curr_xse = xcu->xrq.curr_xse = NULL;
			}
		}
	}

	return err;
}

void submit_kick(struct vstream_info *vs,
			struct xcu_op_handler_params *params,
			struct vstream_metadata *vsm)
{
	int ret;

	params->fd = vs->fd;
	params->param_1 = &vs->id;
	params->param_2 = &vs->channel_id;
	params->param_3 = vsm->sqe;
	params->param_4 = &vsm->sqe_num;
	params->param_5 = &vsm->timeout;
	params->param_6 = &vs->sqcq_type;
	params->param_7 = vs->drv_ctx;
	/* Send vstream on a device for processing. */
	ret = xcu_run(params);
	if (ret) {
		XSCHED_ERR(
			"Failed to send vstream tasks vstreamId=%d to a device for processing.\n",
			vs->id);
	}

	XSCHED_DEBUG("Vstream_id %d submit vsm: sq_tail %d\n", vs->id, vsm->sq_tail);

	kfree(vsm);

	return;
}

/* Initialize xsched rt runqueue during kernel init.
 * Should only be called from xsched_rq_init function.
 */
static inline void xsched_rt_rq_init(struct xsched_cu *xcu)
{
	int prio = 0;

	xcu->xrq.rt.nr_running = 0;

	for_each_xse_prio(prio) {
		INIT_LIST_HEAD(&xcu->xrq.rt.rq[prio]);
		xcu->xrq.rt.prio_nr_running[prio] = 0;
		atomic_set(&xcu->xrq.rt.prio_nr_kicks[prio], 0);
	}
}

/* Initialize xsched cfs runqueue during kernel init.
 * Should only be called from xsched_rq_init function.
 */
static inline void xsched_cfs_rq_init(struct xsched_cu *xcu)
{
	xcu->xrq.cfs.nr_running = 0;
	xcu->xrq.cfs.ctx_timeline = RB_ROOT_CACHED;
}

/* Initialize xsched classes' runqueues. */
static inline void xsched_rq_init(struct xsched_cu *xcu)
{
	xcu->xrq.curr_xse = NULL;
	xcu->xrq.class = &rt_xsched_class;
	xcu->xrq.state = XRQ_STATE_IDLE;
	xsched_rt_rq_init(xcu);
	xsched_cfs_rq_init(xcu);
}

/* Initializes all xsched XCU objects.
 * Should only be called from xsched_xcu_register function.
 */
static void xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group,
				int xcu_id)
{
	bitmap_clear(xcu_group_root->xcu_mask, 0, XSCHED_NR_CUS);

	xcu->id = xcu_id;
	xcu->state = XSCHED_XCU_NONE;
	xcu->group = group;

	atomic_set(&xcu->pending_kicks_rt, 0);
	atomic_set(&xcu->pending_kicks_cfs, 0);

	INIT_LIST_HEAD(&xcu->vsm_list);
	init_waitqueue_head(&xcu->wq_xcu_idle);
	mutex_init(&xcu->xcu_lock);

	/* Mark current XCU in a mask inside XCU root group. */
	set_bit(xcu->id, xcu_group_root->xcu_mask);

	/* Initialize current XCU's runqueue. */
	xsched_rq_init(xcu);

	/* This worker should set XCU to XSCHED_XCU_WAIT_IDLE.
	 * If after initialization XCU still has XSCHED_XCU_NONE
	 * status then we can assume that there was a problem
	 * with XCU kthread job.
	 */
	xcu->worker = kthread_run(xsched_schedule, xcu, "xcu_%u", xcu->id);
}

/* Allocates xcu id in xcu_manager array. */
static int alloc_xcu_id(void)
{
	int xcu_id = -1;

	spin_lock(&xcu_mgr_lock);
	if (num_active_xcu >= XSCHED_NR_CUS)
		goto out_unlock;

	xcu_id = num_active_xcu;
	num_active_xcu++;
	XSCHED_DEBUG("Number of active xcu: %d.\n", num_active_xcu);

out_unlock:
	spin_unlock(&xcu_mgr_lock);
	return xcu_id;
}

/* Adds vstream_metadata object to a specified vstream. */
int xsched_vsm_add_tail(struct vstream_info *vs, vstream_args_t *arg)
{
	struct vstream_metadata *new_vsm;

	new_vsm = kmalloc(sizeof(struct vstream_metadata), GFP_KERNEL);
	if (!new_vsm) {
		XSCHED_ERR("Failed to alloc kick metadata for vs %u @ %s\n",
			vs->id, __func__);
		return -ENOMEM;
	}

	if (vs->kicks_count > MAX_VSTREAM_SIZE) {
		kfree(new_vsm);
		return -EBUSY;
	}

	xsched_init_vsm(new_vsm, vs, arg);
	list_add_tail(&new_vsm->node, &vs->metadata_list);
	new_vsm->add_time = ktime_get();
	vs->kicks_count += 1;

	return 0;
}

/* Fetch the first vstream metadata from vstream metadata list
 * and removes it from that list. Returned vstream metadata pointer
 * to be freed after.
 */
struct vstream_metadata *xsched_vsm_fetch_first(struct vstream_info *vs)
{
	struct vstream_metadata *vsm;

	if (list_empty(&vs->metadata_list)) {
		XSCHED_DEBUG("No metadata to fetch from vs %u @ %s\n",
			vs->id, __func__);
		return NULL;
	}

	vsm = list_first_entry(&vs->metadata_list, struct vstream_metadata, node);
	if (!vsm) {
		XSCHED_ERR("Corrupted metadata list in vs %u @ %s\n",
			vs->id, __func__);
		return NULL;
	}

	list_del(&vsm->node);
	if (vs->kicks_count == 0)
		XSCHED_WARN("kicks_count underflow in vs %u @ %s\n",
			vs->id, __func__);
	else
		vs->kicks_count -= 1;

	return vsm;
}

/*
 * Initialize and register xcu in xcu_manager array.
 */
int xsched_xcu_register(struct xcu_group *group)
{
	int xcu_id;
	struct xsched_cu *xcu;

	xcu_id = alloc_xcu_id();
	if (xcu_id < 0) {
		XSCHED_ERR("Fail to alloc xcu id.\n");
		return -ENOSPC;
	};

	xcu = kzalloc(sizeof(struct xsched_cu), GFP_KERNEL);
	if (!xcu) {
		XSCHED_ERR("Fail to alloc xcu.\n");
		return -ENOMEM;
	};

	group->xcu = xcu;
	xsched_cu_mgr[xcu_id] = xcu;

	/* Init xcu's internals. */
	xsched_xcu_init(xcu, group, xcu_id);
	return 0;
}
EXPORT_SYMBOL(xsched_xcu_register);

int __init xsched_init(void)
{
	/* Initializing global XSched context list. */
	INIT_LIST_HEAD(&xsched_ctx_list);
	xcu_cg_init_common(root_xcg);
	return 0;
}
late_initcall(xsched_init);
