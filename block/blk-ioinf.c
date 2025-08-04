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
/* default wake-up time in jiffies for backgroup job, see ioinf_timer_fn() */
#define IOINF_TIMER_PERID	500
/* default time in jiffies that cgroup will idle without any IO */
#define MIN_SAMPLES		100
#define MAX_BUSY_LEVEL		90
#define MIN_BUSY_LEVEL		-90

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
	u64 rlatency;
	u64 wlatency;
};

struct ioinf_rq_wait {
	struct rq_wait rqw;
	u32 hinflight;
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

	/* global lock */
	spinlock_t		lock;

	/* for offline cgroups */
	struct ioinf_rq_wait	offline;
	/* for online cgroups */
	struct ioinf_rq_wait	online;
	u32 			max_inflight;
	u32 			last_max_inflight;

	struct ioinf_lat_stat	last_stat;
	struct ioinf_lat_stat __percpu *stat;

	int			busy_level;
	int			last_busy_level;
	int			old_scale;
};

/* per disk-cgroup pair structure */
struct ioinf_gq {
	struct blkg_policy_data	pd;
	struct ioinf		*inf;

	/* configured by user */
	u32			weight;
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

void ioinf_global_done(struct ioinf_rq_wait *ioinf_rqw)
{
	int inflight = atomic_dec_return(&ioinf_rqw->rqw.inflight);

	if (inflight < ioinf_rqw->hinflight &&
	    wq_has_sleeper(&ioinf_rqw->rqw.wait))
		wake_up_all(&ioinf_rqw->rqw.wait);
}

static bool infg_offline(struct ioinf_gq *infg)
{
	struct ioinf_cgrp *infcg;
	struct blkcg_gq *blkg;

	if (infg->weight != 0)
		return false;

	/* if user doesn't set per disk weight, use the cgroup default weight */
	blkg = infg_to_blkg(infg);
	infcg = blkcg_to_infcg(blkg->blkcg);

	return infcg->dfl_weight == 0;
}

static struct ioinf_rq_wait *rqw_to_ioinf_rqw(struct rq_wait *rqw)
{
	return container_of(rqw, struct ioinf_rq_wait, rqw);
}

static bool ioinf_global_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf_rq_wait *ioinf_rqw = rqw_to_ioinf_rqw(rqw);
	struct ioinf *inf = private_data;
	u32 inflight;
	u32 limit;

	if (ioinf_rqw == &inf->offline)
		return rq_wait_inc_below(rqw, ioinf_rqw->hinflight);
retry:
	limit = ioinf_rqw->hinflight;
	inflight = atomic_inc_below_return(&rqw->inflight, limit);
	if (inflight > inf->max_inflight)
		inf->max_inflight = inflight;
	if (inflight <= limit)
		return true;

	if (ioinf_rqw->hinflight < inf->inflight - 1) {
		/* Stop lend inflight budget to offline groups */
		inf->offline.hinflight = 1;
		ioinf_rqw->hinflight = inf->inflight - 1;
		goto retry;
	}

	return false;
}

static void ioinf_global_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf_rq_wait *ioinf_rqw = rqw_to_ioinf_rqw(rqw);

	ioinf_global_done(ioinf_rqw);
}

static void
ioinf_throttle_global(struct ioinf *inf, struct ioinf_rq_wait *ioinf_rqw)
{
	rq_qos_wait(&ioinf_rqw->rqw, inf, ioinf_global_inflight_cb,
		    ioinf_global_cleanup_cb, NULL);

	/*
	 * In case no online cgroup is active, daemon will adjust all the
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

	if (infg_offline(infg))
		ioinf_throttle_global(inf, &inf->offline);
	else
		ioinf_throttle_global(inf, &inf->online);
}

static void ioinf_rqos_track(struct rq_qos *rqos, struct request *rq,
			     struct bio *bio)
{
	struct blkcg_gq *blkg = ioinf_bio_blkg(bio);

	if (!blkg)
		return;

	rq->blkg = blkg;
}

static void ioinf_record_lat(struct ioinf *inf, struct request *rq)
{
	u64 lat;

	lat = rq->io_end_time_ns ? rq->io_end_time_ns : blk_time_get_ns();
	lat -= rq->alloc_time_ns;

	if (!inf->params.qos_enabled)
		return;

	switch(req_op(rq)) {
	case REQ_OP_READ:
		if (lat > inf->params.rlat)
			this_cpu_inc(inf->stat->rmissed);
		else
			this_cpu_inc(inf->stat->rmet);
		this_cpu_add(inf->stat->rlatency, lat);
		break;
	case REQ_OP_WRITE:
		if (lat > inf->params.wlat)
			this_cpu_inc(inf->stat->wmissed);
		else
			this_cpu_inc(inf->stat->wmet);
		this_cpu_add(inf->stat->wlatency, lat);
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
		stat.rlatency += pstat->rlatency;
		stat.wlatency += pstat->wlatency;
	}

	return stat;
}

static void ioinf_rqos_done(struct rq_qos *rqos, struct request *rq)
{
	struct blkcg_gq *blkg = rq->blkg;
	struct ioinf_gq *infg;
	struct ioinf *inf;

	if (!blkg)
		return;

	infg = blkg_to_infg(blkg);
	inf = infg->inf;
	if (infg_offline(infg)) {
		ioinf_global_done(&inf->offline);
	} else {
		ioinf_global_done(&inf->online);
		ioinf_record_lat(inf, rq);
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

	spin_lock_irq(&inf->lock);

	seq_printf(m, "busy_level %d inflight %u->%u\n", inf->busy_level,
		   inf->params.inflight, inf->inflight);
	seq_printf(m, "online inflight %u/%d\n",
		   atomic_read(&inf->online.rqw.inflight),
		   inf->online.hinflight);
	seq_printf(m, "offline inflight %u/%d\n",
		   atomic_read(&inf->offline.rqw.inflight),
		   inf->offline.hinflight);

	spin_unlock_irq(&inf->lock);

	return 0;
}

static int ioinf_lat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_lat_stat stat;
	struct ioinf_lat_stat *last;
	u64 nr_read_io, nr_write_io;
	u64 rlatency = 0, wlatency = 0;

	if (!inf->params.qos_enabled)
		return 0;

	stat = ioinf_get_lat(inf);
	last = &inf->last_stat;
	seq_printf(m, "qos lat: %llu %llu %llu %llu\n",
		   stat.rmet, stat.rmissed, stat.wmet, stat.wmissed);

	nr_read_io = stat.rmet - last->rmet;
	nr_read_io += stat.rmissed - last->rmissed;
	if (nr_read_io > 0)
		rlatency = (stat.rlatency - last->rlatency) / nr_read_io;

	nr_write_io = stat.wmet - last->wmet;
	nr_write_io += stat.wmissed - last->wmissed;
	if (nr_write_io > 0)
		wlatency = (stat.wlatency - last->wlatency) / nr_write_io;

	seq_printf(m, "online average latency: (%llu-%llu) (%llu-%llu)\n",
		   nr_read_io, rlatency, nr_write_io, wlatency);

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
	.exit		= ioinf_rqos_exit,

#ifdef CONFIG_BLK_DEBUG_FS
	.debugfs_attrs = ioinf_debugfs_attrs,
#endif
};

static void
ioinf_set_global_inflight(struct ioinf_rq_wait *ioinf_rqw, u32 inflight)
{
	bool budget_increased = false;

	if (inflight > ioinf_rqw->hinflight)
		budget_increased = true;

	ioinf_rqw->hinflight = inflight;

	if (budget_increased && wq_has_sleeper(&ioinf_rqw->rqw.wait))
		wake_up_all(&ioinf_rqw->rqw.wait);
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

static void ioinf_adjust_busy_level(struct ioinf *inf, int old_busy_level)
{
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
}

static void ioinf_timer_global_fn(struct timer_list *timer)
{
	struct ioinf *inf = container_of(timer, struct ioinf, inf_timer);
	bool busy = ioinf_online_busy(inf);
	struct ioinf_rq_wait *online = &inf->online;
	struct ioinf_rq_wait *offline = &inf->offline;
	int old_busy_level = inf->busy_level;
	unsigned long flags;
	u32 last_max_inflight;
	u32 new_budget;

	spin_lock_irqsave(&inf->lock, flags);

	/* First take back the offline budget. */
	if (busy && offline->hinflight > 1) {
		offline->hinflight = 1;
		ioinf_set_global_inflight(online, inf->inflight - 1);
		goto unlock;
	}

	if (busy) { /* slow down */
		int min_level = inf->busy_level + 1;
		int max_level = MAX_BUSY_LEVEL;

		if (inf->last_busy_level > min_level)
			max_level = inf->last_busy_level;
		inf->last_busy_level = inf->busy_level;
		inf->busy_level = (min_level + max_level) / 2;
	} else if (inf->params.qos_enabled) {
		int min_level = MIN_BUSY_LEVEL;
		int max_level = inf->busy_level - 1;

		if (inf->last_busy_level < max_level)
			min_level = inf->last_busy_level;
		inf->last_busy_level = inf->busy_level;
		inf->busy_level = (min_level + max_level) / 2;
	}
	ioinf_adjust_busy_level(inf, old_busy_level);

	last_max_inflight = inf->last_max_inflight;
	new_budget = max(last_max_inflight, inf->max_inflight);
	inf->last_max_inflight = max(last_max_inflight >> 1, inf->max_inflight);
	inf->max_inflight = inf->max_inflight >> 1;

	if (busy || inf->inflight <= new_budget) {
		offline->hinflight = 1;
		ioinf_set_global_inflight(online, inf->inflight - 1);
	} else {
		ioinf_set_global_inflight(online, new_budget);
		ioinf_set_global_inflight(offline,
					  inf->inflight - new_budget);
	}
unlock:
	spin_unlock_irqrestore(&inf->lock, flags);
	mod_timer(&inf->inf_timer, jiffies + inf->inf_timer_perid);
}

static void ioinf_timer_fn(struct timer_list *timer)
{
	ioinf_timer_global_fn(timer);
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
	inf->inf_timer_perid = IOINF_TIMER_PERID;
	inf->offline.hinflight = 1;
	inf->old_scale = 9;
	rq_wait_init(&inf->offline.rqw);
	inf->online.hinflight = IOINF_DFL_WEIGHT;
	rq_wait_init(&inf->online.rqw);
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

static void ioinf_pd_init(struct blkg_policy_data *pd)
{
	struct ioinf_gq *infg = pd_to_infg(pd);
	struct blkcg_gq *blkg = pd_to_blkg(pd);

	infg->inf = q_to_inf(blkg->q);
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
	blkg_conf_exit(&ctx);
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
