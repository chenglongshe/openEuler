// SPDX-License-Identifier: GPL-2.0+
/*
 * Completely Fair Scheduling (CFS) Class for XPU device
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
#include <linux/xsched.h>

#define CFS_INNER_RQ_EMPTY(cfs_xse)	\
	((cfs_xse)->xruntime == XSCHED_TIME_INF)

/* For test xsched_cfs_grp_test.c */
atomic64_t virtual_sched_clock = ATOMIC_INIT(0);

/*
 * Xsched Fair class methods
 * For rq manipulation we rely on root runqueue lock already acquired in core.
 * Access xsched_group_xcu_priv requires no locks because one thread per XCU.
 */
static void dequeue_ctx_fair(struct xsched_entity *xse)
{
}

/**
 * enqueue_ctx_fair() - Add context to the runqueue
 * @xse: xsched entity of context
 * @xcu: executor
 *
 * In contrary to enqueue_task it is called once on context init.
 * Although groups reside in tree, their nodes not counted in nr_running.
 * The xruntime of a group xsched entitry represented by min xruntime inside.
 */
static void enqueue_ctx_fair(struct xsched_entity *xse, struct xsched_cu *xcu)
{
}

static struct xsched_entity *pick_next_ctx_fair(struct xsched_cu *xcu)
{
	return NULL;
}

static inline bool xs_should_preempt_fair(struct xsched_entity *xse)
{
	return 0;
}

static void put_prev_ctx_fair(struct xsched_entity *xse)
{
}

int submit_prepare_ctx_fair(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	return 0;
}

const struct xsched_class fair_xsched_class = {
	.next = NULL,
	.dequeue_ctx = dequeue_ctx_fair,
	.enqueue_ctx = enqueue_ctx_fair,
	.pick_next_ctx = pick_next_ctx_fair,
	.put_prev_ctx = put_prev_ctx_fair,
	.submit_prepare_ctx = submit_prepare_ctx_fair,
	.check_preempt = xs_should_preempt_fair,
};
