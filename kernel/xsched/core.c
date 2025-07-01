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

int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs)
{
	return 0;
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
