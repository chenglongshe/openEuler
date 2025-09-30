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
}

static struct xsched_entity *__raw_pick_next_ctx(struct xsched_cu *xcu)
{
	return NULL;
}

void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
}

void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
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
	return 0;
}

int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs)
{
	int err = 0;
	struct xsched_entity *xse = &ctx->xse;

	atomic_set(&xse->kicks_pending_ctx_cnt, 0);
	atomic_set(&xse->submitted_one_kick, 0);

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

	WRITE_ONCE(xse->on_rq, false);

	spin_lock_init(&xse->xse_lock);
	return err;
}

static int __xsched_submit(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	return 0;
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
					 atomic_read(&xcu->has_active) || xcu->xrq.nr_running);

		XSCHED_DEBUG("%s: rt_nr_running = %d, has_active = %d\n",
			__func__, xcu->xrq.nr_running, atomic_read(&xcu->has_active));

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

/* Initialize xsched classes' runqueues. */
static inline void xsched_rq_init(struct xsched_cu *xcu)
{
	xcu->xrq.nr_running = 0;
	xcu->xrq.curr_xse = NULL;
	xcu->xrq.state = XRQ_STATE_IDLE;
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

	atomic_set(&xcu->has_active, 0);

	INIT_LIST_HEAD(&xcu->vsm_list);

	init_waitqueue_head(&xcu->wq_xcu_idle);

	mutex_init(&xcu->xcu_lock);

	/* Mark current XCU in a mask inside XCU root group. */
	set_bit(xcu->id, xcu_group_root->xcu_mask);

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
	/* Initializing global Xsched context list. */
	INIT_LIST_HEAD(&xsched_ctx_list);

	return 0;
}

late_initcall(xsched_init);
