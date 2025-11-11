// SPDX-License-Identifier: GPL-2.0
/*
 * Block Layer I/O Latency Distribution Timestamp Statistics.
 *
 * Copyright (C) 2025 Kylin Software Co., LTD
 *
 */

#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/blk_types.h>

#include <trace/events/block.h>

#include "blk.h"

#define DEFAULT_THRESHOLD 100
#define ns_to_ms(time) div_u64(time, NSEC_PER_MSEC)

static u64 get_duration(u64 a, u64 b)
{
	return a > b ? a - b : 0;
}

void blk_latency_distribution_bio_acct(struct bio *bio, enum stage_io_latency_group stage)
{
	if (WARN_ON_ONCE(stage >= STAGE_BIO_NR))
		return;

	if (unlikely(!bio->time_ns))
		return;

	/*
	 * If the device is configured with logical volume management (LVM)
	 * or software RAID, its timestamp should not be recorded
	 * redundantly.
	 */
	if (bio->time_ns[stage])
		return;

	bio->time_ns[stage] = ktime_get_ns();
}

void blk_latency_distribution_rq_acct(struct request *rq, enum stage_io_latency_group stage,
				      struct bio *bio)
{
	if (WARN_ON_ONCE(stage >= STAGE_TOTAL_NR ||
			 stage < STAGE_BIO_NR))
		return;

	/*
	 * Reordering may occur, causing the runqueue (RQ) to be
	 * reinserted into the scheduling queue and redispatched.
	 * This can subsequently overwrite the initial timestamp
	 * assignments at points like I and D.
	 */
	if (rq->time_ns[stage])
		return;

	switch (stage) {
	case STAGE_RQ_GETRQ:
		if (bio && bio->time_ns)
			memcpy(rq->time_ns, bio->time_ns, STAGE_BIO_NR * sizeof(u64));
	default:
		rq->time_ns[stage] = ktime_get_ns();
	}
}
EXPORT_SYMBOL_GPL(blk_latency_distribution_rq_acct);

void blk_latency_distribution_rq_complete(struct request *rq, blk_status_t error,
					  unsigned int nr_bytes)
{
	u64 now = blk_time_get_ns();
	u64 duration;
	u64 q2c_ms;

	if (unlikely((!rq->time_ns[STAGE_BIO_ALLOC])))
		return;

	duration = get_duration(now, rq->time_ns[STAGE_BIO_ALLOC]);
	q2c_ms = ns_to_ms(duration);

	if (q2c_ms < DEFAULT_THRESHOLD)
		return;
}
