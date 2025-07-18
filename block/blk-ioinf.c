// SPDX-License-Identifier: GPL-2.0
/*
 * IO inflight relative controller
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/time64.h>
#include <linux/parser.h>
#include <linux/blk-cgroup.h>

#include "blk-cgroup.h"
#include "blk-rq-qos.h"
#include "blk-mq.h"

/* default weight for each cgroup */
#define IOINF_DFL_WEIGHT	10
/* default wake up time in jiffies for backgroup job, see ioinf_timer_fn() */
#define IOINF_TIMER_PERID	500
/* default time in jiffies that cgroup will idle without any IO */
#define INFG_DFL_EXPIRE		100
/* minimal number of samples for congestion control */
#define MIN_SAMPLES		100
/* io.inf.qos controls */
enum {
	INF_ENABLE,
	INF_INFLIGHT,

	QOS_ENABLE,
	QOS_RLAT,
	QOS_WLAT,
	QOS_RPCT,
	QOS_WPCT,

	NR_QOS_CTRL_PARAMS,
};

/* ioinf_gq flags */
enum {
	INFG_EXHAUSTED,
	INFG_LEND,
	INFG_BORROW,
	INFG_OFFLINE,
};

/* qos control params */
struct ioinf_params {
	bool enabled;
	bool qos_enabled;
	u32 inflight;
	u64 rlat;
	u64 wlat;
	u32 rpct;
	u32 wpct;
};

struct ioinf_lat_stat {
	u64 rmet;
	u64 wmet;
	u64 rmissed;
	u64 wmissed;
};

struct infg_lat_stat {
	atomic_long_t latency;
};

/* the global conrtol structure */
struct ioinf {
	struct rq_qos		rqos;

	struct ioinf_params	params;
	/* real inflight with consideration of busy_level */
	u32			inflight;

	/* default time for ioinf_timer_fn */
	unsigned long		inf_timer_perid;
	struct timer_list	inf_timer;

	/* default time for infg_expire_fn */
	unsigned long		infg_expire_jiffies;

	/* global lock */
	spinlock_t		lock;
	/* list of active infgs */
	struct list_head	active_infgs;
	/* list of active infgs that lend inflight budget to other infgs */
	struct list_head	lend_infgs;
	/* list of active infgs that borrow inflight budget from other infgs */
	struct list_head	borrow_infgs;

	/* for offline cgroups */
	u32			offline_hinflight;
	struct rq_wait		offline_rqw;

	struct ioinf_lat_stat	last_stat;
	struct ioinf_lat_stat __percpu *stat;

	int			busy_level;
	int			old_scale;
};

/* per disk-cgroup pair structure */
struct ioinf_gq {
	struct blkg_policy_data	pd;
	struct ioinf		*inf;

	unsigned long		flags;
	/* head of the list is inf->active_infgs */
	struct list_head	active;
	/* head of the list is inf->lend_infgs */
	struct list_head	lend;
	/* head of the list is inf->borrow_infgs */
	struct list_head	borrow;

	/* configured by user */
	u32			weight;
	/* normalized weight */
	u32			hweight;
	/* normalized inflight budget */
	u32			hinflight;
	/* inuse inflight budget */
	u32			hinflight_inuse;
	/* IO beyond budget will wait here */
	struct rq_wait		rqw;

	struct timer_list	expire_timer;

	/* max inflight in current perid */
	u32			max_inflight;
	/* max inflight in last perid, will gradual reduction */
	u32			last_max_inflight;

	/* to calculate avgqu size */
	struct infg_lat_stat	stat;
};

/* per cgroup structure, used to record default weight for all disks */
struct ioinf_cgrp {
	struct blkcg_policy_data	cpd;

	/* if def_weight is 0, means it's offline */
	u32				dfl_weight;
};

/* scale inflight according to busy_level, from 1/10 to 10 */
static const u8 scale_table[20] = {
	[0] = 100,	/* -90 */
	[1] = 90,	/* -80 */
	[2] = 80,	/* -70 */
	[3] = 70,	/* -60 */
	[4] = 60,	/* -50 */
	[5] = 50,	/* -40 */
	[6] = 40,	/* -30 */
	[7] = 30,	/* -20 */
	[8] = 20,	/* -10 */
	[9] = 10,	/*   0  */
	[10] = 9,	/*  10 */
	[11] = 8,	/*  20 */
	[12] = 7,	/*  30 */
	[13] = 6,	/*  40 */
	[14] = 5,	/*  50 */
	[15] = 4,	/*  60 */
	[16] = 3,	/*  70 */
	[17] = 2,	/*  80 */
	[18] = 1,	/*  90 */
};

static struct blkcg_policy blkcg_policy_ioinf;

static struct ioinf *rqos_to_inf(struct rq_qos *rqos)
{
	return container_of(rqos, struct ioinf, rqos);
}

static struct ioinf *q_to_inf(struct request_queue *q)
{
	return rqos_to_inf(rq_qos_id(q, RQ_QOS_INFLIGHT));
}

static struct ioinf_gq *pd_to_infg(struct blkg_policy_data *pd)
{
	if (!pd)
		return NULL;

	return container_of(pd, struct ioinf_gq, pd);
}

static struct ioinf_gq *blkg_to_infg(struct blkcg_gq *blkg)
{
	return pd_to_infg(blkg_to_pd(blkg, &blkcg_policy_ioinf));
}

static struct blkcg_gq *infg_to_blkg(struct ioinf_gq *infg)
{
	return pd_to_blkg(&infg->pd);
}

static struct ioinf_cgrp *blkcg_to_infcg(struct blkcg *blkcg)
{
	struct blkcg_policy_data *cpd =
		blkcg_to_cpd(blkcg, &blkcg_policy_ioinf);

	return container_of(cpd, struct ioinf_cgrp, cpd);
}

static struct blkcg_gq *ioinf_bio_blkg(struct bio *bio)
{
	struct blkcg_gq *blkg = bio->bi_blkg;

	if (!blkg || !blkg->online)
		return NULL;

	if (blkg->blkcg->css.cgroup->level == 0)
		return NULL;

	return blkg;
}

static struct ioinf_gq *ioinf_bio_infg(struct bio *bio)
{
	struct ioinf_gq *infg;
	struct blkcg_gq *blkg = ioinf_bio_blkg(bio);

	if (!blkg)
		return NULL;

	infg = blkg_to_infg(blkg);
	if (!infg)
		return NULL;

	return infg;
}

static u32 infg_weight(struct ioinf_gq *infg)
{
	struct ioinf_cgrp *infcg;
	struct blkcg_gq *blkg;

	if (infg->weight)
		return infg->weight;

	/* if user doen't set per disk weight, use the cgroup default weight */
	blkg = infg_to_blkg(infg);
	infcg = blkcg_to_infcg(blkg->blkcg);

	return infcg->dfl_weight;
}

static void infg_clear_loan(struct ioinf_gq *infg)
{
	if (!list_empty(&infg->lend)) {
		clear_bit(INFG_LEND, &infg->flags);
		list_del_init(&infg->lend);
	}

	if (!list_empty(&infg->borrow)) {
		clear_bit(INFG_BORROW, &infg->flags);
		list_del_init(&infg->borrow);
	}
}

/*
 * called when infg is activate or deactivate
 * TODO: support cgroup hierarchy, each infg is independent for now
 */
static void __propagate_weights(struct ioinf *inf)
{
	struct ioinf_gq *infg;
	u32 total = 0;

	if (list_empty(&inf->active_infgs))
		return;

	/*
	 * TODO: instead of clearing loan and reinitializing everything, it's
	 * better to keep loan and do minor incremental modification.
	 */
	list_for_each_entry(infg, &inf->active_infgs, active) {
		total += infg_weight(infg);
		infg->max_inflight = 0;
		infg->last_max_inflight = 0;
		infg_clear_loan(infg);
	}

	list_for_each_entry(infg, &inf->active_infgs, active) {
		u32 weight = infg_weight(infg);

		infg->hweight = weight * 100 / total;
		infg->hinflight = infg->inf->inflight * infg->hweight / 100;
		if (!infg->hinflight)
			infg->hinflight = 1;
		infg->hinflight_inuse = infg->hinflight;
	}

	mod_timer(&inf->inf_timer, jiffies + inf->inf_timer_perid);
}

static void propagate_weights(struct ioinf *inf)
{
	spin_lock_irq(&inf->lock);
	__propagate_weights(inf);
	spin_unlock_irq(&inf->lock);
}

static void ioinf_active_infg(struct ioinf_gq *infg)
{
	struct ioinf *inf = infg->inf;

	spin_lock_irq(&inf->lock);
	if (list_empty(&infg->active)) {
		list_add(&infg->active, &inf->active_infgs);
		__propagate_weights(inf);
	}
	spin_unlock_irq(&inf->lock);
}

static unsigned int atomic_inc_below_return(atomic_t *v, unsigned int below)
{
	unsigned int cur = atomic_read(v);

	for (;;) {
		unsigned int old;

		if (cur >= below)
			return below + 1;

		old = atomic_cmpxchg(v, cur, cur + 1);
		if (old == cur)
			break;
		cur = old;
	}

	return cur + 1;
}

/*
 * Called from io fast path, return false means inflight IO is full, and the
 * forground thread will wait inflight IO to be done.
 */
static bool ioinf_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf_gq *infg = private_data;
	unsigned int inflight;
	unsigned int limit;

retry:
	limit = infg->hinflight_inuse;
	inflight = atomic_inc_below_return(&infg->rqw.inflight, limit);

	if (inflight > infg->max_inflight)
		infg->max_inflight = inflight;

	if (inflight <= limit)
		return true;

	if (infg->hinflight_inuse == limit) {
		/*
		 * This infg want more inflight budget, set INFG_EXHAUSTED, and
		 * later ioinf_timer_fn() will check, if other infg can lend
		 * budget.
		 */
		if (test_bit(INFG_EXHAUSTED, &infg->flags))
			return false;

		set_bit(INFG_EXHAUSTED, &infg->flags);
		return false;
	}

	/* Stop lend inflight budget to other infgs */
	infg->hinflight_inuse = infg->hinflight;
	/* wake up ioinf_timer_fn() immediately to inform other infgs */
	timer_reduce(&infg->inf->inf_timer, jiffies + 1);
	goto retry;
}

void ioinf_done(struct ioinf_gq *infg)
{
	int inflight = atomic_dec_return(&infg->rqw.inflight);

	BUG_ON(inflight < 0);

	if (inflight < infg->hinflight && wq_has_sleeper(&infg->rqw.wait))
		wake_up_all(&infg->rqw.wait);

	/* deactivate infg if there is no IO for infg_expire_jiffies */
	if (inflight == 0)
		mod_timer(&infg->expire_timer,
			  jiffies + infg->inf->infg_expire_jiffies);
}

void ioinf_offline_done(struct ioinf *inf)
{
	int inflight = atomic_dec_return(&inf->offline_rqw.inflight);

	if (inflight < inf->offline_hinflight &&
	    wq_has_sleeper(&inf->offline_rqw.wait))
		wake_up_all(&inf->offline_rqw.wait);
}

static void ioinf_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	ioinf_done(private_data);
}

static bool infg_offline(struct ioinf_gq *infg)
{
	struct ioinf_cgrp *infcg;
	struct blkcg_gq *blkg;

	if (test_bit(INFG_OFFLINE, &infg->flags))
		return true;

	if (infg->weight != 0)
		return false;

	/* if user doen't set per disk weight, use the cgroup default weight */
	blkg = infg_to_blkg(infg);
	infcg = blkcg_to_infcg(blkg->blkcg);

	return infcg->dfl_weight == 0;
}

static bool ioinf_offline_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf *inf = private_data;

	return rq_wait_inc_below(rqw, inf->offline_hinflight);
}

static void ioinf_offline_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf *inf = private_data;

	ioinf_offline_done(inf);
}

static void ioinf_throttle_offline(struct ioinf *inf, struct bio *bio)
{
	rq_qos_wait(&inf->offline_rqw, inf, ioinf_offline_inflight_cb,
		    ioinf_offline_cleanup_cb, NULL);

	/*
	 * In case no online cgroup is active, daemon will adjuct all the
	 * budget to offline cgroup.
	 */
	timer_reduce(&inf->inf_timer, jiffies + inf->inf_timer_perid);
}

static void ioinf_rqos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_gq *infg = ioinf_bio_infg(bio);

	if (!inf->params.enabled || !infg)
		return;

	if (infg_offline(infg)) {
		ioinf_throttle_offline(inf, bio);
		return;
	}

	if (list_empty_careful(&infg->active))
		ioinf_active_infg(infg);

	rq_qos_wait(&infg->rqw, infg, ioinf_inflight_cb, ioinf_cleanup_cb,
		    NULL);
}

static void ioinf_rqos_track(struct rq_qos *rqos, struct request *rq,
			     struct bio *bio)
{
	struct blkcg_gq *blkg = ioinf_bio_blkg(bio);

	if (!blkg)
		return;

	rq->blkg = blkg;
}

static void ioinf_rqos_cleanup(struct rq_qos *rqos, struct bio *bio)
{
	struct ioinf_gq *infg = ioinf_bio_infg(bio);

	if (!infg || infg->inf->params.enabled ||
	    list_empty_careful(&infg->active))
		return;

	ioinf_done(infg);
}

static void ioinf_record_lat(struct ioinf_gq *infg, struct request *rq)
{
	struct ioinf *inf = infg->inf;
	u64 lat;

	lat = rq->io_end_time_ns ? rq->io_end_time_ns : blk_time_get_ns();
	lat -= rq->alloc_time_ns;
	atomic_long_add(lat, &infg->stat.latency);

	if (!inf->params.qos_enabled)
		return;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		if (lat > inf->params.rlat)
			this_cpu_inc(inf->stat->rmissed);
		else
			this_cpu_inc(inf->stat->rmet);
		break;
	case REQ_OP_WRITE:
		if (lat > inf->params.wlat)
			this_cpu_inc(inf->stat->wmissed);
		else
			this_cpu_inc(inf->stat->wmet);
		break;
	default:
		break;
	}
}

static struct ioinf_lat_stat ioinf_get_lat(struct ioinf *inf)
{
	struct ioinf_lat_stat stat = {0};
	int cpu;

	for_each_possible_cpu(cpu) {
		struct ioinf_lat_stat *pstat = per_cpu_ptr(inf->stat, cpu);

		stat.rmet += pstat->rmet;
		stat.rmissed += pstat->rmissed;
		stat.wmet += pstat->wmet;
		stat.wmissed += pstat->wmissed;
	}

	return stat;
}

static void ioinf_rqos_done(struct rq_qos *rqos, struct request *rq)
{
	struct blkcg_gq *blkg = rq->blkg;
	struct ioinf_gq *infg;

	if (!blkg)
		return;

	infg = blkg_to_infg(blkg);
	if (infg_offline(infg)) {
		ioinf_offline_done(infg->inf);
	} else {
		ioinf_done(infg);
		ioinf_record_lat(infg, rq);
	}

	rq->blkg = NULL;
}

static void ioinf_rqos_exit(struct rq_qos *rqos)
{
	struct ioinf *inf = rqos_to_inf(rqos);

	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_ioinf);

	del_timer_sync(&inf->inf_timer);
	free_percpu(inf->stat);
	kfree(inf);
}

static int ioinf_stat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_gq *infg;
	char path[32];

	spin_lock_irq(&inf->lock);

	seq_printf(m, "busy_level %d inflight %u->%u\n", inf->busy_level,
		   inf->params.inflight, inf->inflight);

	list_for_each_entry(infg, &inf->active_infgs, active) {
		blkg_path(infg_to_blkg(infg), path, sizeof(path));
		seq_printf(m, "%s: hweight %u", path, infg->hweight);

		if (test_bit(INFG_LEND, &infg->flags))
			seq_puts(m, " lend");
		if (test_bit(INFG_BORROW, &infg->flags))
			seq_puts(m, " borrow");
		if (test_bit(INFG_EXHAUSTED, &infg->flags))
			seq_puts(m, " exhausted");

		seq_printf(m, " inflight %d/(%d->%d) %u->%u\n",
			   atomic_read(&infg->rqw.inflight),
			   infg->hinflight, infg->hinflight_inuse,
			   infg->last_max_inflight,
			   infg->max_inflight);
	}

	seq_printf(m, "offline inflight %u/%d\n",
		   atomic_read(&inf->offline_rqw.inflight),
		   inf->offline_hinflight);

	spin_unlock_irq(&inf->lock);

	return 0;
}

static int ioinf_lat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_lat_stat stat = ioinf_get_lat(inf);
	struct ioinf_gq *infg;
	char path[32];

	seq_printf(m, "qos lat: %llu %llu %llu %llu\n",
		   stat.rmet, stat.rmissed, stat.wmet, stat.wmissed);

	spin_lock_irq(&inf->lock);
	list_for_each_entry(infg, &inf->active_infgs, active) {
		blkg_path(infg_to_blkg(infg), path, sizeof(path));
		seq_printf(m, "%s latency %lu\n", path,
			   atomic_long_read(&infg->stat.latency));
	}
	spin_unlock_irq(&inf->lock);

	return 0;
}

static const struct blk_mq_debugfs_attr ioinf_debugfs_attrs[] = {
	{"stat", 0400, ioinf_stat_show},
	{"lat", 0400, ioinf_lat_show},
	{},
};

static struct rq_qos_ops ioinf_rqos_ops = {
	.throttle	= ioinf_rqos_throttle,
	.done		= ioinf_rqos_done,
	.track		= ioinf_rqos_track,
	.cleanup	= ioinf_rqos_cleanup,
	.exit		= ioinf_rqos_exit,

#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = ioinf_debugfs_attrs,
#endif
};

static void infg_update_inflight(struct ioinf_gq *infg, u32 *exhausted_count)
{
	unsigned int last_max_inflight = infg->last_max_inflight;

	infg->hinflight_inuse = max(last_max_inflight, infg->max_inflight);

	infg->last_max_inflight = max(last_max_inflight >> 1, infg->max_inflight);
	infg->max_inflight = infg->max_inflight >> 1;

	if (infg->hinflight_inuse < infg->hinflight) {
		clear_bit(INFG_EXHAUSTED, &infg->flags);
		clear_bit(INFG_BORROW, &infg->flags);
		set_bit(INFG_LEND, &infg->flags);

		if (!list_empty(&infg->borrow))
			list_del_init(&infg->borrow);
		if (list_empty(&infg->lend))
			list_add_tail(&infg->lend, &infg->inf->lend_infgs);
	} else if (test_bit(INFG_EXHAUSTED, &infg->flags)) {
		(*exhausted_count)++;

		if (list_empty(&infg->borrow)) {
			set_bit(INFG_BORROW, &infg->flags);
			list_add_tail(&infg->borrow, &infg->inf->borrow_infgs);
		}
	}
}

static void ioinf_set_offline_inflight(struct ioinf *inf, u32 inflight)
{
	inf->offline_hinflight = inflight;

	if (wq_has_sleeper(&inf->offline_rqw.wait))
		wake_up_all(&inf->offline_rqw.wait);
}

static bool ioinf_online_busy(struct ioinf *inf)
{
	struct ioinf_lat_stat stat;
	u32 met, missed;
	bool ret = false;

	if (!inf->params.qos_enabled)
		return false;

	stat = ioinf_get_lat(inf);
	met = stat.rmet - inf->last_stat.rmet;
	missed = stat.rmissed - inf->last_stat.rmissed;

	if (met + missed >= MIN_SAMPLES &&
	    met * 100 < (met + missed) * inf->params.rpct) {
		ret = true;
		goto out;
	}

	met = stat.wmet - inf->last_stat.wmet;
	missed = stat.wmissed - inf->last_stat.wmissed;

	if (met + missed >= MIN_SAMPLES &&
	    met * 100 < (met + missed) * inf->params.wpct)
		ret = true;

out:
	inf->last_stat = stat;
	return ret;
}

static void inf_clear_exhausted(struct ioinf *inf, u32 borrow)
{
	struct ioinf_gq *infg;

	list_for_each_entry(infg, &inf->borrow_infgs, borrow)
		if (test_and_clear_bit(INFG_EXHAUSTED, &infg->flags))
			infg->hinflight_inuse += borrow;
}

static void ioinf_adjust_busy_level(struct ioinf *inf, u32 old_busy_level)
{
	struct ioinf_gq *infg;
	int scale;

	inf->busy_level = clamp(inf->busy_level, -90, 90);

	if (inf->busy_level == old_busy_level)
		return;

	scale = (inf->busy_level + 90) / 10;
	if (scale == inf->old_scale)
		return;

	scale = scale_table[scale];
	inf->old_scale = scale;

	inf->inflight = inf->params.inflight * scale / 10;

	list_for_each_entry(infg, &inf->active_infgs, active) {
		infg->hinflight = inf->inflight * infg->hweight / 100;
		if (!infg->hinflight)
			infg->hinflight = 1;
		if (infg->hinflight_inuse < infg->hinflight) {
			infg->hinflight_inuse = infg->hinflight;
			if (wq_has_sleeper(&infg->rqw.wait))
				wake_up_all(&infg->rqw.wait);
		}
	}
}

static void infg_clear_borrow(struct ioinf *inf, int borrow)
{
	struct ioinf_gq *infg;
	int count = 0;

retry:
	list_for_each_entry(infg, &inf->borrow_infgs, borrow) {
		if (infg->hinflight_inuse > infg->hinflight) {
			infg->hinflight_inuse--;
			count++;
		}
	}

	borrow -= count;
	if (count && borrow > 0)
		goto retry;
}

static void ioinf_timer_fn(struct timer_list *timer)
{
	struct ioinf *inf = container_of(timer, struct ioinf, inf_timer);
	int exhausted_count = 0, lend_total = 0, borrow_total = 0;
	u32 old_busy_level = inf->busy_level;
	bool busy = ioinf_online_busy(inf);
	struct ioinf_gq *infg;
	unsigned long flags;

	/* only offline cgroup is active */
	if (list_empty_careful(&inf->active_infgs)) {
		ioinf_set_offline_inflight(inf, inf->inflight);
		return;
	}

	spin_lock_irqsave(&inf->lock, flags);

	list_for_each_entry(infg, &inf->active_infgs, active)
		infg_update_inflight(infg, &exhausted_count);

	list_for_each_entry(infg, &inf->lend_infgs, lend)
		lend_total += infg->hinflight - infg->hinflight_inuse;

	list_for_each_entry(infg, &inf->borrow_infgs, borrow)
		borrow_total += infg->hinflight_inuse - infg->hinflight;

	if (lend_total < 0)
		lend_total = 0;
	if (borrow_total < 0)
		borrow_total = 0;
	if (lend_total >= borrow_total) {
		lend_total -= borrow_total;
	} else {
		borrow_total -= lend_total;
		lend_total = 0;
		infg_clear_borrow(inf, borrow_total);
	}
	/*
	 * TODO: handle loan gracefully, equal division for now.
	 */
	if (exhausted_count) {
		u32 borrow;

		if (inf->offline_hinflight > 1) {
			ioinf_set_offline_inflight(inf, 1);
			goto unlock;
		}

		borrow = lend_total / exhausted_count;
		if (borrow > 0)
			inf_clear_exhausted(inf, borrow);
		else if (busy) /* slow down */
			inf->busy_level += exhausted_count;
		else if (inf->params.qos_enabled)
			inf->busy_level--;
	} else {
		if (busy) { /* too much budget */
			if (inf->offline_hinflight > 1)
				inf->offline_hinflight <<= 1;
			inf->busy_level++;
		} else { /* everything is fine, upgrade offline */
			if (inf->params.qos_enabled)
				inf->busy_level--;
			if (lend_total > inf->offline_hinflight)
				ioinf_set_offline_inflight(inf, lend_total);
		}
	}

	ioinf_adjust_busy_level(inf, old_busy_level);

unlock:
	spin_unlock_irqrestore(&inf->lock, flags);

	if (!list_empty_careful(&inf->active_infgs))
		mod_timer(&inf->inf_timer, jiffies + inf->inf_timer_perid);
}

static u32 ioinf_default_inflight(struct gendisk *disk)
{
	return max(disk->queue->nr_requests / 10, 30);
}

static int blk_ioinf_init(struct gendisk *disk)
{
	struct ioinf *inf;
	int ret;

	inf = kzalloc(sizeof(*inf), GFP_KERNEL);
	if (!inf)
		return -ENOMEM;

	inf->stat = alloc_percpu(struct ioinf_lat_stat);
	if (!inf->stat) {
		kfree(inf);
		return -ENOMEM;
	}

	spin_lock_init(&inf->lock);
	inf->params.inflight = ioinf_default_inflight(disk);
	inf->inflight = inf->params.inflight;
	inf->infg_expire_jiffies = INFG_DFL_EXPIRE;
	inf->inf_timer_perid = IOINF_TIMER_PERID;
	inf->offline_hinflight = 1;
	inf->old_scale = 9;
	rq_wait_init(&inf->offline_rqw);

	INIT_LIST_HEAD(&inf->active_infgs);
	INIT_LIST_HEAD(&inf->lend_infgs);
	INIT_LIST_HEAD(&inf->borrow_infgs);
	timer_setup(&inf->inf_timer, ioinf_timer_fn, 0);

	ret = rq_qos_add(&inf->rqos, disk, RQ_QOS_INFLIGHT, &ioinf_rqos_ops);
	if (ret)
		goto err_free_inf;

	ret = blkcg_activate_policy(disk, &blkcg_policy_ioinf);
	if (ret)
		goto err_del_qos;
	return 0;

err_del_qos:
	rq_qos_del(&inf->rqos);
err_free_inf:
	free_percpu(inf->stat);
	kfree(inf);
	return ret;
}

static struct blkcg_policy_data *ioinf_cpd_alloc(gfp_t gfp)
{
	struct ioinf_cgrp *infcg = kzalloc(sizeof(*infcg), gfp);

	if (!infcg)
		return NULL;

	infcg->dfl_weight = IOINF_DFL_WEIGHT;
	return &infcg->cpd;
}

static void ioinf_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(container_of(cpd, struct ioinf_cgrp, cpd));
}

static struct blkg_policy_data *ioinf_pd_alloc(struct gendisk *disk,
					       struct blkcg *blkcg, gfp_t gfp)
{
	struct ioinf_gq *infg = kzalloc_node(sizeof(*infg), gfp, disk->node_id);

	if (!infg)
		return NULL;

	return &infg->pd;
}

static void infg_expire_fn(struct timer_list *timer)
{
	struct ioinf_gq *infg =
		container_of(timer, struct ioinf_gq, expire_timer);
	struct ioinf *inf = infg->inf;
	unsigned long flags;

	if (atomic_read(&infg->rqw.inflight) > 0)
		return;

	spin_lock_irqsave(&inf->lock, flags);
	if (atomic_read(&infg->rqw.inflight) == 0) {
		list_del_init(&infg->active);
		if (atomic_read(&infg->rqw.inflight) == 0) {
			infg_clear_loan(infg);
			__propagate_weights(inf);
		} else {
			list_add(&infg->active, &inf->active_infgs);
		}
	}
	spin_unlock_irqrestore(&inf->lock, flags);
}

static void ioinf_pd_init(struct blkg_policy_data *pd)
{
	struct ioinf_gq *infg = pd_to_infg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(pd);

	INIT_LIST_HEAD(&infg->active);
	INIT_LIST_HEAD(&infg->lend);
	INIT_LIST_HEAD(&infg->borrow);
	infg->inf = q_to_inf(blkg->q);
	rq_wait_init(&infg->rqw);
	timer_setup(&infg->expire_timer, infg_expire_fn, 0);
}

static void ioinf_pd_offline(struct blkg_policy_data *pd)
{
	struct ioinf_gq *infg = pd_to_infg(pd);
	struct ioinf *inf = infg->inf;

	if (list_empty_careful(&infg->active))
		return;

	del_timer_sync(&infg->expire_timer);

	spin_lock_irq(&inf->lock);

	if (!list_empty(&infg->lend))
		list_del_init(&infg->lend);

	if (!list_empty(&infg->borrow))
		list_del_init(&infg->borrow);

	if (!list_empty(&infg->active)) {
		list_del_init(&infg->active);
		__propagate_weights(inf);
	}

	spin_unlock_irq(&inf->lock);
}

static void ioinf_pd_free(struct blkg_policy_data *pd)
{
	struct ioinf_gq *infg = pd_to_infg(pd);

	kfree(infg);
}

static u64 ioinf_weight_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioinf_gq *infg = pd_to_infg(pd);

	if (dname && infg->weight)
		seq_printf(sf, "%s %u\n", dname, infg->weight);

	return 0;
}

static int ioinf_weight_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct ioinf_cgrp *infcg = blkcg_to_infcg(blkcg);

	seq_printf(sf, "default %u\n", infcg->dfl_weight);
	blkcg_print_blkgs(sf, blkcg, ioinf_weight_prfill, &blkcg_policy_ioinf,
			  seq_cft(sf)->private, false);

	return 0;
}

static ssize_t ioinf_weight_write(struct kernfs_open_file *of, char *buf,
				  size_t nbytes, loff_t off)
{
	struct blkcg *blkcg = css_to_blkcg(of_css(of));
	struct ioinf_cgrp *infcg = blkcg_to_infcg(blkcg);
	struct blkg_conf_ctx ctx;
	struct ioinf_gq *infg;
	int ret;
	u32 v;

	if (!strchr(buf, ':')) {
		if (!sscanf(buf, "default %u", &v) && !sscanf(buf, "%u", &v))
			return -EINVAL;
		if (v < 0)
			return -EINVAL;

		infcg->dfl_weight = v;

		return nbytes;
	}

	blkg_conf_init(&ctx, buf);
	ret = blkg_conf_prep(blkcg, &blkcg_policy_ioinf, &ctx);
	if (ret)
		return ret;

	infg = blkg_to_infg(ctx.blkg);
	if (!strncmp(ctx.body, "default", 7)) {
		v = IOINF_DFL_WEIGHT;
	} else if (!sscanf(ctx.body, "%u", &v) ||
		 v < 0 || v > CGROUP_WEIGHT_MAX) {
		blkg_conf_exit(&ctx);
		return -EINVAL;
	}

	infg->weight = v;
	if (v == 0)
		set_bit(INFG_OFFLINE, &infg->flags);
	else
		clear_bit(INFG_OFFLINE, &infg->flags);
	blkg_conf_exit(&ctx);
	propagate_weights(infg->inf);
	return nbytes;
}

static u64 ioinf_qos_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			    int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioinf *inf = q_to_inf(pd->blkg->q);
	struct ioinf_params params;

	if (!dname)
		return 0;

	params = inf->params;
	seq_printf(sf, "%s enable=%d inflight=%u qos_enable=%d", dname,
		   params.enabled, params.inflight, params.qos_enabled);

	if (inf->params.qos_enabled)
		seq_printf(sf, " rlat=%llu rpct=%u wlat=%llu wpct=%u",
			   params.rlat, params.rpct, params.wlat, params.wpct);

	seq_putc(sf, '\n');
	return 0;
}

static int ioinf_qos_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));

	blkcg_print_blkgs(sf, blkcg, ioinf_qos_prfill,
			  &blkcg_policy_ioinf, seq_cft(sf)->private, false);
	return 0;
}

static const match_table_t qos_ctrl_tokens = {
	{ INF_ENABLE,		"enable=%u"	},
	{ INF_INFLIGHT,		"inflight=%u"	},
	{ QOS_ENABLE,		"qos_enable=%u"	},
	{ QOS_RLAT,		"rlat=%u"	},
	{ QOS_WLAT,		"wlat=%u"	},
	{ QOS_RPCT,		"rpct=%u"	},
	{ QOS_WPCT,		"wpct=%u"	},
	{ NR_QOS_CTRL_PARAMS,	NULL		},
};

static ssize_t ioinf_qos_write(struct kernfs_open_file *of, char *input,
			       size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;
	struct gendisk *disk;
	struct ioinf *inf;
	struct ioinf_params params;
	char *body, *p;
	int ret;

	blkg_conf_init(&ctx, input);

	ret = blkg_conf_open_bdev(&ctx);
	if (ret)
		goto err;

	body = ctx.body;
	disk = ctx.bdev->bd_disk;
	if (!queue_is_mq(disk->queue)) {
		ret = -EOPNOTSUPP;
		goto err;
	}

	inf = q_to_inf(disk->queue);
	if (!inf) {
		ret = blk_ioinf_init(disk);
		if (ret)
			goto err;

		inf = q_to_inf(disk->queue);
	}

	params = inf->params;

	while ((p = strsep(&body, " \t\n"))) {
		substring_t args[MAX_OPT_ARGS];
		s64 v;

		if (!*p)
			continue;

		switch (match_token(p, qos_ctrl_tokens, args)) {
		case INF_ENABLE:
			if (match_u64(&args[0], &v))
				goto einval;
			params.enabled = !!v;
			continue;
		case QOS_ENABLE:
			if (match_u64(&args[0], &v))
				goto einval;
			params.qos_enabled = !!v;
			continue;
		case INF_INFLIGHT:
			if (match_u64(&args[0], &v))
				goto einval;
			params.inflight = v;
			continue;
		case QOS_RLAT:
			if (match_u64(&args[0], &v) || v == 0)
				goto einval;
			params.rlat = v;
			continue;
		case QOS_WLAT:
			if (match_u64(&args[0], &v) || v == 0)
				goto einval;
			params.wlat = v;
			continue;
		case QOS_RPCT:
			if (match_u64(&args[0], &v) || v == 0 || v >= 100)
				goto einval;
			params.rpct = v;
			continue;
		case QOS_WPCT:
			if (match_u64(&args[0], &v) || v == 0 || v >= 100)
				goto einval;
			params.wpct = v;
			continue;
		default:
			goto einval;
		}
	}

	if (params.qos_enabled &&
	    (params.rlat == 0 || params.wlat == 0 ||
	     params.rpct == 0 || params.rpct >= 100 ||
	     params.wpct == 0 || params.wpct >= 100))
		goto einval;

	if (params.inflight == 0)
		params.inflight = ioinf_default_inflight(disk);

	if (params.qos_enabled && !inf->params.qos_enabled) {
		blk_stat_enable_accounting(disk->queue);
		blk_queue_flag_set(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);
	} else if (!params.qos_enabled && inf->params.qos_enabled) {
		blk_stat_disable_accounting(disk->queue);
		blk_queue_flag_clear(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);
	}

	inf->params = params;
	inf->busy_level = 0;
	inf->old_scale = 9;
	inf->inflight = params.inflight;
	propagate_weights(inf);

	blkg_conf_exit(&ctx);
	return nbytes;

einval:
	ret = -EINVAL;
err:
	blkg_conf_exit(&ctx);
	return ret;
}

static struct cftype ioinf_files[] = {
	{
		.name = "inf.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = ioinf_weight_show,
		.write = ioinf_weight_write,
	},
	{
		.name = "inf.qos",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = ioinf_qos_show,
		.write = ioinf_qos_write,
	},
	{}
};

static struct cftype ioinf_legacy_files[] = {
	{
		.name = "inf.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = ioinf_weight_show,
		.write = ioinf_weight_write,
	},
	{
		.name = "inf.qos",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = ioinf_qos_show,
		.write = ioinf_qos_write,
	},
	{}
};

static struct blkcg_policy blkcg_policy_ioinf = {
	.dfl_cftypes	= ioinf_files,
	.legacy_cftypes = ioinf_legacy_files,

	.cpd_alloc_fn	= ioinf_cpd_alloc,
	.cpd_free_fn	= ioinf_cpd_free,

	.pd_alloc_fn	= ioinf_pd_alloc,
	.pd_init_fn	= ioinf_pd_init,
	.pd_offline_fn	= ioinf_pd_offline,
	.pd_free_fn	= ioinf_pd_free,
};

static int __init ioinf_init(void)
{
	return blkcg_policy_register(&blkcg_policy_ioinf);
}

static void __exit ioinf_exit(void)
{
	blkcg_policy_unregister(&blkcg_policy_ioinf);
}

module_init(ioinf_init);
module_exit(ioinf_exit);
