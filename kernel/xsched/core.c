// SPDX-License-Identifier: GPL-2.0+
/*
 * Core kernel scheduler code for XPU device
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
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock_types.h>
#include <linux/types.h>
#include <linux/xsched.h>
#include <uapi/linux/sched/types.h>

int num_active_xcu;
spinlock_t xcu_mgr_lock;

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

	XSCHED_CALL_STUB();

	lockdep_assert_held(&xcu->xcu_lock);

	xse->class->put_prev_ctx(xse);

	xse->last_process_time = 0;

	XSCHED_EXIT_STUB();
}

static size_t select_work_def(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	uint32_t kick_count;
	struct vstream_info *vs;
	unsigned int sum_exec_time = 0;
	size_t kicks_submitted = 0;
	struct vstream_metadata *vsm;
	size_t not_empty;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_INFO("Before decrement XSE kick_count=%u @ %s\n",
			    kick_count, __func__);

	if (kick_count == 0) {
		XSCHED_ERR("Tried to submit xse that has 0 kicks @ %s\n",
			   __func__);
		goto out_err;
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
				XSCHED_INFO(
					"vs id = %d Kick submit exec_time %u sq_tail %u sqe_num %u sq_id %u @ %s\n",
					vs->id, vsm->exec_time, vsm->sq_tail,
					vsm->sqe_num, vsm->sq_id, __func__);
				not_empty++;
			}
		}
	} while ((sum_exec_time < XSCHED_CFS_MIN_TIMESLICE) && (not_empty));

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_INFO("After decrement XSE kick_count=%u @ %s\n",
		    kick_count, __func__);

	xse->kicks_submitted += kicks_submitted;

	XSCHED_INFO("xse %d kicks_submitted = %lu @ %s\n",
			    xse->tgid, xse->kicks_submitted, __func__);

out_err:
	return kicks_submitted;
}

static struct xsched_entity *__raw_pick_next_ctx(struct xsched_cu *xcu)
{
	const struct xsched_class *class;
	struct xsched_entity *next = NULL;

	XSCHED_CALL_STUB();

	lockdep_assert_held(&xcu->xcu_lock);

	for_each_xsched_class(class) {
		next = class->pick_next_ctx(xcu);
		if (next) {
			if (class->select_work)
				class->select_work(xcu, next);
			else
				select_work_def(xcu, next);
			break;
		}
	}

	XSCHED_EXIT_STUB();
	return next;
}

void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	XSCHED_CALL_STUB();

	lockdep_assert_held(&xcu->xcu_lock);

	if (!xse_integrity_check(xse)) {
		XSCHED_ERR("Failed xse integrity check @ %s\n", __func__);
		return;
	}

	if (!xse->on_rq) {
		xse->on_rq = true;
		xse->class->enqueue_ctx(xse, xcu);
		__XSCHED_TRACE("Enqueue xse %d @ %s\n", xse->tgid, __func__);
	}

	XSCHED_EXIT_STUB();
}

void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	XSCHED_CALL_STUB();

	lockdep_assert_held(&xcu->xcu_lock);

	if (!xse_integrity_check(xse)) {
		XSCHED_ERR("Failed xse integrity check @ %s\n", __func__);
		return;
	}

	if (xse->on_rq) {
		xse->class->dequeue_ctx(xse);
		xse->on_rq = false;
		__XSCHED_TRACE("Dequeue xse %d @ %s\n", xse->tgid, __func__);
	}

	XSCHED_EXIT_STUB();
}

static int delete_ctx(struct xsched_context *ctx)
{
	struct xsched_cu *xcu = ctx->xse.xcu;
	struct xsched_entity *curr_xse = xcu->xrq.curr_xse;
	struct xsched_entity *xse = &ctx->xse;

	XSCHED_CALL_STUB();

	if (!xse_integrity_check(xse)) {
		XSCHED_ERR("Failed xse integrity check! %s\n", __func__);
		return -EINVAL;
	}

	if (!xse->xcu) {
		XSCHED_ERR("Trying to delete ctx that is not attached to an XCU @ %s\n", __func__);
		return -EINVAL;
	}

	/* Wait till context has been submitted. */
	while (atomic_read(&xse->kicks_pending_ctx_cnt)) {
		XSCHED_INFO(
			"Deleting ctx %d, xse->kicks_pending_ctx_cnt=%d @ %s\n",
			xse->tgid, atomic_read(&xse->kicks_pending_ctx_cnt),
			__func__);
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

	XSCHED_INFO("Deleting ctx %d, pending kicks left=%d @ %s\n", xse->tgid,
		    atomic_read(&xse->kicks_pending_ctx_cnt), __func__);

	XSCHED_EXIT_STUB();

	return 0;
}

/* Frees a given vstream and also frees and dequeues it's context
 * if a given vstream is the last and only vstream attached to it's
 * corresponding context object.
 */
void xsched_free_task(struct kref *kref)
{
	struct xsched_context *ctx;
	vstream_info_t *vs, *tmp;

	XSCHED_CALL_STUB();

	ctx = container_of(kref, struct xsched_context, kref);

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
	XSCHED_INFO("Ctx list mutex released @ %s\n", __func__);

	kfree(ctx);
}

int bind_ctx_to_xcu(vstream_info_t *vstream_info, struct xsched_context *ctx)
{
	struct ctx_devid_revmap_data *revmap_data;
	int target_chan_id = 0;

	/* Find XCU history. */
	hash_for_each_possible(ctx_revmap, revmap_data, hash_node,
				(unsigned long)ctx->devId) {
		if (revmap_data && revmap_data->group) {
			/* Bind ctx to group xcu.*/
			ctx->xse.xcu = revmap_data->group->xcu;
			return 0;
		}
	}

	revmap_data = kzalloc(sizeof(struct ctx_devid_revmap_data), GFP_KERNEL);

	if (revmap_data == NULL) {
		XSCHED_ERR("Revmap_data is NULL @ %s\n", __func__);
		return -1;
	}

	/* Find a real XCU group and if it doesn't exist then try
	 * to find or allocate a XCU_TYPE_NPU group.
	 */
	revmap_data->group =
		xcu_group_find_noalloc(xcu_group_root, XCU_TYPE_NPU);
	target_chan_id = vstream_info->channel_id;

	if (revmap_data->group == NULL) {
		target_chan_id = 0;

		XSCHED_INFO(
			"Failed to find an XCU group for a real device @ %s\n",
			__func__);
		XSCHED_INFO("Creating a test QEMU XCU group");

		revmap_data->group =
			xcu_group_find_noalloc(xcu_group_root, XCU_TYPE_NPU);

		/* If XCU_TYPE_NPU device creation failed then something
		 * went very wrong and we need to return an error.
		 */
		if (revmap_data->group == NULL) {
			XSCHED_ERR(
				"Failed to find or create a QEMU test XCU group @ %s\n",
				__func__);
			return -1;
		}
	}

	/* Find a device group. */
	revmap_data->group =
		xcu_group_find_noalloc(revmap_data->group, ctx->devId);
	if (revmap_data->group == NULL) {
		XSCHED_ERR(
			"Failed to find a device group for dev_id 0x%X @ %s\n",
			ctx->devId, __func__);
		return -1;
	}

	XSCHED_INFO("Found a dev group 0x%X @ %s\n", revmap_data->group->id,
		    __func__);

	/* Find a channel group. */
	revmap_data->group =
		xcu_group_find_noalloc(revmap_data->group, target_chan_id);
	if (revmap_data->group == NULL) {
		XSCHED_ERR(
			"Failed to find a channel group for dev_id 0x%X and chan_id %d @ %s\n",
			ctx->devId, target_chan_id, __func__);
		return -1;
	}

	XSCHED_INFO("Found a channel group 0x%X with XCU %p @ %s\n",
		    revmap_data->group->id, revmap_data->group->xcu, __func__);

	revmap_data->devId = vstream_info->devId;

	/* Bind ctx to an XCU from channel group. */
	ctx->xse.xcu = revmap_data->group->xcu;
	vstream_info->xcu = ctx->xse.xcu;

	XSCHED_INFO("Bound an XCU %p @ %s\n", ctx->xse.xcu, __func__);

	revmap_data->devId = vstream_info->devId;
	hash_add(ctx_revmap, &revmap_data->hash_node,
		 (unsigned long)ctx->devId);

	return 0;
}

int bind_vstream_to_xcu(vstream_info_t *vstream_info)
{
	struct xsched_cu *xcu_found = NULL;
	__u32 type = XCU_TYPE_NPU;

	xcu_found = xcu_find(&type, vstream_info->devId, vstream_info->channel_id);

	if (!xcu_found)
		return -1;

	/* Bind vstream to a xcu. */
	vstream_info->xcu = xcu_found;

	XSCHED_INFO("XCU bound to a vstream: type=%u, dev_id=%u, chan_id=%u.\n",
		    type, vstream_info->devId, vstream_info->channel_id);

	return 0;
}

struct xsched_cu *xcu_find(__u32 *type, __u32 devId, __u32 channel_id)
{
	struct xcu_group *group = NULL;
	__u32 local_type = *type;

	/* Find xcu by type. */
	group = xcu_group_find_noalloc(xcu_group_root, local_type);
	if (group == NULL) {
		XSCHED_INFO("Find XCU group with real device is failed.\n");

		local_type = XCU_TYPE_NPU;
		group = xcu_group_find_noalloc(xcu_group_root, local_type);
		if (group == NULL) {
			XSCHED_ERR("Find XCU with qemu device is failed.\n");
			return NULL;
		}
	}

	/* Find by device id group. */
	group = xcu_group_find_noalloc(group, devId);
	if (group == NULL) {
		XSCHED_ERR("Find device group is failed.\n");
		return NULL;
	}
	/* Find channel id group. */
	group = xcu_group_find_noalloc(group, channel_id);
	if (group == NULL) {
		XSCHED_ERR("Find channel group is failed.\n");
		return NULL;
	}

	*type = local_type;

	XSCHED_INFO("XCU found: type=%u, dev_id=%u, chan_id=%u.\n", local_type,
		    devId, channel_id);

	return group->xcu;
}

int xsched_xse_set_class(struct xsched_entity *xse)
{
	switch (xse->task_type) {
	case XSCHED_TYPE_RT:
		xse->class = &rt_xsched_class;
		XSCHED_INFO("Context is in RT class %s\n", __func__);
		break;
	case XSCHED_TYPE_CFS:
		xse->class = &fair_xsched_class;
		XSCHED_INFO("Context is in CFS class %s\n", __func__);
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

	XSCHED_CALL_STUB();

	atomic_set(&xse->kicks_pending_ctx_cnt, 0);
	atomic_set(&xse->kicks_submited, 0);
	xse->task_type = XSCHED_TYPE_RT;
	xse->last_process_time = 0;

	xse->fd = ctx->fd;
	xse->tgid = ctx->tgid;

	err = bind_ctx_to_xcu(vs, ctx);
	if (err) {
		XSCHED_ERR(
			"Couldn't find valid xcu for vstream %u dev_id %u @ %s\n",
			vs->id, vs->devId, __func__);
		err = -EINVAL;
		goto out_err;
	}

	xse->ctx = ctx;

	if (vs->xcu != NULL)
		xse->xcu = vs->xcu;

	err = xsched_xse_set_class(xse);
	if (err) {
		XSCHED_ERR("Failed to set xse class @ %s\n", __func__);
		goto out_err;
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
out_err:
	XSCHED_EXIT_STUB();

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
	xse->last_process_time += vsm->exec_time;

	XSCHED_INFO("Process vsm sq_tail %d exec_time %u sqe_num %d sq_id %d@ %s\n",
		    vsm->sq_tail, vsm->exec_time, vsm->sqe_num, vsm->sq_id, __func__);
	submit_kick(vs, &params, vsm);

	xse->kicks_processed++;

	XSCHED_INFO("xse %d kicks_processed = %lu @ %s\n",
		    xse->tgid, xse->kicks_processed, __func__);

	XSCHED_EXIT_STUB();
	return 0;
}

static int __xsched_submit(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	int err = 0;
	struct vstream_metadata *vsm, *tmp;
	unsigned int submit_exec_time = 0;
	size_t kicks_submitted = 0;
	unsigned long wait_us;

	XSCHED_CALL_STUB();

	XSCHED_INFO("%s called for xse %d on xcu %u\n", __func__, xse->tgid,
		    xcu->id);

	list_for_each_entry_safe(vsm, tmp, &xcu->vsm_list, node) {
		xsched_proc(xcu, vsm->parent, vsm);
		submit_exec_time += vsm->exec_time;
		kicks_submitted++;
	}

	INIT_LIST_HEAD(&xcu->vsm_list);

	mutex_unlock(&xcu->xcu_lock);

	wait_us = div_u64(submit_exec_time, NSEC_PER_USEC);
	XSCHED_INFO("XCU kicks_submitted=%lu wait_us=%lu @ %s\n",
		    kicks_submitted, wait_us, __func__);

	if (wait_us > 0) {
		/* Sleep shift not larger than 12.5% */
		usleep_range(wait_us, wait_us + (wait_us >> 3));
	}

	mutex_lock(&xcu->xcu_lock);

	XSCHED_EXIT_STUB();

	return err;
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

	XSCHED_CALL_STUB();

	while (!kthread_should_stop()) {
		XSCHED_INFO("%s: Xcu lock released\n", __func__);
		mutex_unlock(&xcu->xcu_lock);

		wait_event_interruptible(xcu->wq_xcu_idle,
					 atomic_read(&xcu->has_active) || xcu->xrq.nr_running);

		XSCHED_INFO("%s: rt_nr_running = %d, has_active = %d\n",
			__func__, xcu->xrq.nr_running, atomic_read(&xcu->has_active));

		mutex_lock(&xcu->xcu_lock);
		XSCHED_INFO("%s: Xcu lock taken\n", __func__);

		if (!xsched_check_pending_kicks_xcu(xcu)) {
			XSCHED_ERR("%s: No pending kicks for xcu %u\n", __func__, xcu->id);
			continue;
		}

		next_xse = __raw_pick_next_ctx(xcu);

		if (!next_xse) {
			XSCHED_ERR("%s: Couldn't find next xse for xcu %u\n", __func__, xcu->id);
			continue;
		}

		xcu->xrq.curr_xse = next_xse;
		__XSCHED_TRACE("%s: Pick next ctx returned xse %d\n", __func__, next_xse->tgid);

		if (__xsched_submit(xcu, next_xse)) {
			XSCHED_ERR("%s: Xse %d on XCU %u tried to submit with zero kicks\n",
			__func__, next_xse->tgid, xcu->id);
			continue;
		}

		curr_xse = xcu->xrq.curr_xse;
		if (curr_xse) { /* if not deleted yet */
			put_prev_ctx(curr_xse);
			if (!atomic_read(&curr_xse->kicks_pending_ctx_cnt)) {
				dequeue_ctx(curr_xse, xcu);
				XSCHED_INFO(
					"%s: Dequeue xse %d due to zero kicks on xcu %u\n",
					__func__, curr_xse->tgid, xcu->id);
				curr_xse = xcu->xrq.curr_xse = NULL;
			}
		}
	}

	XSCHED_INFO("Xsched_schedule finished for xcu %u\n", xcu->id);

	XSCHED_EXIT_STUB();

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

	XSCHED_INFO("Vstream_id %d submit vsm: sq_tail %d\n", vs->id, vsm->sq_tail);

	kfree(vsm);
}

/* Initialize xsched rt runqueue during kernel init.
 * Should only be called from xsched_init function.
 */
static inline void xsched_rt_rq_init(struct xsched_cu *xcu)
{
	int prio = 0;

	for_each_xse_prio(prio) {
		INIT_LIST_HEAD(&xcu->xrq.rt.rq[prio]);
		xcu->xrq.rt.prio_nr_running[prio] = 0;
		atomic_set(&xcu->xrq.rt.prio_nr_kicks[prio], 0);
	}
}

/* Initialize xsched cfs runqueue during kernel init.
 * Should only be called from xsched_init function.
 */
static inline void xsched_cfs_rq_init(struct xsched_cu *xcu)
{
	xcu->xrq.cfs.ctx_timeline = RB_ROOT_CACHED;
}

/* Initialize xsched classes' runqueues. */
static inline void xsched_rq_init(struct xsched_cu *xcu)
{
	xcu->xrq.nr_running = 0;
	xcu->xrq.curr_xse = NULL;
	xcu->xrq.class = &rt_xsched_class;
	xcu->xrq.state = XRQ_STATE_IDLE;
	xsched_rt_rq_init(xcu);
	xsched_cfs_rq_init(xcu);
}

/* Initializes all xsched XCU objects.
 * Should only be called from xsched_register_xcu function.
 */
static void xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group,
			    int xcu_id)
{
	bitmap_clear(xcu_group_root->xcu_mask, 0, XSCHED_NR_CUS);

	xcu->id = xcu_id;
	xcu->xrq.curr_xse = NULL;
	xcu->state = XSCHED_XCU_NONE;
	xcu->group = group;

	atomic_set(&xcu->pending_kicks_rt, 0);
	atomic_set(&xcu->pending_kicks_cfs, 0);
	atomic_set(&xcu->has_active, 0);

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
	XSCHED_INFO("Number of active xcus: %d.\n", num_active_xcu);

out_unlock:
	spin_unlock(&xcu_mgr_lock);
	return xcu_id;
}

/* Adds vstream_metadata object to a specified vstream. */
int xsched_vsm_add_tail(struct vstream_info *vs, vstream_args_t *arg)
{
	int err = 0;
	struct vstream_metadata *new_vsm;

	new_vsm = kmalloc(sizeof(struct vstream_metadata), GFP_KERNEL);
	if (!new_vsm) {
		XSCHED_ERR("Failed to allocate kick metadata for vs %u @ %s\n",
			   vs->id, __func__);
		err = -ENOMEM;
		goto out_err;
	}

	xsched_init_vsm(new_vsm, vs, arg);

	if (vs->kicks_count > MAX_VSTREAM_SIZE) {
		err = -EBUSY;
		kfree(new_vsm);
		goto out_err;
	}

	list_add_tail(&new_vsm->node, &vs->metadata_list);
	vs->kicks_count += 1;

	XSCHED_INFO("Vstream_id %u Add vsm: sq_tail %u, sqe_num %u, kicks_count %u\n",
			vs->id, new_vsm->sq_tail, new_vsm->sqe_num, vs->kicks_count);

out_err:
	return err;
}

/* Fetch the first vstream metadata from vstream metadata list
 * and removes it from that list. Returned vstream metadata pointer
 * to be freed after.
 */
struct vstream_metadata *xsched_vsm_fetch_first(struct vstream_info *vs)
{
	struct vstream_metadata *vsm;

	if (list_empty(&vs->metadata_list)) {
		XSCHED_INFO("No metadata to fetch from vs %u @ %s\n", vs->id,
			    __func__);
		goto out_null;
	}

	vsm = list_first_entry(&vs->metadata_list, struct vstream_metadata, node);

	if (!vsm) {
		XSCHED_ERR(
			"Tried to delete metadata from empty list in vs %u @ %s\n",
			vs->id, __func__);
		goto out_null;
	}

	list_del(&vsm->node);
	vs->kicks_count -= 1;

	return vsm;

out_null:
	return NULL;
}

/*
 * Initialize and register xcu in xcu_manager array.
 */
int xsched_register_xcu(struct xcu_group *group)
{
	int xcu_id;
	struct xsched_cu *xcu;

	xcu_id = alloc_xcu_id();
	if (xcu_id < 0) {
		XSCHED_ERR("Alloc xcu_id failed.\n");
		return -1;
	};

	xcu = kzalloc(sizeof(struct xsched_cu), GFP_KERNEL);
	if (!xcu) {
		XSCHED_ERR("Alloc xcu structure failed.\n");
		return -1;
	};

	group->xcu = xcu;
	xsched_cu_mgr[xcu_id] = xcu;

	/* Init xcu's internals. */
	xsched_xcu_init(xcu, group, xcu_id);

	return 0;
}
EXPORT_SYMBOL(xsched_register_xcu);

int __init xsched_init(void)
{
	/* Initializing global Xsched context list. */
	INIT_LIST_HEAD(&xsched_ctx_list);

	return 0;
}

late_initcall(xsched_init);
