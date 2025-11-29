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
#include <linux/pid_namespace.h>

#include "vstream.h"
#include "xsched.h"

#define XSCHED_RT_TIMESLICE (10 * NSEC_PER_MSEC)
#define TGID_HASH_BITS 8

#define INIT_PID 1

/* Mapping between tgid and priority */
struct tgid_prio {
	pid_t tgid;
	int prio;

	/* priority of init process in namespace */
	struct tgid_prio *ns_prio;
	atomic_t task_cnt;
	refcount_t refcnt;
	struct hlist_node hnode;
};

static DEFINE_HASHTABLE(tgid_prio_map, TGID_HASH_BITS);
static DEFINE_SPINLOCK(tgid_prio_lock);

static int tgid_prio_insert(pid_t tgid, int prio, struct tgid_prio *ns_prio)
{
	struct tgid_prio *new_map;
	unsigned int hash_key;

	new_map = kzalloc(sizeof(struct tgid_prio), GFP_KERNEL);
	if (!new_map)
		return -ENOMEM;

	new_map->tgid = tgid;
	new_map->prio = prio;
	new_map->ns_prio = ns_prio;
	refcount_set(&new_map->refcnt, 1);
	atomic_set(&new_map->task_cnt, 0);
	if (ns_prio)
		atomic_inc(&ns_prio->task_cnt);

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

static struct tgid_prio *tgid_prio_get(pid_t tgid)
{
	struct tgid_prio *map = NULL;
	unsigned int hash_key = hash_32(tgid, TGID_HASH_BITS);

	rcu_read_lock();
	hash_for_each_possible_rcu(tgid_prio_map, map, hnode, hash_key) {
		if (map->tgid == tgid) {
			refcount_inc(&map->refcnt);
			break;
		}
	}
	rcu_read_unlock();
	return map;
}

static void tgid_prio_put(struct tgid_prio *map)
{
	refcount_dec(&map->refcnt);
}

static void tgid_prio_delete(pid_t tgid)
{
	struct tgid_prio *map;
	unsigned int hash_key = hash_32(tgid, TGID_HASH_BITS);

	spin_lock(&tgid_prio_lock);
	hash_for_each_possible(tgid_prio_map, map, hnode, hash_key) {
		if (map->tgid == tgid) {
			if (refcount_read(&map->refcnt) > 1 || atomic_read(&map->task_cnt) > 0) {
				spin_unlock(&tgid_prio_lock);
				return;
			}
			hash_del_rcu(&map->hnode);
			spin_unlock(&tgid_prio_lock);
			if (map->ns_prio)
				atomic_dec(&map->ns_prio->task_cnt);
			kfree(map);
			return;
		}
	}
	spin_unlock(&tgid_prio_lock);
}

static pid_t convert_to_host_pid(pid_t tgid)
{
	struct pid *vpid;
	pid_t host_pid = -1;

	rcu_read_lock();
	vpid = (tgid == -1) ? find_pid_ns(current->pid, &init_pid_ns) : find_vpid(tgid);
	if (vpid)
		host_pid = pid_nr_ns(vpid, &init_pid_ns);
	rcu_read_unlock();

	return host_pid;
}

/* Find the host PID corresponding to init process in the namespace. */
static struct tgid_prio *find_namespace_prio(void)
{
	struct pid_namespace *curr_ns;
	struct pid *ns_init_pid;
	pid_t host_pid = 0;

	rcu_read_lock();
	curr_ns = task_active_pid_ns(current);
	if (!curr_ns)
		goto out_unlock;

	if (curr_ns == &init_pid_ns) {
		host_pid = INIT_PID;
		goto out_unlock;
	}

	ns_init_pid = find_pid_ns(INIT_PID, curr_ns);
	if (!ns_init_pid)
		goto out_unlock;

	host_pid = pid_nr_ns(ns_init_pid, &init_pid_ns);

out_unlock:
	rcu_read_unlock();
	return (host_pid > 0) ? tgid_prio_find(host_pid) : NULL;
}

bool xsched_namespace_enabled(void)
{
	return find_namespace_prio() != NULL;
}

void tgid_prio_init(void)
{
	tgid_prio_insert(INIT_PID, XSE_DEFAULT_PRIO, NULL);
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

static void dequeue_ctx_rt(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	xse_rt_del(xse);
	xcu->xrq.rt.nr_running--;
}

static void enqueue_ctx_rt(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	xse_rt_add(xse, xcu);
	xcu->xrq.rt.nr_running++;
}

static inline bool has_running_rt(struct xsched_cu *xcu)
{
	return !!xcu->xrq.rt.nr_running;
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

void rq_init_rt(struct xsched_cu *xcu)
{
	int prio = 0;

	xcu->xrq.rt.nr_running = 0;
	for_each_xse_prio(prio) {
		INIT_LIST_HEAD(&xcu->xrq.rt.rq[prio]);
	}
}

static int __rt_sched_prio_find(pid_t tgid)
{
	struct tgid_prio *map;

	map = tgid_prio_find(tgid);
	if (map)
		return map->prio;

	map = find_namespace_prio();

	return map ? map->prio : -EINVAL;
}

void xse_init_rt(struct xsched_entity *xse)
{
	struct tgid_prio *map;

	map = tgid_prio_find(xse->tgid);
	if (!map) {
		map = find_namespace_prio();
		tgid_prio_insert(xse->tgid, map->prio, map);
	}
	xse->rt.prio = map->prio;
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
	.kick_slice = XSCHED_RT_KICK_SLICE,
	.rq_init = rq_init_rt,
	.xse_init = xse_init_rt,
	.xse_deinit = xse_deinit_rt,
	.dequeue_ctx = dequeue_ctx_rt,
	.enqueue_ctx = enqueue_ctx_rt,
	.pick_next_ctx = pick_next_ctx_rt,
	.put_prev_ctx = put_prev_ctx_rt,
	.has_running = has_running_rt,
};

static void __rt_sched_prio_set(pid_t tgid, int new_prio)
{
	unsigned int id;
	struct xsched_cu *xcu;
	struct xsched_context *ctx;
	struct xsched_entity *xse;

	for_each_active_xcu(xcu, id) {
		mutex_lock(&xcu->ctx_list_lock);
		mutex_lock(&xcu->xcu_lock);

		ctx = ctx_find_by_tgid_and_xcu(tgid, xcu);
		if (ctx) {
			xse = &ctx->xse;
			xse->rt.prio = new_prio;
			if (xse->on_rq) {
				xse_rt_del(xse);
				xse_rt_add(xse, xcu);
			}
		}

		mutex_unlock(&xcu->xcu_lock);
		mutex_unlock(&xcu->ctx_list_lock);
	}
}

static void update_all_task_in_ns(struct tgid_prio *ns_prio, int new_prio)
{
	struct tgid_prio *map;
	pid_t *update_list = NULL;
	int i, task_idx = 0;

	if (ns_prio)
		update_list = kmalloc_array(
			atomic_read(&ns_prio->task_cnt), sizeof(pid_t), GFP_KERNEL);
	if (!update_list)
		return;

	spin_lock(&tgid_prio_lock);
	hash_for_each(tgid_prio_map, i, map, hnode) {
		if (map->ns_prio && map->ns_prio == ns_prio) {
			map->prio = new_prio;
			update_list[task_idx++] = map->tgid;
		}
	}
	spin_unlock(&tgid_prio_lock);

	for (i = 0; i < task_idx; i++)
		__rt_sched_prio_set(update_list[i], new_prio);

	kfree(update_list);
}

int xsched_rt_prio_set(pid_t tgid, unsigned int prio)
{
	struct tgid_prio *ns_prio, *old_prio;
	int new_prio;
	pid_t host_pid;

	if (tgid == INIT_PID)
		return -EPERM;

	host_pid = convert_to_host_pid(tgid);
	if (host_pid == -1)
		return -EINVAL;

	new_prio = NR_XSE_PRIO - prio;
	if (new_prio > XSE_PRIO_LOW || new_prio < XSE_PRIO_HIGH) {
		XSCHED_ERR("Invalid priority\n");
		return -EINVAL;
	}

	old_prio = tgid_prio_get(host_pid);
	if (old_prio && old_prio->prio == new_prio)
		goto out_put;

	if (!old_prio) {
		ns_prio = find_namespace_prio();
		tgid_prio_insert(host_pid, new_prio, ns_prio);
		return 0;
	}

	old_prio->prio = new_prio;
	if (atomic_read(&old_prio->task_cnt) > 0) {
		/* Update all tasks if the modification is made to the namespace. */
		update_all_task_in_ns(old_prio, new_prio);
	} else {
		__rt_sched_prio_set(host_pid, new_prio);
	}

out_put:
	tgid_prio_put(old_prio);
	return 0;
}

int xsched_rt_prio_get(pid_t tgid)
{
	pid_t host_pid;

	if (tgid == INIT_PID)
		return -EPERM;

	host_pid = convert_to_host_pid(tgid);
	if (host_pid == -1)
		return -EINVAL;

	return NR_XSE_PRIO - __rt_sched_prio_find(host_pid);
}
