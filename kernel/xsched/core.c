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

static int xsched_schedule(void *input_xcu)
{
	return 0;
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
