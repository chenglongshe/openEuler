// SPDX-License-Identifier: GPL-2.0-only
/*
 * PGD allocation/freeing
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/numa_replication.h>

#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

static struct kmem_cache *pgd_cache __ro_after_init;

#ifdef CONFIG_KERNEL_REPLICATION
pgd_t *pgd_alloc(struct mm_struct *mm)
{
	int nid;
	gfp_t gfp = GFP_PGTABLE_USER | __GFP_THISNODE;
	pgd_t **pgd_numa = (pgd_t **)kmalloc(sizeof(pgd_t *) * MAX_NUMNODES, GFP_PGTABLE_KERNEL);

	if (!pgd_numa)
		goto pgd_numa_fail;

	mm->pgd_numa = pgd_numa;

	/*
	 * Kernel replication is not supproted in case of non-page size pgd,
	 * in general we can support it, but maybe later, due to we need to
	 * update page tables allocation significantly, so, let's panic here.
	 */
	BUG_ON(PGD_SIZE != PAGE_SIZE);
	for_each_memory_node(nid) {
		struct page *page;

		page = alloc_pages_node(nid, gfp, 0);
		if (!page)
			goto fail;

		per_node_pgd(mm, nid) = (pgd_t *)page_address(page);
	}

	for_each_online_node(nid)
		per_node_pgd(mm, nid) = per_node_pgd(mm, numa_get_memory_node(nid));

	mm->pgd = per_node_pgd(mm, numa_get_memory_node(0));

	return mm->pgd;

fail:
	pgd_free(mm, mm->pgd);

pgd_numa_fail:
	kfree(pgd_numa);

	return NULL;
}
#else
pgd_t *pgd_alloc(struct mm_struct *mm)
{
	gfp_t gfp = GFP_PGTABLE_USER;

	if (PGD_SIZE == PAGE_SIZE)
		return (pgd_t *)__get_free_page(gfp);
	else
		return kmem_cache_alloc(pgd_cache, gfp);
}
#endif /* CONFIG_KERNEL_REPLICATION */

#ifdef CONFIG_KERNEL_REPLICATION
void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	int nid;
	/*
	 * Kernel replication is not supproted in case of non-page size pgd,
	 * in general we can support it, but maybe later, due to we need to
	 * update page tables allocation significantly, so, let's panic here.
	 */
	BUG_ON(PGD_SIZE != PAGE_SIZE);
	for_each_memory_node(nid) {
		if (per_node_pgd(mm, nid) == NULL)
			break;
		free_page((unsigned long)per_node_pgd(mm, nid));
	}

	for_each_online_node(nid)
		per_node_pgd(mm, nid) = NULL;
	kfree(mm->pgd_numa);
}
#else
void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	if (PGD_SIZE == PAGE_SIZE)
		free_page((unsigned long)pgd);
	else
		kmem_cache_free(pgd_cache, pgd);
}
#endif /* CONFIG_KERNEL_REPLICATION */

void __init pgtable_cache_init(void)
{
	if (PGD_SIZE == PAGE_SIZE)
		return;

#ifdef CONFIG_ARM64_PA_BITS_52
	/*
	 * With 52-bit physical addresses, the architecture requires the
	 * top-level table to be aligned to at least 64 bytes.
	 */
	BUILD_BUG_ON(PGD_SIZE < 64);
#endif

	/*
	 * Naturally aligned pgds required by the architecture.
	 */
	pgd_cache = kmem_cache_create("pgd_cache", PGD_SIZE, PGD_SIZE,
				      SLAB_PANIC, NULL);
}
