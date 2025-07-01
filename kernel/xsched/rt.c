// SPDX-License-Identifier: GPL-2.0+
/*
 * Real-Time Scheduling Class for XPU device
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

#include <uapi/linux/sched/types.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/xsched.h>
#include <linux/vstream.h>

static void dequeue_ctx_rt(struct xsched_entity *xse) {}

static void enqueue_ctx_rt(struct xsched_entity *xse, struct xsched_cu *xcu) {}

static struct xsched_entity *pick_next_ctx_rt(struct xsched_cu *xcu)
{
	return NULL;
}

static void put_prev_ctx_rt(struct xsched_entity *xse) {}

static int submit_prepare_ctx_rt(struct xsched_entity *xse,
								struct xsched_cu *xcu)
{
	return 0;
}

static size_t select_work_rt(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	return 0;
}

static bool check_preempt_ctx_rt(struct xsched_entity *xse)
{
	return true;
}

const struct xsched_class rt_xsched_class = {
	.next = NULL,
	.dequeue_ctx = dequeue_ctx_rt,
	.enqueue_ctx = enqueue_ctx_rt,
	.pick_next_ctx = pick_next_ctx_rt,
	.put_prev_ctx = put_prev_ctx_rt,
	.submit_prepare_ctx = submit_prepare_ctx_rt,
	.select_work = select_work_rt,
	.check_preempt = check_preempt_ctx_rt
};
