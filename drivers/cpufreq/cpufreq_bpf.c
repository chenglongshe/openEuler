// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>
#include <linux/percpu-defs.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include "cpufreq_governor.h"

#define DEFAULT_NAME "performance"

static DEFINE_MUTEX(cg_bpf_mutex);

enum {
	CPUFREQ_BPF_NONE = 0,
	CPUFREQ_BPF_INIT,
	CPUFREQ_BPF_LOADED,
};

struct bpf_tuner {
	unsigned int bpf_gov_stat;
};

struct bpf_policy {
	struct policy_dbs_info policy_dbs;
	unsigned long next_freq;
};

struct cg_bpf_ops {
	unsigned long (*get_next_freq)(struct cpufreq_policy *policy);
	unsigned int (*get_sampling_rate)(struct cpufreq_policy *policy);
	unsigned int (*init)(void);
	void (*exit)(void);
	char name[128];
};

static unsigned long get_next_freq_nop(struct cpufreq_policy *policy) { return policy->max; }

static unsigned int get_sampling_rate_nop(struct cpufreq_policy *policy) { return 0; }

static unsigned int init_nop(void) { return 0; }

static void exit_nop(void) { }

static struct cg_bpf_ops bpf_cg_bpf_ops = {
	.get_next_freq = get_next_freq_nop,
	.get_sampling_rate = get_sampling_rate_nop,
	.init = init_nop,
	.exit = exit_nop,
};

static struct static_key_false cg_bpf_gov_load;

static struct cg_bpf_ops cg_bpf_ops_global;

static struct bpf_tuner *bpf_global_tuner;

static const struct btf_type *cpufreq_policy_type;
static u32 cpufreq_policy_type_id;

static int cg_bpf_struct_access(struct bpf_verifier_log *log,
			     const struct bpf_reg_state *reg, int off,
			     int size)
{
	const struct btf_type *t;

	t = btf_type_by_id(reg->btf, reg->btf_id);

	if (t == cpufreq_policy_type) {
		if (off >= offsetof(struct cpufreq_policy, cpus) &&
		    off + size <= offsetofend(struct cpufreq_policy, nb_max))
			return SCALAR_VALUE;
	}

	return -EACCES;
}

static const struct bpf_verifier_ops cg_bpf_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = btf_ctx_access,
	.btf_struct_access = cg_bpf_struct_access,
};

static int cg_bpf_init_member(const struct btf_type *t, const struct btf_member *member,
			   void *kdata, const void *udata)
{
	const struct cg_bpf_ops *uops = udata;
	struct cg_bpf_ops *ops = kdata;
	u32 offset = __btf_member_bit_offset(t, member) / 8;
	int ret;

	switch (offset) {
	case offsetof(struct cg_bpf_ops, name):
		ret = bpf_obj_name_cpy(ops->name, uops->name,
				       sizeof(ops->name));
		if (ret <= 0)
			return -EINVAL;
		return 1;
	}
	return 0;
}

static int cg_bpf_check_member(const struct btf_type *t,
			    const struct btf_member *member,
			    const struct bpf_prog *prog)
{
	u32 offset = __btf_member_bit_offset(t, member) / 8;

	switch (offset) {
	case offsetof(struct cg_bpf_ops, get_next_freq):
	case offsetof(struct cg_bpf_ops, get_sampling_rate):
	case offsetof(struct cg_bpf_ops, init):
	case offsetof(struct cg_bpf_ops, exit):
	case offsetof(struct cg_bpf_ops, name):
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void cg_bpf_disable(void)
{
	static_branch_disable(&cg_bpf_gov_load);
	bpf_global_tuner->bpf_gov_stat = CPUFREQ_BPF_INIT;
	cg_bpf_ops_global.get_next_freq = get_next_freq_nop;
	cg_bpf_ops_global.get_sampling_rate = get_sampling_rate_nop;
	cg_bpf_ops_global.init = init_nop;
	cg_bpf_ops_global.exit = exit_nop;
	strscpy(cg_bpf_ops_global.name, DEFAULT_NAME, strlen(DEFAULT_NAME));
}

static int cg_bpf_reg(void *kdata, struct bpf_link *link)
{
	struct cg_bpf_ops *ops = (struct cg_bpf_ops *)kdata;

	mutex_lock(&cg_bpf_mutex);

	if (bpf_global_tuner == NULL) {
		mutex_unlock(&cg_bpf_mutex);
		return -EEXIST;
	}

	if (bpf_global_tuner->bpf_gov_stat != CPUFREQ_BPF_INIT) {
		mutex_unlock(&cg_bpf_mutex);
		return -EEXIST;
	}

	bpf_global_tuner->bpf_gov_stat = CPUFREQ_BPF_LOADED;
	cg_bpf_ops_global = *ops;

	if (cg_bpf_ops_global.init && cg_bpf_ops_global.init()) {
		cg_bpf_disable();
		mutex_unlock(&cg_bpf_mutex);
		return -EINVAL;
	}

	static_branch_enable(&cg_bpf_gov_load);
	mutex_unlock(&cg_bpf_mutex);
	return 0;
}

static void cg_bpf_unreg(void *kdata, struct bpf_link *link)
{
	mutex_lock(&cg_bpf_mutex);

	if (cg_bpf_ops_global.exit)
		cg_bpf_ops_global.exit();

	cg_bpf_disable();
	mutex_unlock(&cg_bpf_mutex);
}

static int cg_bpf_init(struct btf *btf)
{
	s32 type_id;

	type_id = btf_find_by_name_kind(btf, "cpufreq_policy", BTF_KIND_STRUCT);
	if (type_id < 0)
		return -EINVAL;

	cpufreq_policy_type = btf_type_by_id(btf, type_id);
	cpufreq_policy_type_id = type_id;
	return 0;
}

static int cg_bpf_update(void *kdata, void *old_kdata, struct bpf_link *link)
{
	return -EOPNOTSUPP;
}

static int cg_bpf_validate(void *kdata)
{
	return 0;
}

static struct bpf_struct_ops bpf_cg_bpf_ops = {
	.verifier_ops = &cg_bpf_verifier_ops,
	.reg = cg_bpf_reg,
	.unreg = cg_bpf_unreg,
	.check_member = cg_bpf_check_member,
	.init_member = cg_bpf_init_member,
	.init = cg_bpf_init,
	.update = cg_bpf_update,
	.validate = cg_bpf_validate,
	.name = "cg_bpf_ops",
	.owner = THIS_MODULE,
	.cfi_stubs = &bpf_cg_bpf_ops
};

static int __init cbpf_init(void)
{
	return register_bpf_struct_ops(&bpf_cg_bpf_ops, cg_bpf_ops);
}
device_initcall(cbpf_init);

static struct attribute *bpf_gov_attrs[] = {
	NULL
};
ATTRIBUTE_GROUPS(bpf_gov);

static inline struct bpf_policy *to_bpf_policy(struct policy_dbs_info *policy_dbs)
{
	return container_of(policy_dbs, struct bpf_policy, policy_dbs);
}

static unsigned int bpf_gov_update(struct cpufreq_policy *policy)
{
	struct bpf_policy *bpf;
	struct policy_dbs_info *policy_dbs;
	unsigned int update_sampling_rate = 0;
	struct dbs_governor *gov = dbs_governor_of(policy);

	/* Only need to update current policy freq */
	policy_dbs = container_of((void *)policy, struct policy_dbs_info, policy);

	bpf = to_bpf_policy(policy_dbs);

	if (static_branch_likely(&cg_bpf_gov_load) &&
	    (cg_bpf_ops_global.get_next_freq != get_next_freq_nop))
		bpf->next_freq = cg_bpf_ops_global.get_next_freq(policy);

	if (bpf->next_freq != policy->cur)
		__cpufreq_driver_target(policy, bpf->next_freq, CPUFREQ_RELATION_H);

	if (static_branch_likely(&cg_bpf_gov_load) &&
	    (cg_bpf_ops_global.get_sampling_rate != get_sampling_rate_nop))
		update_sampling_rate = cg_bpf_ops_global.get_sampling_rate(policy);

	/* If get_sampling_rate return 0, means we don't modify sampling_rate any more. */
	return update_sampling_rate == 0 ? gov->gdbs_data->sampling_rate : update_sampling_rate;
}

static struct policy_dbs_info *bpf_gov_alloc(void)
{
	struct bpf_policy *bpf;

	bpf = kzalloc(sizeof(*bpf), GFP_KERNEL);
	return bpf ? &bpf->policy_dbs : NULL;
}

static void bpf_gov_free(struct policy_dbs_info *policy_dbs)
{
	kfree(to_bpf_policy(policy_dbs));
}

static int bpf_gov_init(struct dbs_data *dbs_data)
{
	struct bpf_tuner *tuner;

	if (!dbs_data)
		return -EPERM;

	tuner = kzalloc(sizeof(*tuner), GFP_KERNEL);
	if (!tuner)
		return -ENOMEM;

	tuner->bpf_gov_stat = CPUFREQ_BPF_INIT;
	dbs_data->io_is_busy = 0;
	dbs_data->ignore_nice_load = 0;
	dbs_data->tuners = tuner;
	dbs_data->up_threshold = 30;
	bpf_global_tuner = tuner;
	static_branch_disable(&cg_bpf_gov_load);
	cg_bpf_ops_global.get_next_freq = get_next_freq_nop;
	cg_bpf_ops_global.get_sampling_rate = get_sampling_rate_nop;
	cg_bpf_ops_global.init = init_nop;
	cg_bpf_ops_global.exit = exit_nop;
	strscpy(cg_bpf_ops_global.name, DEFAULT_NAME, strlen(DEFAULT_NAME));
	return 0;
}

static void bpf_gov_exit(struct dbs_data *dbs_data)
{
	struct bpf_tuner *tuner;

	if (!dbs_data || !dbs_data->tuners)
		return;

	tuner = (struct bpf_tuner *)dbs_data->tuners;
	tuner->bpf_gov_stat = CPUFREQ_BPF_NONE;
	kfree(dbs_data->tuners);
	dbs_data->tuners = NULL;
	bpf_global_tuner = NULL;
}

static void bpf_gov_start(struct cpufreq_policy *policy)
{
	struct bpf_policy *bpf = to_bpf_policy(policy->governor_data);

	bpf->next_freq = cpufreq_driver_resolve_freq(policy, policy->cur);
}

static struct dbs_governor bpf_dbs_gov = {
	.gov = CPUFREQ_DBS_GOVERNOR_INITIALIZER("bpf"),
	.kobj_type = { .default_groups = bpf_gov_groups },
	.gov_dbs_update = bpf_gov_update,
	.alloc = bpf_gov_alloc,
	.free = bpf_gov_free,
	.init = bpf_gov_init,
	.exit = bpf_gov_exit,
	.start = bpf_gov_start,
};

#define CPU_FREQ_GOV_BPF	(bpf_dbs_gov.gov)

MODULE_AUTHOR("lvhuilin <kunkunyuz@qq.com>");
MODULE_LICENSE("GPL");

cpufreq_governor_init(CPU_FREQ_GOV_BPF);
cpufreq_governor_exit(CPU_FREQ_GOV_BPF);
