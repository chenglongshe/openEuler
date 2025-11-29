/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Core kernel scheduler code for XPU device
 */
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/delay.h>

#include "xcu_group.h"
#include "xsched.h"

int num_active_xcu;
static DEFINE_SPINLOCK(xcu_mgr_lock);

struct xsched_cu *xsched_cu_mgr[XSCHED_NR_CUS];
/* List of scheduling classes available */
struct list_head xsched_class_list;

static DEFINE_MUTEX(revmap_mutex);
static DEFINE_HASHTABLE(ctx_revmap, XCU_HASH_ORDER);

atomic_t pending_task_count = ATOMIC_INIT(0);

static void put_prev_ctx(struct xsched_entity *xse)
{
	struct xsched_cu *xcu = xse->xcu;

	lockdep_assert_held(&xcu->xcu_lock);
	xse->class->put_prev_ctx(xse);
	xse->last_exec_runtime = 0;
	atomic_set(&xse->submitted_one_kick, 0);
	XSCHED_DEBUG("Put current xse %d @ %s\n", xse->tgid, __func__);
}

static size_t select_work_def(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	int kick_count, scheduled = 0, not_empty;
	struct vstream_info *vs;
	struct xcu_op_handler_params params;
	struct vstream_metadata *vsm;
	size_t kick_slice = xse->class->kick_slice;

	kick_count = atomic_read(&xse->kicks_pending_ctx_cnt);
	if (kick_count == 0)
		return 0;

	do {
		not_empty = 0;
		for_each_vstream_in_ctx(vs, xse->ctx) {
			spin_lock(&vs->stream_lock);
			vsm = xsched_vsm_fetch_first(vs);
			spin_unlock(&vs->stream_lock);
			if (!vsm)
				continue;
			list_add_tail(&vsm->node, &xcu->vsm_list);
			scheduled++;
			xsched_dec_pending_kicks_xse(xse);
			not_empty++;
		}
	} while ((scheduled < kick_slice) && (not_empty));

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
	xse->total_scheduled += scheduled;

	return scheduled;
}

static struct xsched_entity *__raw_pick_next_ctx(struct xsched_cu *xcu)
{
	const struct xsched_class *class;
	struct xsched_entity *next = NULL;
	size_t scheduled;

	lockdep_assert_held(&xcu->xcu_lock);
	for_each_xsched_class(class) {
		next = class->pick_next_ctx(xcu);
		if (next) {
			scheduled = class->select_work ?
				class->select_work(xcu, next) : select_work_def(xcu, next);
			break;
		}
	}

	return next;
}

void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	lockdep_assert_held(&xcu->xcu_lock);

	if (xse_integrity_check(xse))
		return;

	if (!xse->on_rq) {
		xse->on_rq = true;
		xse->class->enqueue_ctx(xse, xcu);
	}
}

void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu)
{
	lockdep_assert_held(&xcu->xcu_lock);

	if (xse_integrity_check(xse))
		return;

	if (xse->on_rq) {
		xse->class->dequeue_ctx(xse);
		xse->on_rq = false;
	}
}

static int delete_ctx(struct xsched_context *ctx)
{
	struct xsched_cu *xcu = ctx->xse.xcu;
	struct xsched_entity *curr_xse = xcu->xrq.curr_xse;
	struct xsched_entity *xse = &ctx->xse;

	if (xse_integrity_check(xse)) {
		XSCHED_ERR("Fail to check xse integrity @ %s\n", __func__);
		return -EINVAL;
	}

	if (!xse->xcu) {
		XSCHED_ERR("Try to delete ctx that is not attached to xcu @ %s\n",
			__func__);
		return -EINVAL;
	}

	/* Wait till context has been submitted. */
	while (atomic_read(&xse->kicks_pending_ctx_cnt))
		usleep_range(100, 200);

	mutex_lock(&xcu->xcu_lock);
	dequeue_ctx(xse, xcu);
	if (curr_xse == xse)
		xcu->xrq.curr_xse = NULL;
	--xcu->nr_ctx;
	mutex_unlock(&xcu->xcu_lock);

	xse->class->xse_deinit(xse);

	return 0;
}

/* Frees a given vstream and also frees and dequeues it's context
 * if a given vstream is the last and only vstream attached to it's
 * corresponding context object.
 */
void xsched_task_free(struct kref *kref)
{
	struct xsched_context *ctx;
	vstream_info_t *vs, *tmp;
	struct xsched_cu *xcu;

	ctx = container_of(kref, struct xsched_context, kref);
	xcu = ctx->xse.xcu;

	mutex_lock(&xcu->ctx_list_lock);
	/* Wait for XSE to finish submitting */
	delete_ctx(ctx);
	list_for_each_entry_safe(vs, tmp, &ctx->vstream_list, ctx_node) {
		list_del(&vs->ctx_node);
		kfree(vs->data);
		kfree(vs);
	}

	list_del(&ctx->ctx_node);
	--xcu->nr_ctx;
	mutex_unlock(&xcu->ctx_list_lock);

	kfree(ctx);
	atomic_dec(&pending_task_count);
}

int ctx_bind_to_xcu(vstream_info_t *vstream_info, struct xsched_context *ctx)
{
	struct ctx_devid_revmap_data *revmap_data;
	struct xsched_cu *xcu_found = NULL;
	uint32_t type = XCU_TYPE_NPU;

	/* Find XCU history. */
	hash_for_each_possible(ctx_revmap, revmap_data, hash_node,
				(unsigned long)ctx->dev_id) {
		if (revmap_data && revmap_data->group) {
			/* Bind ctx to group xcu.*/
			ctx->xse.xcu = revmap_data->group->xcu;
			return 0;
		}
	}

	revmap_data = kzalloc(sizeof(struct ctx_devid_revmap_data), GFP_KERNEL);
	if (revmap_data == NULL) {
		XSCHED_ERR("Revmap_data is NULL @ %s\n", __func__);
		return -ENOMEM;
	}

	xcu_found = xcu_find(type, ctx->dev_id, vstream_info->channel_id);
	if (!xcu_found) {
		kfree(revmap_data);
		return -EINVAL;
	}

	/* Bind ctx to an XCU from channel group. */
	revmap_data->group = xcu_found->group;
	ctx->xse.xcu = xcu_found;
	vstream_info->xcu = xcu_found;
	revmap_data->dev_id = vstream_info->dev_id;
	XSCHED_DEBUG("Ctx bind to xcu %u @ %s\n", xcu_found->id, __func__);

	hash_add(ctx_revmap, &revmap_data->hash_node,
		(unsigned long)ctx->dev_id);

	return 0;
}

int vstream_bind_to_xcu(vstream_info_t *vstream_info)
{
	struct xsched_cu *xcu_found = NULL;
	uint32_t type = XCU_TYPE_NPU;

	xcu_found = xcu_find(type, vstream_info->dev_id, vstream_info->channel_id);
	if (!xcu_found)
		return -EINVAL;

	/* Bind vstream to a xcu. */
	vstream_info->xcu = xcu_found;
	vstream_info->dev_id = xcu_found->id;
	XSCHED_DEBUG("XCU bound to a vstream: type=%u, dev_id=%u, chan_id=%u.\n",
		type, vstream_info->dev_id, vstream_info->channel_id);

	return 0;
}

struct xsched_cu *xcu_find(
	uint32_t type, uint32_t dev_id, uint32_t channel_id)
{
	struct xcu_group *group = NULL;

	/* Find xcu by type. */
	group = xcu_group_find(xcu_group_root, type);
	if (group == NULL) {
		XSCHED_ERR("Fail to find type group.\n");
		return NULL;
	}

	/* Find device id group. */
	group = xcu_group_find(group, dev_id);
	if (group == NULL) {
		XSCHED_ERR("Fail to find device group.\n");
		return NULL;
	}
	/* Find channel id group. */
	group = xcu_group_find(group, channel_id);
	if (group == NULL) {
		XSCHED_ERR("Fail to find channel group.\n");
		return NULL;
	}

	XSCHED_DEBUG("XCU found: type=%u, dev_id=%u, chan_id=%u.\n",
		type, dev_id, channel_id);

	return group->xcu;
}

int xsched_xse_set_class(struct xsched_entity *xse)
{
	struct xsched_class *sched = xsched_first_class;

	xse->class = sched;
	return 0;
}

int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs)
{
	int err = 0;
	struct xsched_entity *xse = &ctx->xse;

	atomic_set(&xse->kicks_pending_ctx_cnt, 0);
	atomic_set(&xse->submitted_one_kick, 0);

	xse->total_scheduled = 0;
	xse->total_submitted = 0;
	xse->last_exec_runtime = 0;

	xse->fd = ctx->fd;
	xse->tgid = ctx->tgid;

	err = ctx_bind_to_xcu(vs, ctx);
	if (err) {
		XSCHED_ERR(
			"Couldn't find valid xcu for vstream %u dev_id %u @ %s\n",
			vs->id, vs->dev_id, __func__);
		return -EINVAL;
	}

	xse->ctx = ctx;
	BUG_ON(vs->xcu == NULL);
	xse->xcu = vs->xcu;

	err = xsched_xse_set_class(xse);
	if (err) {
		XSCHED_ERR("Fail to set xse class @ %s\n", __func__);
		return err;
	}
	xse->class->xse_init(xse);

	WRITE_ONCE(xse->on_rq, false);

	spin_lock_init(&xse->xse_lock);
	return err;
}

static void submit_kick(struct vstream_metadata *vsm)
{
	struct vstream_info *vs = vsm->parent;
	struct xcu_op_handler_params params;

	params.group = vs->xcu->group;
	params.fd = vs->fd;
	params.param_1 = &vs->id;
	params.param_2 = &vs->channel_id;
	params.param_3 = vsm->sqe;
	params.param_4 = &vsm->sqe_num;
	params.param_5 = &vsm->timeout;
	params.param_6 = &vs->sqcq_type;
	params.param_7 = vs->drv_ctx;
	params.param_8 = &vs->logic_vcq_id;

	/* Send vstream on a device for processing. */
	if (xcu_run(&params) != 0)
		XSCHED_ERR(
			"Fail to send Vstream id %u tasks to a device for processing.\n",
			vs->id);

	XSCHED_DEBUG("Vstream id %u submit vsm: sq_tail %u\n", vs->id, vsm->sq_tail);
}

static void submit_wait(struct vstream_metadata *vsm)
{
	struct vstream_info *vs = vsm->parent;
	struct xcu_op_handler_params params;
	/* Wait timeout in ms. */
	int32_t timeout = XSCHED_WAIT_TIMEOUT;

	params.group = vs->xcu->group;
	params.param_1 = &vs->channel_id;
	params.param_2 = &vs->logic_vcq_id;
	params.param_3 = &vs->user_stream_id;
	params.param_4 = &vsm->sqe;
	params.param_5 = vsm->cqe;
	params.param_6 = vs->drv_ctx;
	params.param_7 = &timeout;

	/* Wait for a device to complete processing. */
	if (xcu_wait(&params))
		XSCHED_WARN("Fail to wait Vstream id %u tasks, logic_cq_id %u.\n",
			vs->id, vs->logic_vcq_id);

	XSCHED_DEBUG("Vstream id %u wait finish, logic_cq_id %u\n",
		vs->id, vs->logic_vcq_id);
}

static int __xsched_submit(struct xsched_cu *xcu, struct xsched_entity *xse)
{
	struct vstream_metadata *vsm, *tmp;
	int submitted = 0;
	long submit_exec_time = 0;
	ktime_t t_start = 0;
	struct xcu_op_handler_params params;

	XSCHED_DEBUG("%s called for xse %d on xcu %u\n",
		__func__, xse->tgid, xcu->id);
	list_for_each_entry_safe(vsm, tmp, &xcu->vsm_list, node) {
		submit_kick(vsm);
		XSCHED_DEBUG("Xse %d vsm %u sched_delay: %lld ns\n",
			xse->tgid, vsm->sq_id, ktime_to_ns(ktime_sub(ktime_get(), vsm->add_time)));

		params.group = vsm->parent->xcu->group;
		params.param_1 = &(int){SQE_IS_NOTIFY};
		params.param_2 = &vsm->sqe;
		if (xcu_sqe_op(&params)) {
			mutex_unlock(&xcu->xcu_lock);
			t_start = ktime_get();
			submit_wait(vsm);
			submit_exec_time += ktime_to_ns(ktime_sub(ktime_get(), t_start));
			mutex_lock(&xcu->xcu_lock);
		}
		submitted++;
		list_del(&vsm->node);
		kfree(vsm);
	}

	xse->last_exec_runtime += submit_exec_time;
	xse->total_submitted += submitted;
	atomic_add(submitted, &xse->submitted_one_kick);
	INIT_LIST_HEAD(&xcu->vsm_list);
	XSCHED_DEBUG("Xse %d submitted=%d total=%zu, exec_time=%ld @ %s\n",
		xse->tgid, submitted, xse->total_submitted,
		submit_exec_time, __func__);

	return submitted;
}

static bool xcu_has_running(struct xsched_cu *xcu)
{
	bool ret = false;
	struct xsched_class *sched;

	mutex_lock(&xcu->xcu_lock);
	for_each_xsched_class(sched) {
		ret |= sched->has_running(xcu);
	}
	mutex_unlock(&xcu->xcu_lock);

	return ret;
}

static int xsched_schedule(void *input_xcu)
{
	struct xsched_cu *xcu = input_xcu;
	struct xsched_entity *curr_xse = NULL;
	struct xsched_entity *next_xse = NULL;

	while (!kthread_should_stop()) {
		mutex_unlock(&xcu->xcu_lock);
		wait_event_interruptible(xcu->wq_xcu_idle,
			xcu_has_running(xcu) || kthread_should_stop());

		mutex_lock(&xcu->xcu_lock);
		if (kthread_should_stop()) {
			mutex_unlock(&xcu->xcu_lock);
			break;
		}

		if (!xsched_check_pending_kicks_xcu(xcu)) {
			XSCHED_WARN("%s: No pending kicks on xcu %u\n", __func__, xcu->id);
			continue;
		}

		next_xse = __raw_pick_next_ctx(xcu);
		if (!next_xse) {
			XSCHED_WARN("%s: Couldn't find next xse on xcu %u\n", __func__, xcu->id);
			continue;
		}

		xcu->xrq.curr_xse = next_xse;
		__xsched_submit(xcu, next_xse);

		curr_xse = xcu->xrq.curr_xse;
		if (!curr_xse)
			continue;

		/* if not deleted yet */
		put_prev_ctx(curr_xse);
		if (!atomic_read(&curr_xse->kicks_pending_ctx_cnt))
			dequeue_ctx(curr_xse, xcu);

		xcu->xrq.curr_xse = NULL;
	}

	return 0;
}

/* Initializes all xsched XCU objects.
 * Should only be called from xsched_xcu_register function.
 */
static int xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group,
	int xcu_id)
{
	struct xsched_class *sched;
	int err;

	xcu->id = xcu_id;
	xcu->state = XSCHED_XCU_NONE;
	xcu->group = group;
	xcu->nr_ctx = 0;

	atomic_set(&xcu->pending_kicks, 0);
	INIT_LIST_HEAD(&xcu->vsm_list);
	INIT_LIST_HEAD(&xcu->ctx_list);
	init_waitqueue_head(&xcu->wq_xcu_idle);
	mutex_init(&xcu->xcu_lock);
	mutex_init(&xcu->ctx_list_lock);
	mutex_init(&xcu->vs_array_lock);

	/* Initialize current XCU's runqueue. */
	for_each_xsched_class(sched) {
		sched->rq_init(xcu);
	}

	xcu->xrq.curr_xse = NULL;

	/* This worker should set XCU to XSCHED_XCU_WAIT_IDLE.
	 * If after initialization XCU still has XSCHED_XCU_NONE
	 * status then we can assume that there was a problem
	 * with XCU kthread job.
	 */
	xcu->worker = kthread_run(xsched_schedule, xcu, "xcu_%u", xcu->id);
	if (IS_ERR(xcu->worker)) {
		err = PTR_ERR(xcu->worker);
		xcu->worker = NULL;
		return err;
	}
	return 0;
}

/* Increment xcu id */
static int nr_active_cu_inc(void)
{
	int cur_num = -1;

	spin_lock(&xcu_mgr_lock);
	if (num_active_xcu >= XSCHED_NR_CUS)
		goto out_unlock;

	cur_num = num_active_xcu;
	num_active_xcu++;

out_unlock:
	spin_unlock(&xcu_mgr_lock);
	return cur_num;
}

static int nr_active_cu_dec(void)
{
	int cur_num = -1;

	spin_lock(&xcu_mgr_lock);
	if (num_active_xcu <= 0)
		goto out_unlock;

	cur_num = num_active_xcu;
	num_active_xcu--;

out_unlock:
	spin_unlock(&xcu_mgr_lock);
	return cur_num;
}

/* Adds vstream_metadata object to a specified vstream. */
int xsched_vsm_add_tail(struct vstream_info *vs, vstream_args_t *arg)
{
	struct vstream_metadata *new_vsm;

	new_vsm = kmalloc(sizeof(struct vstream_metadata), GFP_ATOMIC);
	if (!new_vsm)
		return -ENOMEM;

	if (vs->kicks_count > MAX_VSTREAM_SIZE) {
		kfree(new_vsm);
		return -EBUSY;
	}

	xsched_init_vsm(new_vsm, vs, arg);
	list_add_tail(&new_vsm->node, &vs->metadata_list);
	new_vsm->add_time = ktime_get();
	vs->kicks_count += 1;

	return 0;
}

/* Fetch the first vstream metadata from vstream metadata list
 * and removes it from that list. Returned vstream metadata pointer
 * to be freed after.
 */
struct vstream_metadata *xsched_vsm_fetch_first(struct vstream_info *vs)
{
	struct vstream_metadata *vsm;

	if (list_empty(&vs->metadata_list))
		return NULL;

	vsm = list_first_entry(&vs->metadata_list, struct vstream_metadata, node);
	if (!vsm)
		return NULL;

	list_del(&vsm->node);
	if (vs->kicks_count > 0)
		vs->kicks_count -= 1;

	return vsm;
}

static void xsched_register_sched_class(struct xsched_class *sched)
{
	list_add(&sched->node, &xsched_class_list);
}

/*
 * Initialize and register xcu in xcu_manager array.
 */
int xsched_xcu_register(struct xcu_group *group, uint32_t phys_id)
{
	int xcu_cur_num, ret = 0;
	struct xsched_cu *xcu;

	if (phys_id >= XSCHED_NR_CUS || !group)
		return -EINVAL;

	xcu_cur_num = nr_active_cu_inc();
	if (xcu_cur_num < 0) {
		XSCHED_ERR("Number of present XCU's exceeds %d: %d.\n",
			XSCHED_NR_CUS, num_active_xcu);
		return -ENOSPC;
	};

	xcu = kzalloc(sizeof(struct xsched_cu), GFP_KERNEL);
	if (!xcu) {
		nr_active_cu_dec();
		XSCHED_ERR("Fail to alloc xcu.\n");
		return -ENOMEM;
	}

	group->xcu = xcu;
	xsched_cu_mgr[phys_id] = xcu;

	/* Init xcu's internals. */
	ret = xsched_xcu_init(xcu, group, phys_id);
	if (ret != 0) {
		group->xcu = NULL;
		xsched_cu_mgr[phys_id] = NULL;
		kfree(xcu);
	}
	return ret;
}

int xsched_xcu_unregister(struct xcu_group *group, uint32_t phys_id)
{
	struct xsched_cu *xcu;

	if (phys_id >= XSCHED_NR_CUS)
		return -EINVAL;

	if (!group || !group->xcu || group->xcu != xsched_cu_mgr[phys_id])
		return -EINVAL;

	if (nr_active_cu_dec() < 0) {
		XSCHED_ERR("No active XCU\n");
		return -EPERM;
	};

	xcu = group->xcu;
	mutex_lock(&xcu->xcu_lock);
	wake_up_interruptible(&xcu->wq_xcu_idle);
	mutex_unlock(&xcu->xcu_lock);

	group->xcu = NULL;
	xsched_cu_mgr[phys_id] = NULL;
	kthread_stop(xcu->worker);
	xcu->worker = NULL;
	kfree(xcu);

	return 0;
}

int xsched_sched_init(void)
{
	INIT_LIST_HEAD(&xsched_class_list);
	xsched_register_sched_class(&rt_xsched_class);

	return 0;
}
