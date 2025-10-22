/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/rwsem.h>
#include <linux/slab.h>

#include "xcu_group.h"
#include "xsched.h"

static DECLARE_RWSEM(xcu_group_rwsem);

struct xcu_group *xcu_group_init(int id)
{
	struct xcu_group *node = kzalloc(sizeof(*node), GFP_KERNEL);

	if (!node)
		return NULL;

	node->id = id;
	node->type = XCU_TYPE_NPU;
	idr_init(&node->next_layer);
	atomic_set(&node->child_count, 0);
	return node;
}

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
		XSCHED_ERR("Fail to attach xcu_group: id conflict @ %s\n",
			__func__);
		return -EEXIST;
	}
	new_group->id = id;
	new_group->previous_layer = previous_group;
	atomic_inc(&previous_group->child_count);

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

static inline void __xcu_group_detach(struct xcu_group *group)
{
	if (!group || !group->previous_layer)
		return;

	if (WARN_ON(!xcu_group_is_empty(group)))
		XSCHED_ERR("Leaking group %u with children\n", group->id);
	idr_remove(&group->previous_layer->next_layer, group->id);
	atomic_dec(&group->previous_layer->child_count);
	group->previous_layer = NULL;
}

void xcu_group_detach(struct xcu_group *group)
{
	down_write(&xcu_group_rwsem);
	__xcu_group_detach(group);
	up_write(&xcu_group_rwsem);
}

/* Must call xcu_group_detach() before xcu_group_free() */
void xcu_group_free(struct xcu_group *group)
{
	idr_destroy(&group->next_layer);
	if (group != xcu_group_root)
		kfree(group);
}

static struct xcu_group *__xcu_group_find_nolock(
	struct xcu_group *group, int id)
{
	return idr_find(&group->next_layer, id);
}

struct xcu_group *xcu_group_find(struct xcu_group *group, int id)
{
	struct xcu_group *result;

	down_read(&xcu_group_rwsem);
	result = __xcu_group_find_nolock(group, id);
	up_read(&xcu_group_rwsem);

	return result;
}

bool xcu_group_is_empty(struct xcu_group *group)
{
	return atomic_read(&group->child_count) == 0;
}

/* This function runs "run" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object
 */
int xcu_run(struct xcu_op_handler_params *params)
{
	if (!params->group->opt || !params->group->opt->run) {
		XSCHED_ERR("No function [run] called.\n");
		return -EINVAL;
	}

	return params->group->opt->run(params);
}

/* This function runs "wait" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object
 */
int xcu_wait(struct xcu_op_handler_params *params)
{
	if (!params->group->opt || !params->group->opt->wait) {
		XSCHED_ERR("No function [wait] called.\n");
		return -EINVAL;
	}

	return params->group->opt->wait(params);
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
		XSCHED_ERR("No function [finish] called.\n");
		return -EINVAL;
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
	if (!params->group->opt || !params->group->opt->alloc) {
		XSCHED_ERR("No function [alloc] called.\n");
		return -EINVAL;
	}

	return params->group->opt->alloc(params);
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
	if (!params->group->opt || !params->group->opt->logic_alloc) {
		XSCHED_ERR("No function [logic_alloc] called.\n");
		return -EINVAL;
	}

	return params->group->opt->logic_alloc(params);
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
	if (!params->group->opt || !params->group->opt->logic_free) {
		XSCHED_ERR("No function [logic_free] called.\n");
		return -EINVAL;
	}

	return params->group->opt->logic_free(params);
}

/* This function runs a "sqe_op" callback for a given xcu_group
 * and a given vstream that are passed within
 * xcu_op_handler_params object.
 *
 * This handler provides an interface to set or get sqe info.
 */
int xcu_sqe_op(struct xcu_op_handler_params *params)
{
	if (!params->group->opt || !params->group->opt->sqe_op) {
		XSCHED_ERR("No function [sqe_op] called.\n");
		return -EINVAL;
	}

	return params->group->opt->sqe_op(params);
}

static struct xcu_group __xcu_group_root = {
	.id = 0,
	.type = XCU_TYPE_ROOT,
	.next_layer = IDR_INIT(next_layer),
	.child_count = ATOMIC_INIT(0),
};

struct xcu_group *xcu_group_root = &__xcu_group_root;
