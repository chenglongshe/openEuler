/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_NUMA_REPLICATION_H
#define __ASM_NUMA_REPLICATION_H

#ifdef CONFIG_KERNEL_REPLICATION
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/memory.h>
#include <asm/mmu_context.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

#define PAGE_TABLE_REPLICATION_LEFT  ((max((u64)_end - SZ_2G, (u64)MODULES_VADDR)) & PGDIR_MASK)
#define PAGE_TABLE_REPLICATION_RIGHT ((((u64)_end + SZ_2G) & PGDIR_MASK) + PGDIR_SIZE - 1)

static inline pgd_t *numa_replicate_pgt_pgd(int nid)
{
	pgd_t *new_pgd;
	struct page *pgd_page;

	pgd_page = alloc_pages_node(nid, GFP_PGTABLE_KERNEL, 2);
	BUG_ON(pgd_page == NULL);

	new_pgd = (pgd_t *)page_address(pgd_page);
	new_pgd += (PTRS_PER_PGD * 2); //Extra pages for KPTI

	copy_page((void *)new_pgd, (void *)swapper_pg_dir);

	return new_pgd;
}


void cpu_replace_ttbr1(pgd_t *pgdp);
static inline void numa_load_replicated_pgd(pgd_t *pgd)
{
	cpu_replace_ttbr1(pgd);
	local_flush_tlb_all();
}

static inline ssize_t numa_cpu_dump(struct seq_file *m)
{
	seq_printf(m, "NODE: #%02d, CPU: #%04d, ttbr1_el1: 0x%p, COMM: %s\n",
		numa_node_id(),
		smp_processor_id(),
		(void *)read_sysreg(ttbr1_el1),
		current->group_leader->comm);
	return 0;
}

static inline void numa_sync_text_replicas(unsigned long start, unsigned long end)
{
	__flush_icache_range(start, end);
}
#endif /* CONFIG_KERNEL_REPLICATION */
#endif /* __ASM_NUMA_REPLICATION_H */
