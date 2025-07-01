// SPDX-License-Identifier: GPL-2.0+
/*
 * Code for NPU driver support
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
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/xcu_group.h>
#include <linux/xsched.h>

static DECLARE_RWSEM(xcu_group_rwsem);

struct xcu_group *xcu_group_alloc(void)
{
	struct xcu_group *node = kzalloc(sizeof(*node), GFP_KERNEL);

	if (!node)
		return node;

	node->type = XCU_TYPE_NPU;
	idr_init(&node->next_layer);

	return node;
}
EXPORT_SYMBOL(xcu_group_alloc);

int __xcu_group_attach(struct xcu_group *new_group,
		       struct xcu_group *previous_group)
{
	int id = new_group->id;

	if (id == -1)
		id = idr_alloc(&previous_group->next_layer, new_group, 0,
			       INT_MAX, GFP_KERNEL);
	else
		id = idr_alloc(&previous_group->next_layer, new_group, id,
			       id + 1, GFP_KERNEL);
	if (id < 0) {
		XSCHED_ERR("Attach xcu_group failed: id confilict @ %s\n",
			   __func__);
		return -EEXIST;
	}

	new_group->id = id;
	new_group->previous_layer = previous_group;

	return 0;
}

int xcu_group_attach(struct xcu_group *new_group,
		     struct xcu_group *previous_group)
{
	int ret;

	down_write(&xcu_group_rwsem);
	ret = __xcu_group_attach(new_group, previous_group);
	up_write(&xcu_group_rwsem);

	return ret;
}
EXPORT_SYMBOL(xcu_group_attach);

struct xcu_group *xcu_group_alloc_and_attach(struct xcu_group *previous_group,
					     int id)
{
	struct xcu_group *new = xcu_group_alloc();

	if (!new) {
		XSCHED_ERR("Alloc xcu_group failed @ %s\n", __func__);
		return NULL;
	}
	new->id = id;

	if (!xcu_group_attach(new, previous_group))
		return NULL;

	return new;
}
EXPORT_SYMBOL(xcu_group_alloc_and_attach);

static inline int __xcu_group_detach(struct xcu_group *group)
{
	idr_remove(&group->previous_layer->next_layer, group->id);
	return 0;
}

int xcu_group_detach(struct xcu_group *group)
{
	int ret;

	down_write(&xcu_group_rwsem);
	ret = __xcu_group_detach(group);
	up_write(&xcu_group_rwsem);

	return ret;
}
EXPORT_SYMBOL(xcu_group_detach);

static struct xcu_group *__xcu_group_find_nolock(struct xcu_group *group,
						 int id)
{
	return idr_find(&group->next_layer, id);
}

struct xcu_group *xcu_group_find_noalloc(struct xcu_group *group, int id)
{
	struct xcu_group *result;

	down_read(&xcu_group_rwsem);
	result = __xcu_group_find_nolock(group, id);
	up_read(&xcu_group_rwsem);

	return result;
}
EXPORT_SYMBOL(xcu_group_find_noalloc);

struct xcu_group *xcu_group_find(struct xcu_group *group, int id)
{
	struct xcu_group *target_group;

	down_read(&xcu_group_rwsem);
	target_group = __xcu_group_find_nolock(group, id);
	up_read(&xcu_group_rwsem);

	if (!target_group) {
		target_group = xcu_group_alloc();
		target_group->type = id;
		target_group->id = id;
	}

	return target_group;
}
EXPORT_SYMBOL(xcu_group_find);

/* This function runs "run" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object
 */
int xcu_run(struct xcu_op_handler_params *params)
{
	int ret = 0;

	if (params->group->opt && params->group->opt->run)
		ret = params->group->opt->run(params);
	else
		XSCHED_DEBUG("No function [run] called.\n");

	return ret;
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
	if (!params->group->opt || !params->group->opt->finish) {
		XSCHED_DEBUG("No function [finish] called.\n");
		return 0;
	}
	return params->group->opt->finish(params);
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
	int ret = 0;

	if (params->group->opt && params->group->opt->alloc)
		ret = params->group->opt->alloc(params);
	else
		XSCHED_DEBUG("No function [alloc] called.\n");

	return ret;
}

/* This function runs a "logic_alloc" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 *
 * This handler provides an interface to implement allocation
 * and registering memory of logic CQ buffer.
 */
int xcu_logic_alloc(struct xcu_op_handler_params *params)
{
	int ret = 0;

	if (params->group->opt && params->group->opt->logic_alloc)
		ret = params->group->opt->logic_alloc(params);
	else
		XSCHED_DEBUG("No function [logic_alloc] called.\n");

	return ret;
}

/* This function runs a "logic_free" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 *
 * This handler provides an interface to implement deallocation
 * and unregistering memory of a logic CQ buffer.
 */
int xcu_logic_free(struct xcu_op_handler_params *params)
{
	int ret = 0;

	if (params->group->opt && params->group->opt->logic_free)
		ret = params->group->opt->logic_free(params);
	else
		XSCHED_DEBUG("No function [logic_free] called.\n");

	return ret;
}

static struct xcu_group __xcu_group_root = {
	.id = 0,
	.type = XCU_TYPE_ROOT,
	.next_layer = IDR_INIT(next_layer),
};

struct xcu_group *xcu_group_root = &__xcu_group_root;
EXPORT_SYMBOL(xcu_group_root);
