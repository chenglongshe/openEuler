/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VSTREAM_H
#define _VSTREAM_H

#include <linux/ktime.h>
#include "xsched_ioctl.h"

#define MAX_VSTREAM_SIZE 2048
#define XCU_CQE_SIZE_MAX 32
#define XCU_CQE_REPORT_NUM 4
#define XCU_CQE_BUF_SIZE (XCU_CQE_REPORT_NUM * XCU_CQE_SIZE_MAX)

/* Vstream metadata describes each incoming kick
 * that gets stored into a list of pending kicks
 * inside a vstream to keep track of what is left
 * to be processed by a driver.
 */
typedef struct vstream_metadata {
	/* A value of SQ tail that has been passed with the
	 * kick that is described by this exact metadata object.
	 */
	uint32_t sq_tail;
	uint32_t sqe_num;
	uint32_t sq_id;
	uint8_t sqe[XCU_SQE_SIZE_MAX];

	/* Report buffer for fake read. */
	int8_t cqe[XCU_CQE_BUF_SIZE];
	uint32_t cqe_num;
	int32_t timeout;

	/* A node for metadata list */
	struct list_head node;

	struct vstream_info *parent;

	/* Time of list insertion */
	ktime_t add_time;
} vstream_metadata_t;

typedef struct vstream_info {
	uint32_t user_stream_id;
	uint32_t id;
	uint32_t vcq_id;
	uint32_t logic_vcq_id;
	uint32_t dev_id;
	uint32_t channel_id;
	uint32_t fd;
	uint32_t task_type;
	int tgid;
	int sqcq_type;

	void *drv_ctx;

	int inode_fd;

	/* Pointer to corresponding context. */
	struct xsched_context *ctx;

	/* List node in context's vstream list. */
	struct list_head ctx_node;

	/* Pointer to an CU object on which this
	 * vstream is currently being processed.
	 * NULL if vstream is not being processed.
	 */
	struct xsched_cu *xcu;

	/* List node in an CU list of vstreams that
	 * are currently being processed by this specific CU.
	 */
	struct list_head xcu_node;

	/* Private vstream data. */
	void *data;

	spinlock_t stream_lock;

	uint32_t kicks_count;

	/* List of metadata a.k.a. all recorded unprocesed
	 * kicks for this exact vstream.
	 */
	struct list_head metadata_list;
} vstream_info_t;

#endif /* _VSTREAM_H */
