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
#define IOINF_DFL_WEIGHT	0
#define IOINF_MIN_INFLIGHT	30
#define IOINFG_MIN_INFLIGHT	1
/* default wake-up time in jiffies for backgroup job, see ioinf_timer_fn() */
#define IOINF_TIMER_PERID	500

/* io.inf.qos controls */
enum {
	INF_ENABLE,
	INF_INFLIGHT,

	NR_QOS_CTRL_PARAMS,
};

/* qos control params */
struct ioinf_params {
	bool enabled;
	u32 inflight;
};

struct ioinf_io_stat {
	u64 nr;
	u64 lat;
};

struct ioinf_lat_stat {
	struct ioinf_io_stat read;
	struct ioinf_io_stat write;
};

struct ioinf_rq_wait {
	struct rq_wait rqw;
	u32 hinflight;
	u32 max_inflight;
	u32 last_max;
	u32 exhausted;
	u32 issued;
};

/* the global conrtol structure */
struct ioinf {
	struct rq_qos		rqos;

	struct ioinf_params	params;
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

	struct ioinf_lat_stat	last_stat;
	struct ioinf_lat_stat	delta_stat;
	struct ioinf_lat_stat __percpu *stat;
};

/* per disk-cgroup pair structure */
struct ioinf_gq {
	struct blkg_policy_data	pd;
	struct ioinf		*inf;

	/* configured by user */
	u32			user_weight;
};

/* per cgroup structure, used to record default weight for all disks */
struct ioinf_cgrp {
	struct blkcg_policy_data	cpd;

	/* if default user weight is 0, means it's offline */
	u32				dfl_user_weight;
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

static struct ioinf_rq_wait *rqw_to_ioinf_rqw(struct rq_wait *rqw)
{
	return container_of(rqw, struct ioinf_rq_wait, rqw);
}

static u32 infg_user_weight(struct ioinf_gq *infg)
{
	struct ioinf_cgrp *infcg;
	struct blkcg_gq *blkg;

	if (infg->user_weight)
		return infg->user_weight;

	/* if user doesn't set per disk weight, use the cgroup default weight */
	blkg = infg_to_blkg(infg);
	infcg = blkcg_to_infcg(blkg->blkcg);

	return infcg->dfl_user_weight;
}

static bool infg_offline(struct ioinf_gq *infg)
{
	return infg_user_weight(infg) == 0;
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

static void ioinf_set_hinflight(struct ioinf_rq_wait *ioinf_rqw, u32 new)
{
	u32 old = ioinf_rqw->hinflight;

	ioinf_rqw->hinflight = new;
	ioinf_rqw->last_max = max(ioinf_rqw->last_max >> 1,
				  ioinf_rqw->max_inflight);
	ioinf_rqw->max_inflight = new >> 1;

	if (new > old && wq_has_sleeper(&ioinf_rqw->rqw.wait))
		wake_up_all(&ioinf_rqw->rqw.wait);
}

void ioinf_done(struct ioinf_rq_wait *ioinf_rqw)
{
	int inflight = atomic_dec_return(&ioinf_rqw->rqw.inflight);

	BUG_ON(inflight < 0);

	if (inflight < ioinf_rqw->hinflight &&
	    wq_has_sleeper(&ioinf_rqw->rqw.wait))
		wake_up_all(&ioinf_rqw->rqw.wait);
}

static bool ioinf_inflight_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf_rq_wait *ioinf_rqw = rqw_to_ioinf_rqw(rqw);
	struct ioinf *inf = private_data;
	u32 inflight;
	u32 limit;

retry:
	limit = ioinf_rqw->hinflight;
	inflight = atomic_inc_below_return(&rqw->inflight, limit);
	if (inflight > ioinf_rqw->max_inflight)
		ioinf_rqw->max_inflight = inflight;
	if (inflight <= limit) {
		ioinf_rqw->issued++;
		return true;
	}

	if (ioinf_rqw == &inf->offline) {
		ioinf_rqw->exhausted++;
		return false;
	}

	if (inf->offline.hinflight > IOINFG_MIN_INFLIGHT) {
		spin_lock_irq(&inf->lock);
		/* Reclaim half of the inflight budget from offline groups. */
		ioinf_set_hinflight(&inf->offline,
				    inf->offline.hinflight >> 1);
		ioinf_set_hinflight(&inf->online,
				    inf->inflight - inf->offline.hinflight);
		spin_unlock_irq(&inf->lock);
	}

	if (ioinf_rqw->hinflight > limit)
		goto retry;

	ioinf_rqw->exhausted++;
	return false;
}

static void ioinf_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	struct ioinf_rq_wait *ioinf_rqw = rqw_to_ioinf_rqw(rqw);

	ioinf_done(ioinf_rqw);
}

static void ioinf_throttle(struct ioinf *inf, struct ioinf_rq_wait *ioinf_rqw)
{
	rq_qos_wait(&ioinf_rqw->rqw, inf, ioinf_inflight_cb,
		    ioinf_cleanup_cb, NULL);

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
		ioinf_throttle(inf, &inf->offline);
	else
		ioinf_throttle(inf, &inf->online);
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

	switch(req_op(rq)) {
	case REQ_OP_READ:
		this_cpu_inc(inf->stat->read.nr);
		this_cpu_add(inf->stat->read.lat, lat);
		break;
	case REQ_OP_WRITE:
		this_cpu_inc(inf->stat->write.nr);
		this_cpu_add(inf->stat->write.lat, lat);
		break;
	default:
		break;
	}
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
		ioinf_done(&inf->offline);
	} else {
		ioinf_done(&inf->online);
		ioinf_record_lat(inf, rq);
	}

	rq->blkg = NULL;
}

static void ioinf_rqos_exit(struct rq_qos *rqos)
{
	struct ioinf *inf = rqos_to_inf(rqos);

	blkcg_deactivate_policy(rqos->disk, &blkcg_policy_ioinf);

	timer_shutdown_sync(&inf->inf_timer);
	free_percpu(inf->stat);
	kfree(inf);
}

static int ioinf_stat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct ioinf *inf = rqos_to_inf(rqos);

	spin_lock_irq(&inf->lock);

	seq_printf(m, "inflight %u->%u\n", inf->params.inflight, inf->inflight);

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
	struct ioinf_lat_stat *stat = &inf->delta_stat;

	seq_printf(m, "online average latency: (%llu-%llu) (%llu-%llu)\n",
		stat->read.nr, stat->read.lat, stat->write.nr, stat->write.lat);

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

u32 ioinf_calc_budget(struct ioinf_rq_wait *ioinf_rqw)
{
	u32 new_budget;
	u64 exhausted = ioinf_rqw->exhausted;
	u64 issued = ioinf_rqw->issued;

	new_budget = max(ioinf_rqw->last_max, ioinf_rqw->max_inflight);
	/* How much budget is needed to avoid 'exhausted'? */
	if (exhausted && issued)
		new_budget += exhausted * new_budget / issued;

	return new_budget;
}

static void ioinf_sample_cpu_lat(struct ioinf_lat_stat *cur, int cpu,
				 struct ioinf_lat_stat __percpu *stat)
{
	struct ioinf_lat_stat *pstat = per_cpu_ptr(stat, cpu);

	cur->read.nr += pstat->read.nr;
	cur->read.lat += pstat->read.lat;
	cur->write.nr += pstat->write.nr;
	cur->write.lat += pstat->write.lat;
}

static struct ioinf_lat_stat ioinf_calc_stat(struct ioinf_lat_stat *cur,
					     struct ioinf_lat_stat *last)
{
	struct ioinf_lat_stat delta = {0};

	delta.read.nr = cur->read.nr - last->read.nr;
	delta.read.lat = cur->read.lat - last->read.lat;
	if (delta.read.nr > 0)
		delta.read.lat = delta.read.lat / delta.read.nr;

	delta.write.nr = cur->write.nr - last->write.nr;
	delta.write.lat = cur->write.lat - last->write.lat;
	if (delta.write.nr > 0)
		delta.write.lat = delta.write.lat / delta.write.nr;

	return delta;
}

static void ioinf_sample_lat(struct ioinf *inf)
{
	struct ioinf_lat_stat cur = {0};
	int cpu;

	for_each_possible_cpu(cpu)
		ioinf_sample_cpu_lat(&cur, cpu, inf->stat);
	inf->delta_stat = ioinf_calc_stat(&cur, &inf->last_stat);
	inf->last_stat = cur;
}

static
void ioinf_update_inflight(struct ioinf *inf, u32 new_online, u32 new_offline)
{
	inf->inflight = inf->params.inflight;
	if (inf->inflight < IOINF_MIN_INFLIGHT)
		inf->inflight = IOINF_MIN_INFLIGHT;

	if (new_online >= inf->inflight)
		new_offline = min(new_offline, IOINFG_MIN_INFLIGHT);
	else if (new_online + new_offline > inf->inflight)
		new_offline = inf->inflight - new_online;
	new_online = inf->inflight - new_offline;

	ioinf_set_hinflight(&inf->offline, new_offline);
	inf->offline.exhausted = 0;
	inf->offline.issued = 0;

	ioinf_set_hinflight(&inf->online, new_online);
	inf->online.exhausted = 0;
	inf->online.issued = 0;
}

static void ioinf_timer_fn(struct timer_list *timer)
{
	struct ioinf *inf = container_of(timer, struct ioinf, inf_timer);
	struct ioinf_rq_wait *online = &inf->online;
	struct ioinf_rq_wait *offline = &inf->offline;
	unsigned long flags;
	u32 online_budget, offline_budget;

	ioinf_sample_lat(inf);
	spin_lock_irqsave(&inf->lock, flags);

	online_budget = ioinf_calc_budget(online);
	offline_budget = ioinf_calc_budget(offline);
	ioinf_update_inflight(inf, online_budget, offline_budget);

	spin_unlock_irqrestore(&inf->lock, flags);
	mod_timer(&inf->inf_timer, jiffies + inf->inf_timer_perid);
}

static u32 ioinf_default_inflight(struct gendisk *disk)
{
	return max(disk->queue->nr_requests, IOINF_MIN_INFLIGHT);
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
	inf->offline.hinflight = IOINFG_MIN_INFLIGHT;
	rq_wait_init(&inf->offline.rqw);
	inf->online.hinflight = inf->inflight - IOINFG_MIN_INFLIGHT;
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
	timer_shutdown_sync(&inf->inf_timer);
	free_percpu(inf->stat);
	kfree(inf);
	return ret;
}

static u64 ioinf_weight_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioinf_gq *infg = pd_to_infg(pd);

	if (dname && infg->user_weight)
		seq_printf(sf, "%s %u\n", dname, infg->user_weight);

	return 0;
}

static int ioinf_weight_show(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct ioinf_cgrp *infcg = blkcg_to_infcg(blkcg);

	seq_printf(sf, "default %u\n", infcg->dfl_user_weight);
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

		infcg->dfl_user_weight = v;

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

	infg->user_weight = v;
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
	seq_printf(sf, "%s enable=%d inflight=%u", dname,
		   params.enabled, params.inflight);

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
	{ NR_QOS_CTRL_PARAMS,	NULL		},
};

static ssize_t ioinf_qos_write(struct kernfs_open_file *of, char *input,
			       size_t nbytes, loff_t off)
{
	struct blkg_conf_ctx ctx;
	struct gendisk *disk;
	struct ioinf *inf;
	struct ioinf_params params = {0};
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
	if (inf)
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
		case INF_INFLIGHT:
			if (match_u64(&args[0], &v) || v == 0)
				goto einval;
			params.inflight = v;
			continue;
		default:
			goto einval;
		}
	}

	if (!inf && params.enabled) {
		ret = blk_ioinf_init(disk);
		if (ret)
			goto err;
		inf = q_to_inf(disk->queue);
		if (!params.inflight)
			params.inflight = inf->params.inflight;
		blk_queue_flag_set(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);
	} else if (inf && !params.enabled) {
		blk_queue_flag_clear(QUEUE_FLAG_RQ_ALLOC_TIME, disk->queue);
		timer_shutdown_sync(&inf->inf_timer);
		blkcg_deactivate_policy(inf->rqos.disk, &blkcg_policy_ioinf);
		rq_qos_del(&inf->rqos);
		kfree(inf);
		inf = NULL;
	}

	if (inf) {
		inf->params = params;
		if (inf->inflight != params.inflight) {
			spin_lock_irq(&inf->lock);
			ioinf_update_inflight(inf, inf->online.hinflight,
					      inf->offline.hinflight);
			spin_unlock_irq(&inf->lock);
		}
	}

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

static struct blkcg_policy_data *ioinf_cpd_alloc(gfp_t gfp)
{
	struct ioinf_cgrp *infcg = kzalloc(sizeof(*infcg), gfp);

	if (!infcg)
		return NULL;

	infcg->dfl_user_weight = IOINF_DFL_WEIGHT;
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
