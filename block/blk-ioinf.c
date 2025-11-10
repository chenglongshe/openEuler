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
#include "blk-rq-qos.h"
#include "blk-mq.h"

/* default weight for each cgroup */
#define IOINF_DFL_WEIGHT	10
/* default wake up time in jiffies for backgroup job, see ioinf_timer_fn() */
#define IOINF_TIMER_PERID	500
/* default time in jiffies that cgroup will idle without any IO */
#define INFG_DFL_EXPIRE		100

/* io.inf.qos controls */
enum {
	QOS_ENABLE,
	QOS_INFLIGHT,
	NR_QOS_CTRL_PARAMS,
};

/* ioinf_gq flags */
enum {
	INFG_EXHAUSTED,
	INFG_LEND,
	INFG_BORROW,
};

/* the global conrtol structure */
struct ioinf {
	struct rq_qos		rqos;

	/* qos control params */
	bool			enabled;
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
};

/* per cgroup structure, used to record default weight for all disks */
struct ioinf_cgrp {
	struct blkcg_policy_data	cpd;

	u32				dfl_weight;
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

static void ioinf_cleanup_cb(struct rq_wait *rqw, void *private_data)
{
	ioinf_done(private_data);
}

static void ioinf_rqos_throttle(struct rq_qos *rqos, struct bio *bio)
{
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_gq *infg = ioinf_bio_infg(bio);

	if (!inf->enabled || !infg)
		return;

	if (list_empty_careful(&infg->active))
		ioinf_active_infg(infg);

	rq_qos_wait(&infg->rqw, infg, ioinf_inflight_cb, ioinf_cleanup_cb);
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

	if (!infg || infg->inf->enabled ||
	    list_empty_careful(&infg->active))
		return;

	ioinf_done(infg);
}

static void ioinf_rqos_done(struct rq_qos *rqos, struct request *rq)
{
	struct blkcg_gq *blkg = rq->blkg;

	if (blkg) {
		ioinf_done(blkg_to_infg(blkg));
		rq->blkg = NULL;
	}
}

static void ioinf_rqos_exit(struct rq_qos *rqos)
{
	struct ioinf *inf = rqos_to_inf(rqos);

	blkcg_deactivate_policy(rqos->q, &blkcg_policy_ioinf);

	del_timer_sync(&inf->inf_timer);
	kfree(inf);
}

static int ioinf_stat_show(void *data, struct seq_file *m)
{
	struct rq_qos *rqos = data;
	struct ioinf *inf = rqos_to_inf(rqos);
	struct ioinf_gq *infg;
	char path[32];

	spin_lock_irq(&inf->lock);
	list_for_each_entry(infg, &inf->active_infgs, active) {
		blkg_path(infg_to_blkg(infg), path, sizeof(path));
		seq_printf(m, "%s: hweight %u, inflight %d/(%d->%d) %u->%u\n", path,
			   infg->hweight, atomic_read(&infg->rqw.inflight),
			   infg->hinflight, infg->hinflight_inuse,
			   infg->last_max_inflight,
			   infg->max_inflight);
	}
	spin_unlock_irq(&inf->lock);

	return 0;
}

static const struct blk_mq_debugfs_attr ioinf_debugfs_attrs[] = {
	{"stat", 0400, ioinf_stat_show},
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

	if (infg->hinflight_inuse < infg->hinflight &&
	    list_empty(&infg->lend)) {
		if (!list_empty(&infg->borrow)) {
			clear_bit(INFG_BORROW, &infg->flags);
			list_del_init(&infg->borrow);
		}

		set_bit(INFG_LEND, &infg->flags);
		list_add_tail(&infg->lend, &infg->inf->lend_infgs);
	}

	if (test_bit(INFG_EXHAUSTED, &infg->flags)) {
		(*exhausted_count)++;
		if (list_empty(&infg->borrow)) {
			set_bit(INFG_BORROW, &infg->flags);
			list_add_tail(&infg->borrow, &infg->inf->borrow_infgs);
		}
	}
}

static void ioinf_timer_fn(struct timer_list *timer)
{
	struct ioinf *inf = container_of(timer, struct ioinf, inf_timer);
	struct ioinf_gq *infg;
	u32 exhausted_count = 0;
	u32 lend_total = 0;
	unsigned long flags;

	if (list_empty(&inf->active_infgs))
		return;

	spin_lock_irqsave(&inf->lock, flags);

	list_for_each_entry(infg, &inf->active_infgs, active)
		infg_update_inflight(infg, &exhausted_count);

	list_for_each_entry(infg, &inf->lend_infgs, lend)
		lend_total += infg->hinflight - infg->hinflight_inuse;

	/*
	 * TODO: handle loan gracefully, equal division for now.
	 */
	if (exhausted_count) {
		u32 borrow = lend_total / exhausted_count;

		list_for_each_entry(infg, &inf->borrow_infgs, borrow) {
			if (test_and_clear_bit(INFG_EXHAUSTED, &infg->flags))
				infg->hinflight_inuse += borrow;
		}
	}

	spin_unlock_irqrestore(&inf->lock, flags);
}

static int blk_ioinf_init(struct request_queue *q)
{
	struct rq_qos *rqos;
	struct ioinf *inf;
	int ret;

	inf = kzalloc_node(sizeof(*inf), GFP_KERNEL, q->node);
	if (!inf)
		return -ENOMEM;

	spin_lock_init(&inf->lock);
	inf->inflight = q->nr_requests;
	inf->infg_expire_jiffies = INFG_DFL_EXPIRE;
	inf->inf_timer_perid = IOINF_TIMER_PERID;
	INIT_LIST_HEAD(&inf->active_infgs);
	INIT_LIST_HEAD(&inf->lend_infgs);
	INIT_LIST_HEAD(&inf->borrow_infgs);
	rqos = &inf->rqos;

	rqos->q = q;
	rqos->id = RQ_QOS_INFLIGHT;
	rqos->ops = &ioinf_rqos_ops;

	timer_setup(&inf->inf_timer, ioinf_timer_fn, 0);

	ret = rq_qos_add(q, rqos);
	if (ret)
		goto err_free_inf;

	ret = blkcg_activate_policy(q, &blkcg_policy_ioinf);
	if (ret)
		goto err_del_qos;
	return 0;

err_del_qos:
	rq_qos_del(q, rqos);
err_free_inf:
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

static struct blkg_policy_data *ioinf_pd_alloc(gfp_t gfp,
					       struct request_queue *q,
					       struct blkcg *blkcg)
{
	struct ioinf_gq *infg = kzalloc_node(sizeof(*infg), gfp, q->node);

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
		if (v <= 0)
			return -EINVAL;

		infcg->dfl_weight = v;

		return nbytes;
	}

	ret = blkg_conf_prep(blkcg, &blkcg_policy_ioinf, buf, &ctx);
	if (ret)
		return ret;

	infg = blkg_to_infg(ctx.blkg);
	if (!strncmp(ctx.body, "default", 7)) {
		v = IOINF_DFL_WEIGHT;
	} else if (!sscanf(ctx.body, "%u", &v) ||
		 v < CGROUP_WEIGHT_MIN || v > CGROUP_WEIGHT_MAX) {
		blkg_conf_finish(&ctx);
		return -EINVAL;
	}

	infg->weight = v;
	blkg_conf_finish(&ctx);
	propagate_weights(infg->inf);
	return nbytes;
}

static u64 ioinf_qos_prfill(struct seq_file *sf, struct blkg_policy_data *pd,
			    int off)
{
	const char *dname = blkg_dev_name(pd->blkg);
	struct ioinf *inf = q_to_inf(pd->blkg->q);

	if (!dname)
		return 0;

	seq_printf(sf, "%s enable=%d inflight=%u\n", dname, inf->enabled,
		   inf->inflight);
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
	{ QOS_ENABLE,		"enable=%u"	},
	{ QOS_INFLIGHT,		"inflight=%u"	},
	{ NR_QOS_CTRL_PARAMS,	NULL		},
};

static ssize_t ioinf_qos_write(struct kernfs_open_file *of, char *input,
			       size_t nbytes, loff_t off)
{
	struct gendisk *disk;
	struct ioinf *inf;
	u32 inflight;
	bool enable;
	char *p;
	int ret;

	disk = blkcg_conf_get_disk(&input);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	if (!queue_is_mq(disk->queue)) {
		ret = -EOPNOTSUPP;
		goto err;
	}

	inf = q_to_inf(disk->queue);
	if (!inf) {
		ret = blk_ioinf_init(disk->queue);
		if (ret)
			goto err;

		inf = q_to_inf(disk->queue);
	}

	enable = inf->enabled;
	inflight = inf->inflight;

	while ((p = strsep(&input, " \t\n"))) {
		substring_t args[MAX_OPT_ARGS];
		s64 v;

		if (!*p)
			continue;

		switch (match_token(p, qos_ctrl_tokens, args)) {
		case QOS_ENABLE:
			if (match_u64(&args[0], &v))
				goto einval;
			enable = !!v;
			continue;
		case QOS_INFLIGHT:
			if (match_u64(&args[0], &v))
				goto einval;
			inflight = v;
			continue;
		default:
			goto einval;
		}
	}

	inf->enabled = enable;

	if (inflight == 0)
		inflight = disk->queue->nr_requests;

	if (inf->inflight != inflight) {
		inf->inflight = inflight;
		propagate_weights(inf);
	}

	put_disk_and_module(disk);
	return nbytes;

einval:
	ret = -EINVAL;
err:
	put_disk_and_module(disk);
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
