// SPDX-License-Identifier: GPL-2.0+
/*
 * Real-Time Scheduling Class for XPU device
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

#include <uapi/linux/sched/types.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/xsched.h>
#include <linux/vstream.h>

/* Add xsched entitiy to a run list based on priority, set on_cu flag
 * and set a corresponding curr_prios bit if necessary.
 */
static inline void
xse_rt_add(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	list_add_tail(&xse->rt.list_node, &xcu->xrq.rt.rq[xse->rt.prio]);
	__set_bit(xse->rt.prio, xcu->xrq.rt.curr_prios);
}

/* Delete xsched entitiy from a run list, unset on_cu flag and
 * unset corresponding curr_prios bit if necessary.
 */
static inline void xse_rt_del(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	list_del_init(&xse->rt.list_node);
	if (list_empty(&xcu->xrq.rt.rq[xse->rt.prio]))
		__clear_bit(xse->rt.prio, xcu->xrq.rt.curr_prios);
}

static inline void xse_rt_move_tail(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	list_move_tail(&xse->rt.list_node, &xcu->xrq.rt.rq[xse->rt.prio]);
}

/* Increase RT runqueue total and per prio nr_running stat. */
static inline void xrq_inc_nr_running(struct xsched_entity *xse,
				      struct xsched_cu *xcu)
{
	xcu->xrq.rt.nr_running++;
	xcu->xrq.rt.prio_nr_running[xse->rt.prio]++;
	set_bit(xse->rt.prio, xcu->xrq.rt.curr_prios);
}

/* Decrease RT runqueue total and per prio nr_running stat
 * and raise a bug if nr_running decrease beyond zero.
 */
static inline void xrq_dec_nr_running(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	xcu->xrq.rt.nr_running--;
	xcu->xrq.rt.prio_nr_running[xse->rt.prio]--;

	if (!xcu->xrq.rt.prio_nr_running[xse->rt.prio])
		clear_bit(xse->rt.prio, xcu->xrq.rt.curr_prios);
}

static void dequeue_ctx_rt(struct xsched_entity *xse)
{
	xse_rt_del(xse);
	xrq_dec_nr_running(xse);
}

static void enqueue_ctx_rt(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	xse_rt_add(xse, xcu);
	xrq_inc_nr_running(xse, xcu);
}

static inline struct xsched_entity *xrq_next_xse(struct xsched_cu *xcu,
						 int prio)
{
	return list_first_entry(&xcu->xrq.rt.rq[prio], struct xsched_entity,
				rt.list_node);
}

/* Return the next priority for pick_next_ctx taking into
 * account if there are pending kicks on certain priority.
 */
static inline uint32_t get_next_prio_rt(struct xsched_rq *xrq)
{
	int32_t curr_prio;
	bool bit_val;
	unsigned long *prios = xrq->rt.curr_prios;
	atomic_t *prio_nr_kicks = xrq->rt.prio_nr_kicks;

	/* Using generic for loop instead of for_each_set_bit
	 * because it will be faster than for_each_set_bit.
	 */
	for (curr_prio = NR_XSE_PRIO - 1; curr_prio >= 0; curr_prio--) {
		bit_val = test_bit(curr_prio, prios);
		if (!bit_val && atomic_read(&prio_nr_kicks[curr_prio])) {
			XSCHED_ERR(
				"kicks > 0 on RT priority with the priority bit unset\n");
			BUG();
			return NR_XSE_PRIO;
		}

		if (bit_val && atomic_read(&prio_nr_kicks[curr_prio]))
			return curr_prio;
	}
	return NR_XSE_PRIO;
}

static struct xsched_entity *pick_next_ctx_rt(struct xsched_cu *xcu)
{
	struct xsched_entity *result;
	int next_prio;

	next_prio = get_next_prio_rt(&xcu->xrq);
	if (next_prio >= NR_XSE_PRIO) {
		XSCHED_DEBUG("No pending kicks in RT class @ %s\n", __func__);
		return NULL;
	}

	if (!xcu->xrq.rt.prio_nr_running[next_prio]) {
		XSCHED_ERR(
			"The nr_running of RT is 0 while there are pending kicks for %u prio\n",
			next_prio);
		return NULL;
	}

	result = xrq_next_xse(xcu, next_prio);
	if (!result)
		XSCHED_ERR("Next XSE not found @ %s\n", __func__);

	return result;
}

static void put_prev_ctx_rt(struct xsched_entity *xse)
{
	xse->rt.kick_slice -= atomic_read(&xse->submitted_one_kick);
	XSCHED_DEBUG(
		"Update XSE=%d kick_slice=%lld, XSE submitted=%d in RT class @ %s\n",
		xse->tgid, xse->rt.kick_slice,
		atomic_read(&xse->submitted_one_kick), __func__);

	if (xse->rt.kick_slice <= 0) {
		xse->rt.kick_slice = XSCHED_RT_KICK_SLICE;
		XSCHED_DEBUG("Refill XSE=%d kick_slice=%lld in RT class @ %s\n",
			    xse->tgid, xse->rt.kick_slice, __func__);
		xse_rt_move_tail(xse);
	}
}

static int submit_prepare_ctx_rt(struct xsched_entity *xse,
				struct xsched_cu *xcu)
{
	if (!atomic_read(&xse->kicks_pending_ctx_cnt)) {
		xse->rt.state = XSE_READY;
		xse->rt.kick_slice = 0;
		return -EAGAIN;
	}
	xse->rt.state = XSE_RUNNING;

	return 0;
}

static bool check_preempt_ctx_rt(struct xsched_entity *xse)
{
	return true;
}

static size_t select_work_rt(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	int kick_count, scheduled = 0;
	struct vstream_info *vs;
	struct vstream_metadata *vsm;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("Before decrement XSE kick_count=%d @ %s\n",
		kick_count, __func__);

	if (kick_count == 0) {
		XSCHED_WARN("Try to select xse that has 0 kicks @ %s\n",
			__func__);
		return 0;
	}

	for_each_vstream_in_ctx(vs, xse->ctx) {
		spin_lock(&vs->stream_lock);
		while ((vsm = xsched_vsm_fetch_first(vs))) {
			list_add_tail(&vsm->node, &xcu->vsm_list);
			scheduled++;
			xsched_dec_pending_kicks_xse(xse);
		}
		spin_unlock(&vs->stream_lock);
	}

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("After decrement XSE kick_count=%d @ %s\n",
		kick_count, __func__);

	xse->total_scheduled += scheduled;
	return scheduled;
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
