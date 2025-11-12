/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XSCHED_IOCTL_H
#define _XSCHED_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PAYLOAD_SIZE_MAX 512
#define XCU_SQE_SIZE_MAX 64

typedef struct vstream_alloc_args {
	int type;
	__u32 user_stream_id;
} vstream_alloc_args_t;

typedef struct vstream_free_args { } vstream_free_args_t;

typedef struct vstream_kick_args {
	__u32 sqe_num;
	__s32 timeout;
	__s8 sqe[XCU_SQE_SIZE_MAX];
} vstream_kick_args_t;

typedef struct vstream_args {
	__u32 channel_id;
	__u32 fd;
	__u32 dev_id;
	__u32 task_type;
	__u32 sq_id;
	__u32 cq_id;

	/* Device related structures. */
	union {
		vstream_alloc_args_t va_args;
		vstream_free_args_t vf_args;
		vstream_kick_args_t vk_args;
	};

	__u32 payload_size;
	char payload[PAYLOAD_SIZE_MAX];
} vstream_args_t;

struct priority_args {
	__s32 pid;
	__u32 sched_priority;
};

#define XSCHED_MAGIC 'x'
#define XSCHED_ALLOC _IOWR(XSCHED_MAGIC, 0, vstream_args_t)
#define XSCHED_FREE _IOW(XSCHED_MAGIC, 1, vstream_args_t)
#define XSCHED_KICK _IOW(XSCHED_MAGIC, 2, vstream_args_t)
#define XSCHED_SET_PRIO _IOW(XSCHED_MAGIC, 3, struct priority_args)
#define XSCHED_GET_PRIO _IOR(XSCHED_MAGIC, 4, struct priority_args)

/* XSched ioctl handler */
int xsched_alloc(vstream_args_t *arg);
int xsched_free(vstream_args_t *arg);
int xsched_kick(vstream_args_t *arg);

#endif /* _XSCHED_IOCTL_H */
