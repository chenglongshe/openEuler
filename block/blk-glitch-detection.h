// SPDX-License-Identifier: GPL-2.0
/*
 * Block Layer I/O glitch_detection Timestamp Statistics.
 *
 * Copyright (C) 2025 Kylin Software Co., LTD
 *
 */

#ifndef BLK_IO_GLITCH_DETECTION_H
#define BLK_IO_GLITCH_DETECTION_H

#include <linux/blkdev.h>
#include <linux/blk_types.h>

#ifdef CONFIG_BLK_IO_GLITCH_DETECTION

#define DEFAULT_THRESHOLD_US 100000
#define ns_to_us(time) div_u64(time, NSEC_PER_USEC)

struct blk_glitch_detection_stats {
	struct dentry *debugfs_dir;
	unsigned long threshold_us;
};

void blk_glitch_get_bio_stats(struct bio *bio);
void blk_glitch_put_bio_stats(struct bio *bio);
void blk_glitch_get_rq_stats(struct request *rq);
void blk_glitch_init_rq_stats(struct request *rq);
void
blk_glitch_detection_bio_acct(struct bio *bio, enum stage_io_latency_group stage);
void blk_glitch_detection_debugfs_register(struct request_queue *q);
void blk_glitch_detection_debugfs_unregister(struct request_queue *q);
#else
static inline void
blk_glitch_detection_bio_acct(struct bio *bio, enum stage_io_latency_group stage)
{
}

static inline void
blk_glitch_detection_debugfs_unregister(struct request_queue *q)
{
}

static inline void
blk_glitch_detection_debugfs_register(struct request_queue *q)
{
}
#endif /* CONFIG_BLK_IO_GLITCH_DETECTION */

#endif /* BLK_IO_GLITCH_DETECTION_H */
