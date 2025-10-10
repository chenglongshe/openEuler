/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __XSCHED_XCU_GROUP_H__
#define __XSCHED_XCU_GROUP_H__

#include <linux/idr.h>
#include <uapi/linux/xcu_vstream.h>

extern struct xcu_group *xcu_group_root;

enum xcu_type {
	XCU_TYPE_ROOT,
	XCU_TYPE_XPU,
};

struct xcu_op_handler_params {
};

typedef int (*xcu_op_handler_fn_t)(struct xcu_op_handler_params *params);

struct xcu_operation {
	xcu_op_handler_fn_t run;
	xcu_op_handler_fn_t finish;
	xcu_op_handler_fn_t wait;
	xcu_op_handler_fn_t complete;
	xcu_op_handler_fn_t alloc;
};

struct xcu_group {
	/* sq id. */
	uint32_t id;

	/* Type of XCU group. */
	enum xcu_type type;

	/* IDR for the next layer of XCU group tree. */
	struct idr next_layer;
};

#ifdef CONFIG_XCU_SCHEDULER
int xcu_group_attach(struct xcu_group *new_group,
		     struct xcu_group *previous_group);
void xcu_group_detach(struct xcu_group *group);
struct xcu_group *xcu_group_find(struct xcu_group *group, int id);
struct xcu_group *xcu_group_init(int id);
void xcu_group_free(struct xcu_group *group);

extern int xcu_run(struct xcu_op_handler_params *params);
extern int xcu_wait(struct xcu_op_handler_params *params);
extern int xcu_complete(struct xcu_op_handler_params *params);
extern int xcu_finish(struct xcu_op_handler_params *params);
extern int xcu_alloc(struct xcu_op_handler_params *params);
extern int xcu_logic_alloc(struct xcu_op_handler_params *params);
extern int xcu_logic_free(struct xcu_op_handler_params *params);
#endif /* !CONFIG_XCU_SCHEDULER */

#endif /* __XSCHED_XCU_GROUP_H__ */
