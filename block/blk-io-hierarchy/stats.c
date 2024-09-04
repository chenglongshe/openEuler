// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023. Huawei Technologies Co., Ltd. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/debugfs.h>

#include "stats.h"
#include "iodump.h"
#include "../blk.h"
#include "../blk-mq-debugfs.h"

#define io_hierarchy_add(statsp, field, group, nr) \
	this_cpu_add((statsp)->field[group], nr)
#define io_hierarchy_inc(statsp, field, group) \
	io_hierarchy_add(statsp, field, group, 1)

#define PRE_ALLOC_BIO_CNT 8

static mempool_t *hdata_pool;

void blk_mq_debugfs_register_hierarchy_stats(struct request_queue *q)
{
	struct blk_io_hierarchy_stats *stats;
	enum stage_group stage;

	stats = q->io_hierarchy_stats;
	if (!stats || !blk_mq_debugfs_enabled(q))
		return;

	stats->debugfs_dir = debugfs_create_dir("blk_io_hierarchy",
						q->debugfs_dir);
	blk_mq_debugfs_create_default_hierarchy_attr(q);

	for (stage = 0; stage < NR_STAGE_GROUPS; ++stage)
		blk_mq_debugfs_register_hierarchy(q, stage);
}

static void bio_alloc_hierarchy_data(struct bio *bio)
{
	if (!bio->hdata) {
		struct bio_hierarchy_data *hdata =
					mempool_alloc(hdata_pool, GFP_NOIO);

		bio_hierarchy_data_init(bio, hdata);
		bio->hdata = hdata;
	}
}

void bio_free_hierarchy_data(struct bio *bio)
{
	if (!bio->hdata)
		return;

	mempool_free(bio->hdata, hdata_pool);
	bio->hdata = NULL;
}

void blk_mq_debugfs_unregister_hierarchy_stats(struct request_queue *q)
{
	struct blk_io_hierarchy_stats *stats;
	enum stage_group stage;

	stats = q->io_hierarchy_stats;
	if (!stats || !blk_mq_debugfs_enabled(q))
		return;

	for (stage = 0; stage < NR_STAGE_GROUPS; ++stage)
		blk_mq_debugfs_unregister_hierarchy(q, stage);

	debugfs_remove_recursive(stats->debugfs_dir);
	stats->debugfs_dir = NULL;
}

int blk_io_hierarchy_stats_alloc(struct request_queue *q)
{
	struct blk_io_hierarchy_stats *stats;

	if (!q->mq_ops)
		return 0;

	stats = kzalloc(sizeof(struct blk_io_hierarchy_stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	stats->q = q;
	q->io_hierarchy_stats = stats;

	return 0;
}

void blk_io_hierarchy_stats_free(struct request_queue *q)
{
	struct blk_io_hierarchy_stats *stats = q->io_hierarchy_stats;

	if (!stats)
		return;

	q->io_hierarchy_stats = NULL;
	kfree(stats);
}

bool blk_mq_hierarchy_registered(struct request_queue *q,
				 enum stage_group stage)
{
	struct blk_io_hierarchy_stats *stats = q->io_hierarchy_stats;

	if (!stats)
		return false;

	return stats->hstage[stage] != NULL;
}
EXPORT_SYMBOL_GPL(blk_mq_hierarchy_registered);

void blk_mq_register_hierarchy(struct request_queue *q, enum stage_group stage)
{
	struct blk_io_hierarchy_stats *stats = q->io_hierarchy_stats;
	struct hierarchy_stage *hstage;

	if (!stats || !hierarchy_stage_name(stage))
		return;

	if (blk_mq_hierarchy_registered(q, stage)) {
		pr_warn("blk-io-hierarchy: disk %s is registering stage %s again.",
			kobject_name(q->kobj.parent),
			hierarchy_stage_name(stage));
		return;
	}

	/*
	 * Alloc memory before freeze queue, prevent deadlock if new IO is
	 * issued by memory reclaim.
	 */
	hstage = kmalloc(sizeof(*hstage), GFP_KERNEL);
	if (!hstage)
		return;

	hstage->hstats = alloc_percpu(struct hierarchy_stats);
	if (!hstage->hstats) {
		kfree(hstage);
		return;
	}

	hstage->stage = stage;
	hstage->debugfs_dir = NULL;
	if (blk_io_hierarchy_iodump_init(q, hstage) < 0) {
		free_percpu(hstage->hstats);
		kfree(hstage);
		return;
	}

	blk_mq_freeze_queue(q);

	WRITE_ONCE(stats->hstage[stage], hstage);
	blk_mq_debugfs_register_hierarchy(q, stage);

	blk_mq_unfreeze_queue(q);
}
EXPORT_SYMBOL_GPL(blk_mq_register_hierarchy);

void blk_mq_unregister_hierarchy(struct request_queue *q,
				 enum stage_group stage)
{
	struct blk_io_hierarchy_stats *stats = q->io_hierarchy_stats;
	struct hierarchy_stage *hstage;

	if (!blk_mq_hierarchy_registered(q, stage))
		return;

	blk_mq_debugfs_unregister_hierarchy(q, stage);
	blk_io_hierarchy_iodump_exit(q, stage);

	hstage = stats->hstage[stage];
	stats->hstage[stage] = NULL;
	free_percpu(hstage->hstats);
	kfree(hstage);
}
EXPORT_SYMBOL_GPL(blk_mq_unregister_hierarchy);

static enum stat_group bio_hierarchy_op(struct bio *bio)
{
	if (op_is_discard(bio->bi_opf))
		return STAT_DISCARD;

	if (op_is_flush(bio->bi_opf) &&
	    !(bio_sectors(bio) || bio_flagged(bio, BIO_HAS_DATA)))
		return STAT_FLUSH;

	if (op_is_write(bio->bi_opf))
		return STAT_WRITE;

	return STAT_READ;
}


void bio_hierarchy_start_io_acct(struct bio *bio, enum stage_group stage)
{
	struct request_queue *q = bio->bi_disk->queue;
	struct hierarchy_stage *hstage;

	if (!blk_mq_hierarchy_registered(q, stage))
		return;

	hstage = q->io_hierarchy_stats->hstage[stage];
	bio_alloc_hierarchy_data(bio);
	io_hierarchy_inc(hstage->hstats, dispatched, bio_hierarchy_op(bio));
	bio->hdata->time = blk_time_get_ns();
	hierarchy_add_bio(hstage, bio);
}

void __bio_hierarchy_end_io_acct(struct bio *bio, enum stage_group stage,
				 u64 time)
{
	struct request_queue *q = bio->bi_disk->queue;
	struct hierarchy_stage *hstage;
	u64 duration;
	enum stat_group op;

	if (!blk_mq_hierarchy_registered(q, stage))
		return;

	op = bio_hierarchy_op(bio);
	duration = time - bio->hdata->time;
	hstage = q->io_hierarchy_stats->hstage[stage];

	hierarchy_remove_bio(hstage, bio);
	io_hierarchy_inc(hstage->hstats, completed, op);
	io_hierarchy_add(hstage->hstats, nsecs, op, duration);
	hierarchy_account_slow_io_ns(hstage, op, duration);
}

static enum stat_group rq_hierarchy_op(struct request *rq)
{
	if (op_is_discard(rq->cmd_flags))
		return STAT_DISCARD;

	if (is_flush_rq(rq))
		return STAT_FLUSH;

	if (op_is_write(rq->cmd_flags))
		return STAT_WRITE;

	return STAT_READ;
}

void __rq_hierarchy_start_io_acct(struct request *rq,
				  struct hierarchy_stage *hstage)
{
	io_hierarchy_inc(hstage->hstats, dispatched, rq_hierarchy_op(rq));
	WRITE_ONCE(rq->hierarchy_time, jiffies);

	/*
	 * Paired with barrier in hierarchy_show_rq_fn(), make sure
	 * hierarchy_time is set before stage.
	 */
	smp_store_release(&rq->stage, hstage->stage);
}
EXPORT_SYMBOL_GPL(__rq_hierarchy_start_io_acct);

void __rq_hierarchy_end_io_acct(struct request *rq,
				struct hierarchy_stage *hstage)
{
	unsigned long duration = jiffies - rq->hierarchy_time;
	enum stat_group op = rq_hierarchy_op(rq);

	io_hierarchy_inc(hstage->hstats, completed, op);
	io_hierarchy_add(hstage->hstats, jiffies, op, duration);
	hierarchy_account_slow_io_jiffies(hstage, op, duration);
	WRITE_ONCE(rq->stage, NR_RQ_STAGE_GROUPS);
}
EXPORT_SYMBOL_GPL(__rq_hierarchy_end_io_acct);

#ifdef CONFIG_HIERARCHY_BIO
void bio_hierarchy_start(struct bio *bio)
{
	struct gendisk *disk = bio->bi_disk;
	struct hierarchy_stage *hstage;

	if (bio_flagged(bio, BIO_HIERARCHY_ACCT))
		return;

	if (!blk_mq_hierarchy_registered(disk->queue, STAGE_BIO))
		return;

	bio_set_flag(bio, BIO_HIERARCHY_ACCT);
	if (bio_has_data(bio))
		bio_set_flag(bio, BIO_HAS_DATA);
	hstage = disk->queue->io_hierarchy_stats->hstage[STAGE_BIO];
	io_hierarchy_inc(hstage->hstats, dispatched, bio_hierarchy_op(bio));
}

void __bio_hierarchy_end(struct bio *bio, u64 now)
{
	struct gendisk *disk = bio->bi_disk;
	struct hierarchy_stage *hstage;
	u64 duration;
	enum stat_group op;

	op = bio_hierarchy_op(bio);
	duration = now - bio->bi_alloc_time_ns;
	hstage = disk->queue->io_hierarchy_stats->hstage[STAGE_BIO];

	io_hierarchy_inc(hstage->hstats, completed, op);
	io_hierarchy_add(hstage->hstats, nsecs, op, duration);
	hierarchy_account_slow_io_ns(hstage, op, duration);

	bio_clear_flag(bio, BIO_HIERARCHY_ACCT);
	bio_clear_flag(bio, BIO_HAS_DATA);
}

#endif

static int __init hierarchy_stats_init(void)
{
	hdata_pool = mempool_create_kmalloc_pool(PRE_ALLOC_BIO_CNT,
			sizeof(struct bio_hierarchy_data));
	if (!hdata_pool)
		panic("Failed to create hdata_pool\n");

	return 0;
}
module_init(hierarchy_stats_init);
