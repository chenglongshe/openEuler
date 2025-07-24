// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2025 Huawei Technologies Co., Ltd */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>

#define READ_ONCE(x)		(*(volatile typeof(x) *)&(x))

void bpf_rcu_read_lock(void) __ksym;
void bpf_rcu_read_unlock(void) __ksym;
void bpf_task_release(struct task_struct *p) __ksym;
struct task_struct *bpf_current_level1_reaper(void) __ksym;
struct mem_cgroup *bpf_mem_cgroup_from_task(struct task_struct *p) __ksym;
void cgroup_rstat_flush_atomic(struct cgroup *cgrp) __ksym;
void bpf_si_memswinfo(struct bpf_sysinfo *si) __ksym;
unsigned long bpf_page_counter_read(struct page_counter *pc) __ksym;
unsigned long bpf_mem_committed(void) __ksym;
unsigned long bpf_mem_commit_limit(void) __ksym;
unsigned long bpf_mem_vmalloc_total(void) __ksym;
unsigned long bpf_mem_vmalloc_used(void) __ksym;
unsigned long bpf_mem_percpu(void) __ksym;
unsigned long bpf_mem_failure(void) __ksym;
unsigned long bpf_mem_totalcma(void) __ksym;
unsigned long bpf_mem_freecma(void) __ksym;
int bpf_hugetlb_report_meminfo(struct bpf_mem_hugepage *hugepage_info) __ksym;
void bpf_mem_direct_map(unsigned long *p) __ksym;
unsigned long bpf_mem_file_hugepage(void) __ksym;
unsigned long bpf_mem_file_pmdmapped(void) __ksym;
unsigned long bpf_mem_kreclaimable(void) __ksym;

extern bool CONFIG_SWAP __kconfig __weak;
extern bool CONFIG_MEMCG_KMEM __kconfig __weak;
extern bool CONFIG_ZSWAP __kconfig __weak;
extern bool CONFIG_MEMORY_FAILURE __kconfig __weak;
extern bool CONFIG_TRANSPARENT_HUGEPAGE __kconfig __weak;
extern bool CONFIG_CMA __kconfig __weak;
extern bool CONFIG_X86 __kconfig __weak;
extern bool CONFIG_X86_64 __kconfig __weak;
extern bool CONFIG_X86_PAE __kconfig __weak;

/* Axiom */
#define PAGE_SHIFT	12
#define PMD_SHIFT	21
/* include/linux/huge_mm.h */
#define HPAGE_PMD_SHIFT	PMD_SHIFT
#define HPAGE_PMD_ORDER	(HPAGE_PMD_SHIFT-PAGE_SHIFT)
#define HPAGE_PMD_NR	(1<<HPAGE_PMD_ORDER)

char _license[] SEC("license") = "GPL";

#define KB(pg)		((pg) * 4)
#define SI_PG(v, unit)	(v * unit / 4096)

static inline unsigned long
memcg_page_state(struct mem_cgroup *memcg, int idx)
{
	long x = READ_ONCE(memcg->vmstats->state[idx]);

	return x < 0 ? 0 : x;
}

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
s64 dump_meminfo(struct bpf_iter__generic_single *ctx)
{
	struct seq_file *m = ctx->meta->seq;
	struct task_struct *reaper;
	struct mem_cgroup *memcg;
	struct bpf_sysinfo si = {};
	struct bpf_mem_hugepage hg_info = {};
	u64 usage, limit;
	unsigned long sreclaimable, sunreclaim;
	unsigned long memswlimit, memswusage;
	unsigned long cached, active_anon, inactive_anon;
	unsigned long active_file, inactive_file, unevicatable;
	unsigned long swapusage, swapfree, swaptotal;
	unsigned long committed;

	bpf_hugetlb_report_meminfo(&hg_info);

	committed = bpf_mem_committed();
	bpf_si_memswinfo(&si);

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
	cgroup_rstat_flush_atomic(memcg->css.cgroup);
	limit = memcg->memory.max;
	if (limit == 0 || limit > si.totalram)
		limit = si.totalram;
	/*
	 * Reference: page_counter_read().
	 * memcg->memory.usage is atomic, should be read by (bpf_)atomic_long_read.
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

	swapfree = swaptotal - swapusage;
	if (swapfree > si.freeswap)
		swapfree = si.freeswap;

	cached = memcg_page_state(memcg, NR_FILE_PAGES);
	active_anon = memcg_page_state(memcg, NR_ACTIVE_ANON);
	inactive_anon = memcg_page_state(memcg, NR_INACTIVE_ANON);
	active_file = memcg_page_state(memcg, NR_ACTIVE_FILE);
	inactive_file = memcg_page_state(memcg, NR_INACTIVE_FILE);
	unevicatable = memcg_page_state(memcg, NR_UNEVICTABLE);
	sreclaimable = memcg_page_state(memcg, NR_SLAB_RECLAIMABLE_B);
	sunreclaim = memcg_page_state(memcg, NR_SLAB_UNRECLAIMABLE_B);

	BPF_SEQ_PRINTF(m, "MemTotal:           %8llu kB\n", KB(limit));
	BPF_SEQ_PRINTF(m, "MemFree:            %8llu kB\n", KB(limit - usage));
	BPF_SEQ_PRINTF(m, "MemAvailable:       %8llu kB\n", KB(limit - usage + cached));
	BPF_SEQ_PRINTF(m, "Buffers:            %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "Cached:             %8llu kB\n", KB(cached));

	if (CONFIG_SWAP)
		BPF_SEQ_PRINTF(m, "SwapCached:         %8llu kB\n", KB(memcg_page_state(memcg, NR_SWAPCACHE)));

	BPF_SEQ_PRINTF(m, "Active:             %8llu kB\n", KB(active_anon + active_file));
	BPF_SEQ_PRINTF(m, "Inactive:           %8llu kB\n", KB(inactive_anon + inactive_file));
	BPF_SEQ_PRINTF(m, "Active(anon):       %8llu kB\n", KB(active_anon));
	BPF_SEQ_PRINTF(m, "Inactive(anon):     %8llu kB\n", KB(inactive_anon));
	BPF_SEQ_PRINTF(m, "Active(file):       %8llu kB\n", KB(active_file));
	BPF_SEQ_PRINTF(m, "Inactive(file):     %8llu kB\n", KB(inactive_file));
	BPF_SEQ_PRINTF(m, "Unevictable:        %8llu kB\n", KB(unevicatable));
	BPF_SEQ_PRINTF(m, "Mlocked:            %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "SwapTotal:          %8llu kB\n", KB(swaptotal));
	BPF_SEQ_PRINTF(m, "SwapFree:           %8llu kB\n", KB(swapfree));

	if (CONFIG_MEMCG_KMEM && CONFIG_ZSWAP) {
		BPF_SEQ_PRINTF(m, "Zswap:              %8llu kB\n", memcg_page_state(memcg, MEMCG_ZSWAP_B));
		BPF_SEQ_PRINTF(m, "Zswapped:           %8llu kB\n", memcg_page_state(memcg, MEMCG_ZSWAPPED));
	}

	BPF_SEQ_PRINTF(m, "Dirty:              %8llu kB\n", KB(memcg_page_state(memcg, NR_FILE_DIRTY)));
	BPF_SEQ_PRINTF(m, "Writeback:          %8llu kB\n", KB(memcg_page_state(memcg, NR_WRITEBACK)));
	BPF_SEQ_PRINTF(m, "AnonPages:          %8llu kB\n", KB(memcg_page_state(memcg, NR_ANON_MAPPED)));
	BPF_SEQ_PRINTF(m, "Mapped:             %8llu kB\n", KB(memcg_page_state(memcg, NR_FILE_MAPPED)));
	BPF_SEQ_PRINTF(m, "Shmem:              %8llu kB\n", KB(memcg_page_state(memcg, NR_SHMEM)));
	BPF_SEQ_PRINTF(m, "KReclaimable:       %8llu kB\n", KB(bpf_mem_kreclaimable()));
	BPF_SEQ_PRINTF(m, "Slab:               %8llu kB\n", KB(sreclaimable + sunreclaim));
	BPF_SEQ_PRINTF(m, "SReclaimable:       %8llu kB\n", KB(sreclaimable));
	BPF_SEQ_PRINTF(m, "SUnreclaim:         %8llu kB\n", KB(sunreclaim));
	BPF_SEQ_PRINTF(m, "KernelStack:        %8llu kB\n", memcg_page_state(memcg, NR_KERNEL_STACK_KB));
	BPF_SEQ_PRINTF(m, "PageTables:         %8llu kB\n", KB(memcg_page_state(memcg, NR_PAGETABLE)));
	BPF_SEQ_PRINTF(m, "SecPageTables       %8llu kB\n", KB(memcg_page_state(memcg, NR_SECONDARY_PAGETABLE)));
	BPF_SEQ_PRINTF(m, "NFS_Unstable:       %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "Bounce:             %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "WritebackTmp:       %8llu kB\n", KB(memcg_page_state(memcg, NR_WRITEBACK_TEMP)));
	BPF_SEQ_PRINTF(m, "CommitLimit:        %8llu kB\n", KB(bpf_mem_commit_limit()));
	BPF_SEQ_PRINTF(m, "Committed_AS:       %8llu kB\n", KB(committed));
	BPF_SEQ_PRINTF(m, "VmallocTotal:       %8llu kB\n", bpf_mem_vmalloc_total());
	BPF_SEQ_PRINTF(m, "VmallocUsed:        %8llu kB\n", KB(bpf_mem_vmalloc_used()));
	BPF_SEQ_PRINTF(m, "VmallocChunk:       %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "Percpu:             %8llu kB\n", KB(bpf_mem_percpu()));

	if (CONFIG_MEMORY_FAILURE)
		BPF_SEQ_PRINTF(m, "HardwareCorrupted:  %8llu kB\n", bpf_mem_failure());

	if (CONFIG_TRANSPARENT_HUGEPAGE) {
		BPF_SEQ_PRINTF(m, "AnonHugePages:      %8llu kB\n", KB(memcg_page_state(memcg, NR_ANON_THPS) *
								       HPAGE_PMD_NR));
		BPF_SEQ_PRINTF(m, "ShmemHugePages:     %8llu kB\n", KB(memcg_page_state(memcg, NR_SHMEM_THPS) *
								       HPAGE_PMD_NR));
		BPF_SEQ_PRINTF(m, "ShmemPmdMapped:     %8llu kB\n", KB(memcg_page_state(memcg, NR_SHMEM_PMDMAPPED) *
								       HPAGE_PMD_NR));
		BPF_SEQ_PRINTF(m, "FileHugePages:      %8llu kB\n", KB(bpf_mem_file_hugepage()));
		BPF_SEQ_PRINTF(m, "FilePmdMapped:      %8llu kB\n", KB(bpf_mem_file_pmdmapped()));
	}
	if (CONFIG_CMA) {
		BPF_SEQ_PRINTF(m, "CmaTotal:           %8llu kB\n", KB(bpf_mem_totalcma()));
		BPF_SEQ_PRINTF(m, "CmaFree:            %8llu kB\n", KB(bpf_mem_freecma()));
	}
	BPF_SEQ_PRINTF(m, "Unaccepted:         %8llu kB\n", KB(0));
	BPF_SEQ_PRINTF(m, "HugePages_Total:    %8llu\n", hg_info.total);
	BPF_SEQ_PRINTF(m, "HugePages_Free:     %8llu\n", hg_info.free);
	BPF_SEQ_PRINTF(m, "HugePages_Rsvd:     %8llu\n", hg_info.rsvd);
	BPF_SEQ_PRINTF(m, "HugePages_Surp:     %8llu\n", hg_info.surp);
	BPF_SEQ_PRINTF(m, "Hugepagesize:       %8llu kB\n", hg_info.size);
	BPF_SEQ_PRINTF(m, "Hugetlb:            %8llu kB\n", hg_info.hugetlb);

	if (CONFIG_X86) {
		unsigned long direct_map_info[3] = {};

		bpf_mem_direct_map(direct_map_info);
		BPF_SEQ_PRINTF(m, "DirectMap4k:        %8llu kB\n", direct_map_info[0]);
		if (CONFIG_X86_64 || CONFIG_X86_PAE)
			BPF_SEQ_PRINTF(m, "DirectMap2M:        %8llu kB\n", direct_map_info[1]);
		else
			BPF_SEQ_PRINTF(m, "DirectMap4M:        %8llu kB\n", direct_map_info[1]);
		BPF_SEQ_PRINTF(m, "DirectMap1G:        %8llu kB\n", direct_map_info[2]);
	}

	bpf_rcu_read_unlock();
	bpf_task_release(reaper);

	return RET_OK;
}
