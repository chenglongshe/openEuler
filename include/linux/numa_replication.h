/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NUMA_REPLICATION_H
#define _LINUX_NUMA_REPLICATION_H

#include <linux/kabi.h>

/*
 * Why? Because linux is defined to 1 for some reason,
 * and linux/mm.h converted to 1/mm.h. Perhaps compiler?
 * Do not ask me, I have no idea.
 */
#if defined(linux)
#define tmp_linux_value linux
#undef linux
#endif

#include KABI_HIDE_INCLUDE(<linux/mm_types.h>)
#include KABI_HIDE_INCLUDE(<linux/nodemask.h>)
#include KABI_HIDE_INCLUDE(<linux/module.h>)
#include KABI_HIDE_INCLUDE(<linux/mm.h>)

#ifdef CONFIG_KERNEL_REPLICATION
#include KABI_HIDE_INCLUDE(<asm/numa_replication.h>)
#endif

#if defined(tmp_linux_value)
#define linux tmp_linux_value
#undef tmp_linux_value
#endif


extern nodemask_t replica_nodes;

#define for_each_memory_node(nid)			\
	for (nid = first_node(replica_nodes);		\
	     nid != MAX_NUMNODES;			\
	     nid = next_node(nid, replica_nodes))

#ifdef CONFIG_KERNEL_REPLICATION
#define this_node_pgd(mm) ((mm)->pgd_numa[numa_node_id()])
#define per_node_pgd(mm, nid) ((mm)->pgd_numa[nid])

static inline bool numa_addr_has_replica(const void *addr)
{
	return ((unsigned long)addr >= PAGE_TABLE_REPLICATION_LEFT) &&
		((unsigned long)addr <= PAGE_TABLE_REPLICATION_RIGHT);
}

void __init numa_replication_init(void);
void __init numa_replicate_kernel_text(void);
void numa_replicate_kernel_rodata(void);
void numa_replication_fini(void);

bool is_text_replicated(void);
void numa_setup_pgd(void);
void __init_or_module *numa_get_replica(void *vaddr, int nid);
int numa_get_memory_node(int nid);
void dump_mm_pgtables(struct mm_struct *mm,
		      unsigned long start, unsigned long end);
#else
#define this_node_pgd(mm) ((mm)->pgd)
#define per_node_pgd(mm, nid) ((mm)->pgd)

static inline void numa_setup_pgd(void)
{
}

static inline void __init numa_replication_init(void)
{
}

static inline void __init numa_replicate_kernel_text(void)
{
}

static inline void numa_replicate_kernel_rodata(void)
{
}

static inline void numa_replication_fini(void)
{
}

static inline bool numa_addr_has_replica(const void *addr)
{
	return false;
}

static inline bool is_text_replicated(void)
{
	return false;
}

static inline void __init_or_module *numa_get_replica(void *vaddr, int nid)
{
	return lm_alias(vaddr);
}

static inline void dump_mm_pgtables(struct mm_struct *mm,
				    unsigned long start, unsigned long end)
{
}
#endif /*CONFIG_KERNEL_REPLICATION*/
#endif /*_LINUX_NUMA_REPLICATION_H*/
