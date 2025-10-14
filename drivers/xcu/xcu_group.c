// SPDX-License-Identifier: GPL-2.0+
/*
 * Code for NPU driver support
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
#include <linux/xcu_group.h>

/* This function runs "run" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object
 */
int xcu_run(struct xcu_op_handler_params *params)
{
	return 0;
}

/* This function runs "wait" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object
 */
int xcu_wait(struct xcu_op_handler_params *params)
{
	return 0;
}

/* This function runs "complete" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 */
int xcu_complete(struct xcu_op_handler_params *params)
{
	return 0;
}

/* This function runs "finish" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 *
 * This handler provides an interface to implement deallocation
 * and freeing memory for SQ and CQ buffers.
 */
int xcu_finish(struct xcu_op_handler_params *params)
{
	return 0;
}

/* This function runs a "alloc" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 *
 * This handler provides an interface to implement allocation
 * and registering memory for SQ and CQ buffers.
 */
int xcu_alloc(struct xcu_op_handler_params *params)
{
	return 0;
}

static struct xcu_group __xcu_group_root = {
	.id = 0,
	.type = XCU_TYPE_ROOT,
	.next_layer = IDR_INIT(next_layer),
};

struct xcu_group *xcu_group_root = &__xcu_group_root;
EXPORT_SYMBOL(xcu_group_root);
