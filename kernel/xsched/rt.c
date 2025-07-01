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

/* Returns the next priority for pick_next_ctx taking into
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
			WARN_ON_ONCE(1);
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
		XSCHED_INFO("No pending kicks in RT class @ %s\n", __func__);
		return NULL;
	}

	if (!xcu->xrq.rt.prio_nr_running[next_prio]) {
		XSCHED_ERR(
			"In RT runqueue nr_running is zero while there are pending kicks for %u prio\n",
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
	xse->rt.kick_slice -= atomic_read(&xse->kicks_submited);
	XSCHED_INFO(
		"Update XSE=%d kick_slice=%lld, XSE kicks_submited=%d in RT class @ %s\n",
		xse->tgid, xse->rt.kick_slice,
		atomic_read(&xse->kicks_submited), __func__);

	if (xse->rt.kick_slice <= 0) {
		xse->rt.kick_slice = XSCHED_RT_KICK_SLICE;
		XSCHED_INFO("Refill XSE=%d kick_slice=%lld in RT class @ %s\n",
			    xse->tgid, xse->rt.kick_slice, __func__);
		xse_rt_move_tail(xse);
	}

	atomic_set(&xse->kicks_submited, 0);
}

static int submit_prepare_ctx_rt(struct xsched_entity *xse,
								struct xsched_cu *xcu)
{
	if (!atomic_read(&xse->kicks_pending_ctx_cnt)) {
		XSCHED_INFO("xse %d doesn't have pending kicks @ %s\n",
			    xse->tgid, __func__);
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
	uint32_t kick_count;
	struct vstream_info *vs;
	size_t kicks_submitted = 0;
	struct vstream_metadata *vsm;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_INFO("Before decrement XSE kick_count=%u @ %s\n",
			    kick_count, __func__);

	if (kick_count == 0) {
		XSCHED_ERR("Tried to submit xse that has 0 kicks @ %s\n",
			   __func__);
		goto out_err;
	}

	for_each_vstream_in_ctx(vs, xse->ctx) {
		spin_lock(&vs->stream_lock);
		while ((vsm = xsched_vsm_fetch_first(vs))) {
			list_add_tail(&vsm->node, &xcu->vsm_list);
			kicks_submitted++;
			xsched_dec_pending_kicks_xse(xse);
			XSCHED_INFO(
				"vs id = %u Kick submit sq_tail %u sqe_num %u sq_id %u @ %s\n",
				vs->id, vsm->sq_tail, vsm->sqe_num, vsm->sq_id, __func__);
		}
		spin_unlock(&vs->stream_lock);
	}

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_INFO("After decrement XSE kick_count=%u @ %s\n",
		    kick_count, __func__);

	xse->kicks_submitted += kicks_submitted;

	XSCHED_INFO("xse %d kicks_submitted = %lu @ %s\n",
			    xse->tgid, xse->kicks_submitted, __func__);

out_err:
	return kicks_submitted;
}

const struct xsched_class rt_xsched_class = {
	.next = &fair_xsched_class,
	.dequeue_ctx = dequeue_ctx_rt,
	.enqueue_ctx = enqueue_ctx_rt,
	.pick_next_ctx = pick_next_ctx_rt,
	.put_prev_ctx = put_prev_ctx_rt,
	.submit_prepare_ctx = submit_prepare_ctx_rt,
	.select_work = select_work_rt,
	.check_preempt = check_preempt_ctx_rt
};
