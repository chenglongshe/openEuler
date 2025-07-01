/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_XSCHED_H__
#define __LINUX_XSCHED_H__

#include <linux/xcu_group.h>
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define XSCHED_ERR_PREFIX "XSched [ERROR]: "
#define XSCHED_ERR(fmt, ...)                                                   \
	pr_err(XSCHED_ERR_PREFIX fmt, ##__VA_ARGS__)

#define XSCHED_WARN_PREFIX "XSched [WARNING]: "
#define XSCHED_WARN(fmt, ...)                                                  \
	pr_warn(XSCHED_WARN_PREFIX fmt, ##__VA_ARGS__)

/* Debug specific prints that are enabled
 * if CONFIG_XSCHED_DEBUG_PRINTS = y
 */
#ifdef CONFIG_XSCHED_DEBUG_PRINTS

#define XSCHED_INFO_PREFIX "XSched [INFO]: "
#define XSCHED_INFO(fmt, ...)                                                  \
	pr_info(XSCHED_INFO_PREFIX fmt, ##__VA_ARGS__)

#define XSCHED_DEBUG_PREFIX "XSched [DEBUG]: "
#define XSCHED_DEBUG(fmt, ...)                                                 \
	pr_debug(XSCHED_DEBUG_PREFIX fmt, ##__VA_ARGS__)

#define XSCHED_CALL_STUB()                                                     \
	XSCHED_INFO("-----* %s @ %s called *-----\n", __func__, __FILE__)

#define XSCHED_EXIT_STUB()                                                     \
	XSCHED_INFO("-----* %s @ %s exited *-----\n", __func__, __FILE__)

#define XSCHED_TRACE_PREFIX	"XSCHED [TRACE]: "

#define __XSCHED_TRACE(fmt, ...)			\
	pr_info(XSCHED_TRACE_PREFIX fmt, ##__VA_ARGS__)
#define XSCHED_TRACE(event, xcu, xse, vs, type)		\
	__XSCHED_TRACE("event_type=%u %u %s %s; xcu=%u; xse=%u; vs=%u; type=%s;",	\
			xse, vs, event, type, xcu, xse, vs, type)
#else
#define XSCHED_INFO(fmt, ...)
#define XSCHED_DEBUG(fmt, ...)
#define XSCHED_EXIT_STUB()
#define XSCHED_CALL_STUB()
#define __XSCHED_TRACE(fmt, ...)
#endif

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

	NR_XSCHED_XCU_STATUS
};

/* This is the abstraction object of the xcu computing unit. */
struct xsched_cu {
	uint32_t id;
	uint32_t state;

	struct task_struct *worker;

	struct xcu_group *group;

	struct mutex xcu_lock;

	wait_queue_head_t wq_xcu_idle;
	wait_queue_head_t wq_xcu_running;
	wait_queue_head_t wq_xcore_running;
};

int xsched_register_xcu(struct xcu_group *group);
#endif /* !__LINUX_XSCHED_H__ */
