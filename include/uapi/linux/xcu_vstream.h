/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_XCU_VSTREAM_H
#define _UAPI_XCU_VSTREAM_H

#include <linux/types.h>

#define PAYLOAD_SIZE_MAX 512
#define XCU_SQE_SIZE_MAX 64

/*
 * VSTREAM_ALLOC: alloc a vstream, buffer for tasks
 * VSTREAM_FREE: free a vstream
 * VSTREAM_KICK: there are tasks to be executed in the vstream
 */
typedef enum VSTREAM_COMMAND {
	VSTREAM_ALLOC = 0,
	VSTREAM_FREE,
	VSTREAM_KICK,
	MAX_COMMAND
} vstream_command_t;

typedef struct vstream_alloc_args {
	__s32 type;
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
	__s8 payload[PAYLOAD_SIZE_MAX];
} vstream_args_t;

#endif /* _UAPI_LINUX_SCHED_H */
