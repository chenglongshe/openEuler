/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_XSCHED_H__
#define __LINUX_XSCHED_H__

#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/xcu_group.h>
#include <linux/cgroup.h>
#include <linux/vstream.h>

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#ifdef CONFIG_XCU_VSTREAM
#define MAX_VSTREAM_NUM (512)
#endif

#define XSCHED_ERR_PREFIX "XSched [ERROR]: "
#define XSCHED_ERR(fmt, ...)                                                   \
	pr_err(pr_fmt(XSCHED_ERR_PREFIX fmt), ##__VA_ARGS__)

#define XSCHED_WARN_PREFIX "XSched [WARNING]: "
#define XSCHED_WARN(fmt, ...)                                                  \
	pr_warn(pr_fmt(XSCHED_WARN_PREFIX fmt), ##__VA_ARGS__)

/*
 * Debug specific prints for XSched
 */

#define XSCHED_DEBUG_PREFIX "XSched [DEBUG]: "
#define XSCHED_DEBUG(fmt, ...)                                                 \
	pr_debug(pr_fmt(XSCHED_DEBUG_PREFIX fmt), ##__VA_ARGS__)

#define XSCHED_CALL_STUB()                                                     \
	XSCHED_DEBUG(" -----* %s @ %s called *-----\n", __func__, __FILE__)

#define XSCHED_EXIT_STUB()                                                     \
	XSCHED_DEBUG(" -----* %s @ %s exited *-----\n", __func__, __FILE__)

#define XCU_HASH_ORDER 6

#define RUNTIME_INF ((u64)~0ULL)
#define XSCHED_TIME_INF RUNTIME_INF
#define XSCHED_CFS_ENTITY_WEIGHT_DFLT 1
#define XSCHED_CFS_QUOTA_PERIOD_MS (100 * NSEC_PER_MSEC)
#define XSCHED_CFG_SHARE_DFLT 1024

#define __GET_VS_TASK_TYPE(t) ((t)&0xFF)
#define __GET_VS_TASK_PRIO_RT(t) (((t) >> 8) & 0xFF)
#define GET_VS_TASK_TYPE(vs_ptr) __GET_VS_TASK_TYPE((vs_ptr)->task_type)
#define GET_VS_TASK_PRIO_RT(vs_ptr) __GET_VS_TASK_PRIO_RT((vs_ptr)->task_type)

/*
 * A default kick slice for RT class XSEs.
 */
#define XSCHED_RT_KICK_SLICE 20
/*
 * A default kick slice for CFS class XSEs.
 */
#define XSCHED_CFS_KICK_SLICE 10

extern struct xsched_cu *xsched_cu_mgr[XSCHED_NR_CUS];

enum xcu_sched_type {
	XSCHED_TYPE_RT,
	XSCHED_TYPE_DFLT = XSCHED_TYPE_RT,
	XSCHED_TYPE_CFS,
	XSCHED_TYPE_NUM,
};

enum xse_prio {
	XSE_PRIO_LOW,
	XSE_PRIO_HIGH,
	NR_XSE_PRIO,
};

enum xsched_rq_state {
	XRQ_STATE_INACTIVE = 0x00,
	XRQ_STATE_IDLE = 0x01,
	XRQ_STATE_BUSY = 0x02,
	XRQ_STATE_SUBMIT = 0x04,
	XRQ_STATE_WAIT_RUNNING = 0x08,
};

enum xse_state {
	XSE_PREPARE,
	XSE_READY,
	XSE_RUNNING,
	XSE_BLOCK,
	XSE_DEAD,
};

enum xse_flag {
	XSE_TIF_NONE,
	XSE_TIF_PREEMPT,
	XSE_TIF_BALANCE, /* Unused so far */
};

extern const struct xsched_class rt_xsched_class;
extern const struct xsched_class fair_xsched_class;

#define xsched_first_class (&rt_xsched_class)
#define for_each_xsched_class(class)                                           \
	for (class = xsched_first_class; class; class = class->next)
#define for_each_xse_prio(prio)                                                \
	for (prio = XSE_PRIO_LOW; prio < NR_XSE_PRIO; prio++)
#define for_each_vstream_in_ctx(vs, ctx)                                       \
	list_for_each_entry((vs), &((ctx)->vstream_list), ctx_node)

/* Manages xsched CFS-like class rbtree based runqueue. */
struct xsched_rq_cfs {
	unsigned int nr_running;
	unsigned int load;
	u64 min_xruntime;
	struct rb_root_cached ctx_timeline;
};

/* Manages xsched RT-like class linked list based runqueue.
 *
 * Now RT-like class runqueue structs is identical
 * but will most likely grow different in the
 * future as the Xsched evolves.
 */
struct xsched_rq_rt {
	struct list_head rq[NR_XSE_PRIO];
	unsigned int nr_running;
	int prio_nr_running[NR_XSE_PRIO];
	atomic_t prio_nr_kicks[NR_XSE_PRIO];
	DECLARE_BITMAP(curr_prios, NR_XSE_PRIO);
};

/* Base XSched runqueue object structure that contains both mutual and
 * individual parameters for different scheduling classes.
 */
struct xsched_rq {
	struct xsched_entity *curr_xse;
	const struct xsched_class *class;

	int state;

	/* RT class run queue.*/
	struct xsched_rq_rt rt;
	/* CFS class run queue.*/
	struct xsched_rq_cfs cfs;
};

enum xcu_state {
	XCU_INACTIVE,
	XCU_IDLE,
	XCU_BUSY,
	XCU_SUBMIT,
};

enum xsched_cu_status {
	/* Worker not initialized. */
	XSCHED_XCU_NONE,

	/* Worker is sleeping in idle state. */
	XSCHED_XCU_WAIT_IDLE,

	/* Worker is sleeping in running state. */
	XSCHED_XCU_WAIT_RUNNING,

	/* Worker is active but not processing anything. */
	XSCHED_XCU_ACTIVE,

	NR_XSCHED_XCU_STATUS,
};

/* This is the abstraction object of the xcu computing unit. */
struct xsched_cu {
	uint32_t id;
	uint32_t state;

	/* RT class kick counter. */
	atomic_t pending_kicks_rt;
	/* CFS class kick counter. */
	atomic_t pending_kicks_cfs;

	struct task_struct *worker;

	/* Storage list for contexts associated with this xcu */
	uint32_t nr_ctx;
	struct list_head ctx_list;
	struct mutex ctx_list_lock;

#ifdef CONFIG_XCU_VSTREAM
	vstream_info_t *vs_array[MAX_VSTREAM_NUM];
	struct mutex vs_array_lock;
#endif

	struct xsched_rq xrq;
	struct list_head vsm_list;

	struct xcu_group *group;

	struct mutex xcu_lock;

	wait_queue_head_t wq_xcu_idle;
	wait_queue_head_t wq_xcu_running;
};

extern int num_active_xcu;
#define for_each_active_xcu(xcu, id)                                           \
	for ((id) = 0, xcu = xsched_cu_mgr[(id)];                                  \
	     (id) < num_active_xcu && (xcu = xsched_cu_mgr[(id)]); (id)++)

struct xsched_entity_rt {
	struct list_head list_node;
	enum xse_state state;
	enum xse_flag flag;
	enum xse_prio prio;

	ktime_t timeslice;
	s64 kick_slice;
};

struct xsched_entity_cfs {
	struct rb_node run_node;

	/* Rq on which this entity is (to be) queued. */
	struct xsched_rq_cfs *cfs_rq;

	/* Value of "virtual" runtime to sort entities in rbtree */
	u64 xruntime;
	u32 weight;

	/* Execution time of scheduling entity */
	u64 exec_start;
	u64 sum_exec_runtime;
};

struct xsched_entity {
	uint32_t task_type;

	bool on_rq;

	pid_t owner_pid;
	pid_t tgid;

	/* Amount of pending kicks currently sitting on this context. */
	atomic_t kicks_pending_ctx_cnt;

	/* Amount of submitted kicks context, used for resched decision. */
	atomic_t submitted_one_kick;

	size_t total_scheduled;
	size_t total_submitted;

	/* File descriptor coming from an associated context
	 * used for identifying a given xsched entity in
	 * info and error prints.
	 */
	uint32_t fd;

	/* Xsched class for this xse. */
	const struct xsched_class *class;

	/* RT class entity. */
	struct xsched_entity_rt rt;
	/* CFS class entity. */
	struct xsched_entity_cfs cfs;

	/* Pointer to context object. */
	struct xsched_context *ctx;

	/* Xsched entity execution statistics */
	u64 last_exec_runtime;

	/* Pointer to an XCU object that represents an XCU
	 * on which this xse is to be processed or is being
	 * processed currently.
	 */
	struct xsched_cu *xcu;

	/* Link to list of xsched_group items */
	struct list_head group_node;
	struct xsched_group *parent_grp;
	bool is_group;

	/* General purpose xse lock. */
	spinlock_t xse_lock;
};

static inline bool xse_is_rt(const struct xsched_entity *xse)
{
	return xse && xse->class == &rt_xsched_class;
}

static inline bool xse_is_cfs(const struct xsched_entity *xse)
{
	return xse && xse->class == &fair_xsched_class;
}

/* xsched_group's xcu related stuff */
struct xsched_group_xcu_priv {
	/* Owner of this group */
	struct xsched_group *self;

	/* xcu id */
	int xcu_id;

	/* Link to scheduler */
	struct xsched_entity xse; /* xse of this group on runqueue */
	struct xsched_rq_cfs *cfs_rq; /* cfs runqueue "owned" by this group */
	struct xsched_rq_rt *rt_rq; /* rt runqueue "owned" by this group */

	/* Statistics */
	int nr_throttled;
	u64 throttled_time;
	u64 overrun_time;
};

/* Xsched scheduling control group */
struct xsched_group {
	/* Cgroups controller structure */
	struct cgroup_subsys_state css;

	/* Control group settings: */
	int sched_type;
	int prio;

	/* Bandwidth setting: shares value set by user */
	u64 shares_cfg;
	u64 shares_cfg_red;
	u32 weight;
	u64 children_shares_sum;

	/* Bandwidth setting: maximal quota in period */
	s64 quota;
	/* record the runtime of operators during the period */
	s64 runtime;
	s64 period;
	struct hrtimer quota_timeout;
	struct work_struct refill_work;
	u64 qoslevel;

	struct xsched_group_xcu_priv perxcu_priv[XSCHED_NR_CUS];

	/* Groups hierarchcy */
	struct xsched_group *parent;
	struct list_head children_groups;
	struct list_head group_node;

	spinlock_t lock;

	/* for XSE to move in perxcu */
	struct list_head members;
};

#define XSCHED_RQ_OF(xse)                                                      \
	(container_of(((xse)->cfs.cfs_rq), struct xsched_rq, cfs))

#define XSCHED_RQ_OF_CFS_XSE(cfs_xse)                                          \
	(container_of(((cfs_xse)->cfs_rq), struct xsched_rq, cfs))

#define XSCHED_SE_OF(cfs_xse)                                                  \
	(container_of((cfs_xse), struct xsched_entity, cfs))

#define xcg_parent_grp_xcu(xcg)                                                \
	((xcg)->self->parent->perxcu_priv[(xcg)->xcu_id])

#define xse_parent_grp_xcu(xse_cfs)                                            \
	(&((XSCHED_SE_OF(xse_cfs)                                                  \
		    ->parent_grp->perxcu_priv[(XSCHED_SE_OF(xse_cfs))->xcu->id])))

static inline struct xsched_group_xcu_priv *
xse_this_grp_xcu(struct xsched_entity_cfs *xse_cfs)
{
	struct xsched_entity *xse;

	xse = xse_cfs ? container_of(xse_cfs, struct xsched_entity, cfs) : NULL;
	return xse ? container_of(xse, struct xsched_group_xcu_priv, xse) : NULL;
}

static inline struct xsched_group *
xse_this_grp(struct xsched_entity_cfs *xse_cfs)
{
	return xse_cfs ? xse_this_grp_xcu(xse_cfs)->self : NULL;
}

/* Returns a pointer to an atomic_t variable representing a counter
 * of currently pending vstream kicks on a given XCU and for a
 * given xsched class.
 */
static inline atomic_t *
xsched_get_pending_kicks_class(const struct xsched_class *class,
				struct xsched_cu *xcu)
{
	/* Right now for testing purposes we have only XCU running streams. */
	if (!xcu) {
		XSCHED_ERR("Try to get pending kicks with xcu=NULL.\n");
		return NULL;
	}

	if (!class) {
		XSCHED_ERR("Try to get pending kicks with class=NULL.\n");
		return NULL;
	}

	if (class == &rt_xsched_class)
		return &xcu->pending_kicks_rt;
	if (class == &fair_xsched_class)
		return &xcu->pending_kicks_cfs;

	XSCHED_ERR("Xsched entity has an invalid class @ %s\n", __func__);
	return NULL;
}

/* Returns a pointer to an atomic_t variable representing a counter of
 * currently pending vstream kicks for an XCU on which a given xsched
 * entity is enqueued on and for a xsched class that assigned to a
 * given xsched entity.
 */
static inline atomic_t *
xsched_get_pending_kicks_xse(const struct xsched_entity *xse)
{
	if (!xse) {
		XSCHED_ERR("Try to get pending kicks with xse=NULL\n");
		return NULL;
	}

	if (!xse->xcu) {
		XSCHED_ERR("Try to get pending kicks with xse->xcu=NULL\n");
		return NULL;
	}

	return xsched_get_pending_kicks_class(xse->class, xse->xcu);
}

/* Increments pending kicks counter for an XCU that the given
 * xsched entity is attached to and for xsched entity's xsched
 * class.
 */
static inline int xsched_inc_pending_kicks_xse(struct xsched_entity *xse)
{
	atomic_t *kicks_class = NULL;

	kicks_class = xsched_get_pending_kicks_xse(xse);
	if (!kicks_class)
		return -EINVAL;

	/* Incrementing pending kicks for XSE's sched class */
	atomic_inc(kicks_class);

	/* Icrement pending kicks for current XSE. */
	atomic_inc(&xse->kicks_pending_ctx_cnt);

	/* Incrementing prio based pending kicks counter for RT class */
	if (xse_is_rt(xse))
		atomic_inc(&xse->xcu->xrq.rt.prio_nr_kicks[xse->rt.prio]);

	return 0;
}

/* Decrements pending kicks counter for an XCU that the given
 * xsched entity is attached to and for XSched entity's sched
 * class.
 */
static inline int xsched_dec_pending_kicks_xse(struct xsched_entity *xse)
{
	atomic_t *kicks_class = NULL;
	atomic_t *kicks_prio_rt = NULL;

	kicks_class = xsched_get_pending_kicks_xse(xse);
	if (!kicks_class)
		return -EINVAL;

	if (!atomic_read(kicks_class)) {
		XSCHED_ERR("Try to decrement pending kicks beyond 0!\n");
		return -EINVAL;
	}

	/* Decrementing pending kicks for XSE's sched class. */
	atomic_dec(kicks_class);

	/* Decrementing pending kicks for current XSE. */
	atomic_dec(&xse->kicks_pending_ctx_cnt);

	/* Decrementing prio based pending kicks counter for RT class. */
	if (xse_is_rt(xse)) {
		kicks_prio_rt = &xse->xcu->xrq.rt.prio_nr_kicks[xse->rt.prio];
		if (!atomic_read(kicks_prio_rt)) {
			XSCHED_ERR(
				"Try to decrement prio pending kicks beyond 0!\n");
			return -EINVAL;
		}
		atomic_dec(kicks_prio_rt);
	}

	return 0;
}

/* Checks if there are pending kicks left on a given XCU for all
 * xsched classes.
 */
static inline bool xsched_check_pending_kicks_xcu(struct xsched_cu *xcu)
{
	atomic_t *kicks_rt;
	atomic_t *kicks_cfs;

	kicks_rt = xsched_get_pending_kicks_class(&rt_xsched_class, xcu);
	kicks_cfs = xsched_get_pending_kicks_class(&fair_xsched_class, xcu);
	if (!kicks_rt || !kicks_cfs)
		return false;

	return (!!atomic_read(kicks_rt) || !!atomic_read(kicks_cfs));
}

static inline int xse_integrity_check(const struct xsched_entity *xse)
{
	if (!xse) {
		XSCHED_ERR("xse is null @ %s\n", __func__);
		return -EINVAL;
	}

	if (!xse->class) {
		XSCHED_ERR("xse->class is null @ %s\n", __func__);
		return -EINVAL;
	}

	return 0;
}

struct xsched_context {
	uint32_t fd;
	uint32_t dev_id;
	pid_t tgid;

	struct list_head vstream_list;
	struct list_head ctx_node;

	struct xsched_entity xse;

	spinlock_t ctx_lock;
	struct mutex ctx_mutex;
	struct kref kref;
};

extern struct list_head xsched_ctx_list;
extern struct mutex xsched_ctx_list_mutex;

/* Returns a pointer to xsched_context object corresponding to a given
 * tgid and xcu.
 */
static inline struct xsched_context *
ctx_find_by_tgid_and_xcu(pid_t tgid, struct xsched_cu *xcu)
{
	struct xsched_context *ctx;
	struct xsched_context *ret = NULL;

	list_for_each_entry(ctx, &xcu->ctx_list, ctx_node) {
		if (ctx->tgid == tgid) {
			ret = ctx;
			break;
		}
	}
	return ret;
}

/* Xsched class. */
struct xsched_class {
	const struct xsched_class *next;

	/* Removes a given XSE from it's runqueue. */
	void (*dequeue_ctx)(struct xsched_entity *xse);

	/* Places a given XSE on a runqueue on a given XCU. */
	void (*enqueue_ctx)(struct xsched_entity *xse, struct xsched_cu *xcu);

	/* Returns a next XSE to be submitted on a given XCU. */
	struct xsched_entity *(*pick_next_ctx)(struct xsched_cu *xcu);

	/* Put a XSE back into rq during preemption. */
	void (*put_prev_ctx)(struct xsched_entity *xse);

	/* Prepares a given XSE for submission on a given XCU. */
	int (*submit_prepare_ctx)(struct xsched_entity *xse,
				  struct xsched_cu *xcu);

	/* Check context preemption. */
	bool (*check_preempt)(struct xsched_entity *xse);

	/* Select jobs from XSE to submit on XCU */
	size_t (*select_work)(struct xsched_cu *xcu, struct xsched_entity *xse);
};

static inline void xsched_init_vsm(struct vstream_metadata *vsm,
				struct vstream_info *vs, vstream_args_t *arg)
{
	vsm->sq_id = arg->sq_id;
	vsm->sqe_num = arg->vk_args.sqe_num;
	vsm->timeout = arg->vk_args.timeout;
	memcpy(vsm->sqe, arg->vk_args.sqe, XCU_SQE_SIZE_MAX);
	vsm->parent = vs;
	INIT_LIST_HEAD(&vsm->node);
}

int xsched_xcu_register(struct xcu_group *group, int phys_id);
void xsched_task_free(struct kref *kref);
int xsched_ctx_init_xse(struct xsched_context *ctx, struct vstream_info *vs);
int ctx_bind_to_xcu(vstream_info_t *vstream_info, struct xsched_context *ctx);
int vstream_bind_to_xcu(vstream_info_t *vstream_info);
struct xsched_cu *xcu_find(uint32_t *type,
				uint32_t dev_id, uint32_t channel_id);

/* Vstream metadata proccesing functions.*/
int xsched_vsm_add_tail(struct vstream_info *vs, vstream_args_t *arg);
struct vstream_metadata *xsched_vsm_fetch_first(struct vstream_info *vs);
/* Xsched group manage functions */
int xsched_group_inherit(struct task_struct *tsk, struct xsched_entity *xse);
void xcu_cg_init_common(struct xsched_group *xcg);
void xcu_grp_shares_update(struct xsched_group *xg);
void xsched_group_xse_detach(struct xsched_entity *xse);

void xsched_quota_init(void);
void xsched_quota_timeout_init(struct xsched_group *xg);
void xsched_quota_timeout_update(struct xsched_group *xg);
void xsched_quota_account(struct xsched_group *xg, s64 exec_time);
bool xsched_quota_exceed(struct xsched_group *xg);
void xsched_quota_refill(struct work_struct *work);
void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu);
void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu);
#endif /* __LINUX_XSCHED_H__ */
