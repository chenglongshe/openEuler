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

void
blk_glitch_detection_bio_acct(struct bio *bio, enum stage_io_latency_group stage);
void
blk_glitch_detection_rq_acct(struct request *rq, enum stage_io_latency_group stage,
			     struct bio *bio);
void
blk_glitch_detection_rq_complete(struct request *rq, blk_status_t error,
				 unsigned int nr_bytes);

#else

static inline void
blk_glitch_detection_bio_acct(struct bio *bio, enum stage_io_latency_group stage)
{
}
static inline void
blk_glitch_detection_rq_acct(struct request *rq, enum stage_io_latency_group stage,
			     struct bio *bio)
{
}
static inline void
blk_glitch_detection_rq_complete(struct request *rq, blk_status_t error,
				 unsigned int nr_bytes)
{
}
#endif /* CONFIG_BLK_IO_GLITCH_DETECTION */

#endif /* BLK_IO_GLITCH_DETECTION_H */
