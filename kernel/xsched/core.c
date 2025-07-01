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

	mutex_lock(&xsched_ctx_list_mutex);
	list_for_each_entry_safe(vs, tmp, &ctx->vstream_list, ctx_node) {
		list_del(&vs->ctx_node);
		kfree(vs->data);
		kfree(vs);
	}

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
	return 0;
}

int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs)
{
	int err = 0;
	struct xsched_entity *xse = &ctx->xse;

	XSCHED_CALL_STUB();

	atomic_set(&xse->kicks_pending_ctx_cnt, 0);
	atomic_set(&xse->kicks_submited, 0);

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

	WRITE_ONCE(xse->on_rq, false);

	spin_lock_init(&xse->xse_lock);
out_err:
	XSCHED_EXIT_STUB();

	return err;
}

static int xsched_schedule(void *input_xcu)
{
	return 0;
}

/* Initializes all xsched XCU objects.
 * Should only be called from xsched_register_xcu function.
 */
static void xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group,
			    int xcu_id)
{
	bitmap_clear(xcu_group_root->xcu_mask, 0, XSCHED_NR_CUS);

	xcu->id = xcu_id;
	xcu->state = XSCHED_XCU_NONE;
	xcu->group = group;

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
