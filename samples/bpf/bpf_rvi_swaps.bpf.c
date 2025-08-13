// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Huawei Technologies Co., Ltd */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>

struct task_struct *bpf_current_level1_reaper(void) __ksym;
void bpf_task_release(struct task_struct *p) __ksym;
struct mem_cgroup *bpf_mem_cgroup_from_task(struct task_struct *p) __ksym;
void bpf_si_memswinfo(struct bpf_sysinfo *si) __ksym;
unsigned long bpf_atomic_long_read(const atomic_long_t *v) __ksym;
unsigned long bpf_page_counter_read(struct page_counter *pc) __ksym;
void bpf_rcu_read_lock(void) __ksym;
void bpf_rcu_read_unlock(void) __ksym;
void cgroup_rstat_flush_atomic(struct cgroup *cgrp) __ksym;

char _license[] SEC("license") = "GPL";

/* Reference: https://docs.ebpf.io/ebpf-library/libbpf/ebpf/__ksym/ */
extern void cgrp_dfl_root __ksym;
/* Reference: cgroup_on_dfl() */
static inline bool cgroup_on_dfl(const struct cgroup *cgrp)
{
	return cgrp->root == &cgrp_dfl_root;
}

#define RET_OK    0
#define RET_FAIL  1
#define RET_SKIP -1

SEC("iter/generic_single")
s64 dump_swaps(struct bpf_iter__generic_single *ctx)
{
	struct seq_file *m = ctx->meta->seq;
	struct task_struct *reaper;
	struct mem_cgroup *memcg;
	struct bpf_sysinfo si = {};
	u64 limit, usage, swapusage = 0, swaptotal = 0;
	u64 kb_per_page;

	reaper = bpf_current_level1_reaper();
	if (!reaper)
		return RET_FAIL;
	bpf_rcu_read_lock();
	memcg = bpf_mem_cgroup_from_task(reaper);
	if (!memcg) {
		bpf_rcu_read_unlock();
		bpf_task_release(reaper);
		return RET_FAIL;
	}

	bpf_si_memswinfo(&si);
	cgroup_rstat_flush_atomic(memcg->css.cgroup);
	limit = memcg->memory.max;
	/*
	 * si.totalram: size in pages
	 * si.mem_unit: PAGE_SIZE
	 * memcg->memory.{max,...}: counting in pages
	 */
	if (limit == 0 || limit > si.totalram)
		limit = si.totalram;
	/*
	 * Reference: page_counter_read().
	 * memcg->memory.usage is atomic, should be read by (bpf_)atomic_long_read.
	 * Consider using mem_cgroup_usage(memcg, true/false)?
	 */
	usage = bpf_page_counter_read(&memcg->memory);
	if (usage == 0 || usage > limit)
		usage = limit;

	if (cgroup_on_dfl(memcg->css.cgroup)) { // if memcg is on V2 hierarchy
		swaptotal = memcg->swap.max;
		swapusage = bpf_page_counter_read(&memcg->swap);
	} else {
		u64 memsw_limit = memcg->memsw.max; // memsw = mem + swap
		u64 memsw_usage = bpf_page_counter_read(&memcg->memsw);

		/*
		 * Reasonably, memsw.max should >= memory.max, as memsw = mem + swap in V1.
		 * But it's not necessarily the case, as users may configure them as they wish.
		 */
		if (memsw_limit > limit)
			swaptotal = memsw_limit - limit;
		/* Similar treatment for {memsw,memory}.usage */
		if (swaptotal && memsw_usage > usage)
			swapusage = memsw_usage - usage;
	}
	if (swaptotal > si.totalswap)
		swaptotal = si.totalswap;
	if (swapusage > si.totalswap - si.freeswap)
		swapusage = si.totalswap - si.freeswap;

	kb_per_page = si.mem_unit >> 10;
	/* Reference: swap_show(). Aligned with LXCFS. */
	BPF_SEQ_PRINTF(m, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
	if (swaptotal > 0)
		BPF_SEQ_PRINTF(m, "none%*svirtual\t\t%llu\t%llu\t0\n",
				  36, " ", swaptotal * kb_per_page,
				  swapusage * kb_per_page); // in KB

	bpf_rcu_read_unlock();
	bpf_task_release(reaper);

	return RET_OK;
}
