/* SPDX-License-Identifier: GPL-2.0 */
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
#include <linux/hashtable.h>
#include <linux/sched.h>

#include "vstream.h"
#include "xsched.h"

#define XSCHED_RT_TIMESLICE (10 * NSEC_PER_MSEC)
#define TGID_HASH_BITS 8

/* Mapping between tgid and priority */
struct tgid_prio {
	pid_t tgid;
	int prio;
	struct hlist_node hnode;
};

static DEFINE_HASHTABLE(tgid_prio_map, TGID_HASH_BITS);
static DEFINE_SPINLOCK(tgid_prio_lock);

static int tgid_prio_insert(pid_t tgid, int prio)
{
	struct tgid_prio *new_map;
	unsigned int hash_key;

	if (prio > XSE_PRIO_LOW || prio < XSE_PRIO_HIGH) {
		XSCHED_ERR("Invalid priority\n");
		return -EINVAL;
	}

	new_map = kzalloc(sizeof(struct tgid_prio), GFP_KERNEL);
	if (!new_map) {
		XSCHED_ERR("Fail to alloc mapping (tgid=%d) @ %s\n",
			tgid, __func__);
		return -ENOMEM;
	}

	new_map->tgid = tgid;
	new_map->prio = prio;

	hash_key = hash_32(tgid, TGID_HASH_BITS);

	spin_lock(&tgid_prio_lock);
	hash_add_rcu(tgid_prio_map, &new_map->hnode, hash_key);
	spin_unlock(&tgid_prio_lock);

	return 0;
}

static struct tgid_prio *tgid_prio_find(pid_t tgid)
{
	struct tgid_prio *map = NULL;
	unsigned int hash_key = hash_32(tgid, TGID_HASH_BITS);

	rcu_read_lock();
	hash_for_each_possible_rcu(tgid_prio_map, map, hnode, hash_key) {
		if (map->tgid == tgid)
			break;
	}
	rcu_read_unlock();
	return map;
}

static void tgid_prio_delete(pid_t tgid)
{
	struct tgid_prio *map;
	unsigned int hash_key = hash_32(tgid, TGID_HASH_BITS);

	spin_lock(&tgid_prio_lock);
	hash_for_each_possible(tgid_prio_map, map, hnode, hash_key) {
		if (map->tgid == tgid) {
			hash_del_rcu(&map->hnode);
			spin_unlock(&tgid_prio_lock);
			kfree(map);
			return;
		}
	}
	spin_unlock(&tgid_prio_lock);
}

void tgid_prio_cleanup(void)
{
	struct tgid_prio *map;
	struct hlist_node *tmp;
	int i;

	spin_lock(&tgid_prio_lock);
	hash_for_each_safe(tgid_prio_map, i, tmp, map, hnode) {
		hash_del(&map->hnode);
		kfree(map);
	}
	spin_unlock(&tgid_prio_lock);
}

static inline void
xse_rt_add(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	list_add_tail(&xse->rt.list_node, &xcu->xrq.rt.rq[xse->rt.prio]);
}

static inline void xse_rt_del(struct xsched_entity *xse)
{
	list_del_init(&xse->rt.list_node);
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
}

/* Decrease RT runqueue total and per prio nr_running stat
 * and raise a bug if nr_running decrease beyond zero.
 */
static inline void xrq_dec_nr_running(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	xcu->xrq.rt.nr_running--;
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

static inline struct xsched_entity *xrq_next_xse(
	struct xsched_cu *xcu, int prio)
{
	return list_first_entry(&xcu->xrq.rt.rq[prio], struct xsched_entity,
				rt.list_node);
}

/* Return the next priority for pick_next_ctx taking into
 * account if there are pending kicks on certain priority.
 */
static inline uint32_t get_next_prio_rt(struct xsched_rq *xrq)
{
	unsigned int curr_prio;

	for_each_xse_prio(curr_prio) {
		if (!list_empty(&xrq->rt.rq[curr_prio]))
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

	result = xrq_next_xse(xcu, next_prio);
	if (!result)
		XSCHED_ERR("Next XSE not found @ %s\n", __func__);

	XSCHED_DEBUG("Next XSE %u at prio %u @ %s\n", result->tgid, next_prio, __func__);
	return result;
}

static void put_prev_ctx_rt(struct xsched_entity *xse)
{
	xse->rt.timeslice -= xse->last_exec_runtime;
	XSCHED_DEBUG(
		"Update XSE=%d timeslice=%lld, last_exec_time=%llu in RT class @ %s\n",
		xse->tgid, ktime_to_ns(xse->rt.timeslice),
		xse->last_exec_runtime, __func__);

	if (xse->rt.timeslice <= 0) {
		xse->rt.timeslice = XSCHED_RT_TIMESLICE;
		XSCHED_DEBUG("Refill XSE=%d timeslice=%lld in RT class @ %s\n",
			xse->tgid, ktime_to_ns(xse->rt.timeslice), __func__);
		xse_rt_move_tail(xse);
	}
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
	struct xcu_op_handler_params params;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("Before decrement XSE kick_count=%d @ %s\n",
		kick_count, __func__);

	if (kick_count == 0)
		return 0;

	for_each_vstream_in_ctx(vs, xse->ctx) {
		spin_lock(&vs->stream_lock);
		while ((vsm = xsched_vsm_fetch_first(vs))) {
			list_add_tail(&vsm->node, &xcu->vsm_list);
			scheduled++;
			xsched_dec_pending_kicks_xse(xse);
		}
		spin_unlock(&vs->stream_lock);
	}

	/*
	 * Iterate over all vstreams in context:
	 * Set wr_cqe bit in last computing task in vsm_list
	 */
	for_each_vstream_in_ctx(vs, xse->ctx) {
		list_for_each_entry_reverse(vsm, &xcu->vsm_list, node) {
			if (vsm->parent == vs) {
				params.group = vsm->parent->xcu->group;
				params.param_1 = &(int){SQE_SET_NOTIFY};
				params.param_2 = &vsm->sqe;
				xcu_sqe_op(&params);
				break;
			}
		}
	}

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	XSCHED_DEBUG("After decrement XSE kick_count=%d @ %s\n",
		kick_count, __func__);

	xse->total_scheduled += scheduled;
	return scheduled;
}

void rq_init_rt(struct xsched_cu *xcu)
{
	int prio = 0;

	xcu->xrq.rt.nr_running = 0;
	for_each_xse_prio(prio) {
		INIT_LIST_HEAD(&xcu->xrq.rt.rq[prio]);
	}
}

void xse_init_rt(struct xsched_entity *xse)
{
	struct tgid_prio *map = tgid_prio_find(xse->tgid);

	xse->rt.prio = (map) ? map->prio : XSE_PRIO_LOW;
	XSCHED_DEBUG("Xse init: set priority=%d.\n", xse->rt.prio);
	if (!map)
		tgid_prio_insert(xse->tgid, xse->rt.prio);
	xse->rt.timeslice = XSCHED_RT_TIMESLICE;
	INIT_LIST_HEAD(&xse->rt.list_node);
}

void xse_deinit_rt(struct xsched_entity *xse)
{
	struct tgid_prio *map = tgid_prio_find(xse->tgid);

	if (map) {
		tgid_prio_delete(xse->tgid);
		XSCHED_DEBUG("Map deleted: tgid=%d\n", xse->tgid);
	}
}

struct xsched_class rt_xsched_class = {
	.class_id = XSCHED_TYPE_RT,
	.rq_init = rq_init_rt,
	.xse_init = xse_init_rt,
	.xse_deinit = xse_deinit_rt,
	.dequeue_ctx = dequeue_ctx_rt,
	.enqueue_ctx = enqueue_ctx_rt,
	.pick_next_ctx = pick_next_ctx_rt,
	.put_prev_ctx = put_prev_ctx_rt,
	.check_preempt = check_preempt_ctx_rt
};

static pid_t convert_to_host_pid(pid_t tgid)
{
	struct pid *vpid;

	if (tgid < -1)
		return -1;

	vpid = (tgid > 0) ? find_vpid(tgid) : find_vpid(current->pid);
	if (!vpid)
		return -1;

	return pid_nr_ns(vpid, &init_pid_ns);
}

int xsched_rt_prio_set(pid_t tgid, unsigned int prio)
{
	unsigned int id;
	int rt_prio;
	struct xsched_cu *xcu;
	struct xsched_context *ctx;
	struct xsched_entity *xse;
	pid_t host_pid;

	host_pid = convert_to_host_pid(tgid);
	if (host_pid == -1)
		return -EINVAL;

	rt_prio = NR_XSE_PRIO - prio;
	tgid_prio_delete(host_pid);
	tgid_prio_insert(host_pid, rt_prio);

	for_each_active_xcu(xcu, id) {
		mutex_lock(&xcu->xcu_lock);
		mutex_lock(&xcu->ctx_list_lock);

		ctx = ctx_find_by_tgid_and_xcu(tgid, xcu);
		if (ctx) {
			xse = &ctx->xse;
			xse->rt.prio = prio;
			if (xse->on_rq) {
				xse_rt_del(xse);
				xse_rt_add(xse, xcu);
			}
		}

		mutex_unlock(&xcu->ctx_list_lock);
		mutex_unlock(&xcu->xcu_lock);
	}

	return 0;
}

int xsched_rt_prio_get(pid_t tgid)
{
	struct tgid_prio *map;
	pid_t host_pid;

	host_pid = convert_to_host_pid(tgid);
	if (host_pid == -1)
		return -EINVAL;

	map = tgid_prio_find(host_pid);
	if (!map)
		return -EINVAL;

	return NR_XSE_PRIO - map->prio;
}
