// SPDX-License-Identifier: GPL-2.0
/*
 * Block Layer I/O Glitch Detection Timestamp Statistics.
 *
 * Copyright (C) 2025 Kylin Software Co., LTD
 *
 */

#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#include <trace/events/block.h>

#include "blk.h"
#include "blk-glitch-detection.h"
#include "blk-mq-debugfs.h"

static u64 get_duration(u64 a, u64 b)
{
	return a > b ? a - b : 0;
}

void blk_glitch_detection_bio_acct(struct bio *bio, enum stage_io_latency_group stage)
{
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

void blk_glitch_detection_rq_acct(struct request *rq, enum stage_io_latency_group stage,
				  struct bio *bio)
{
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
EXPORT_SYMBOL_GPL(blk_glitch_detection_rq_acct);

void blk_glitch_detection_rq_complete(struct request *rq, blk_status_t error,
				      unsigned int nr_bytes)
{
	struct blk_glitch_detection_stats *stats;
	u64 now = blk_time_get_ns();
	u64 duration;
	u64 q2c_us;

	if (unlikely((!rq->time_ns[STAGE_BIO_ALLOC])))
		return;

	duration = get_duration(now, rq->time_ns[STAGE_BIO_ALLOC]);
	q2c_us = ns_to_us(duration);

	stats = rq->q->bgd_debugfs;
	if (!stats)
		return;

	if (q2c_us < stats->threshold_us)
		return;

	trace_block_io_glitch_detection(rq, error, nr_bytes);
}
EXPORT_SYMBOL_GPL(blk_glitch_detection_rq_complete);

static int blk_threshold_show(void *data, struct seq_file *m)
{
	struct blk_glitch_detection_stats *stats = data;

	seq_printf(m, "%lu\n", stats->threshold_us);
	return 0;
}

/*
 * max size needed by different bases to express U64
 * HEX: "0xFFFFFFFFFFFFFFFF" --> 18
 * DEC: "18446744073709551615" --> 20
 * OCT: "01777777777777777777777" --> 23
 * pick the max one to define NUMBER_BUF_LEN
 */
#define MAX_BUF_LEN 24
static ssize_t blk_threshold_store(void *data, const char __user *buf, size_t count,
				   loff_t *ppos)
{
	int err;
	unsigned long val;
	char b[MAX_BUF_LEN + 1];
	struct blk_glitch_detection_stats *stats = data;

	if (count > MAX_BUF_LEN)
		return -EINVAL;

	if (copy_from_user(b, buf, count))
		return -EFAULT;

	b[count] = 0;
	err = kstrtoul(b, 0, &val);
	if (!err)
		stats->threshold_us = val;

	return err ? err : count;
}

static const struct blk_mq_debugfs_attr threshold_attr[] = {
	{
		"threshold",
		0600,
		blk_threshold_show,
		blk_threshold_store,
	},
	{},
};

void blk_glitch_detection_debugfs_unregister(struct request_queue *q)
{
	struct blk_glitch_detection_stats *stats = q->bgd_debugfs;

	lockdep_assert_held(&q->debugfs_mutex);

	if (stats == NULL)
		return;

	if (!blk_mq_debugfs_enabled(q))
		return;

	debugfs_remove_recursive(stats->debugfs_dir);
	stats->debugfs_dir = NULL;
	kfree(stats);
	q->bgd_debugfs = NULL;
}

void blk_glitch_detection_debugfs_register(struct request_queue *q)
{
	struct blk_glitch_detection_stats *stats;

	lockdep_assert_held(&q->debugfs_mutex);

	stats = kzalloc(sizeof(struct blk_glitch_detection_stats),
			GFP_KERNEL);
	if (!stats)
		return;

	if (!blk_mq_debugfs_enabled(q))
		return;

	stats->debugfs_dir = debugfs_create_dir("blk_glitch_detection",
						q->debugfs_dir);
	debugfs_create_files(stats->debugfs_dir, stats, threshold_attr);
	stats->threshold_us = DEFAULT_THRESHOLD_US;
	q->bgd_debugfs = stats;
}
