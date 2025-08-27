/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NUMA_KERNEL_REPLICATION_H
#define _LINUX_NUMA_KERNEL_REPLICATION_H

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
#include KABI_HIDE_INCLUDE(<linux/llist.h>)

#ifdef CONFIG_KERNEL_REPLICATION
#include KABI_HIDE_INCLUDE(<asm/numa_replication.h>)
#endif

#if defined(tmp_linux_value)
#define linux tmp_linux_value
#undef tmp_linux_value
#endif

typedef enum {
	NONE = 0,
	PMD_PROPAGATION = 1,
	PUD_PROPAGATION = 2,
	P4D_PROPAGATION = 3,
	PGD_PROPAGATION = 4
} propagation_level_t;

extern nodemask_t replica_nodes;

#define for_each_memory_node(nid) \
	for (nid = first_node(replica_nodes);		\
	     nid != MAX_NUMNODES;										\
	     nid = next_node(nid, replica_nodes))

#ifdef CONFIG_KERNEL_REPLICATION

#define this_node_pgd(mm) ((mm)->pgd_numa[numa_node_id()])
#define per_node_pgd(mm, nid) ((mm)->pgd_numa[nid])


static inline bool numa_addr_has_replica(const void *addr)
{
	return ((unsigned long)addr >= PAGE_TABLE_REPLICATION_LEFT) &&
		((unsigned long)addr <= PAGE_TABLE_REPLICATION_RIGHT);
}

static inline void clear_pgtable_list(struct page *head)
{
	struct llist_node *node;

	/* Replica list already have been destroyed */
	if (head->replica_list_node.next == NULL)
		return;

	for (node = llist_del_first(&head->replica_list_head);
	     node != &head->replica_list_node;
	     node = llist_del_first(&head->replica_list_head))
		node->next = NULL;
	head->replica_list_node.next = NULL;
}

static inline void build_pgd_chain(pgd_t **tables)
{
	int nid;
	int prev_node = -1;

	for_each_memory_node(nid) {
		virt_to_page(tables[nid])->replica_list_head.first = NULL;
		if (prev_node != -1) {
			llist_add(&virt_to_page(tables[nid])->replica_list_node, &virt_to_page(tables[prev_node])->replica_list_head);
		} else {
			/*
			 * This list is not supposed to be circular,
			 * but in order to simplify macro implementation,
			 * we do it anyway.
			 * God help us
			 */
			virt_to_page(tables[nid])->replica_list_node.next = &virt_to_page(tables[nid])->replica_list_node;
		}
		prev_node = nid;
	}
}

static inline bool numa_pgtable_replicated(void *table)
{
	return PageReplicated(virt_to_page(table));
}

void __init numa_replication_init(void);
void __init numa_replicate_kernel_text(void);
void numa_replicate_kernel_rodata(void);
void numa_replication_fini(void);

bool is_text_replicated(void);
propagation_level_t get_propagation_level(void);
void numa_setup_pgd(void);
void __init_or_module *numa_get_replica(void *vaddr, int nid);
int numa_get_memory_node(int nid);
void dump_mm_pgtables(struct mm_struct *mm,
		      unsigned long start, unsigned long end);

static inline unsigned long offset_in_table(void *ptr)
{
	return (unsigned long)ptr & (~PAGE_MASK);
}

static inline unsigned long get_table_ptr(struct page *table, unsigned long offset)
{
	return ((unsigned long)page_to_virt(table) + offset);
}

/**
 * @pos:	struct page* of current replica
 * @table:	current table entry to write (virtual address)
 * @head_table:	table entry form 0th node, will not be a part of this loop
 * @nid:	node id of current pgtable
 * @offset:	offset of current table entry in table page in bytes [0 .. 4088]
 * @start:	boolean value for tmp storage
 */
#define for_each_pgtable(pos, table, head_table, nid, offset, start)						\
	for (pos = llist_entry(&virt_to_page(head_table)->replica_list_node, typeof(*pos), replica_list_node),	\
	     start = true, nid = page_to_nid(pos),								\
	     offset = offset_in_table(head_table), table = (typeof(table))get_table_ptr(pos, offset);		\
	     pos != virt_to_page(head_table) || start;								\
	     pos = llist_entry((pos)->replica_list_node.next, typeof(*pos), replica_list_node),			\
	     table = (typeof(table))get_table_ptr(pos, offset),							\
	     nid = page_to_nid(pos), start = false)

/**
 * @pos:	struct page* of current replica
 * @table:	current table entry to write (virtual address)
 * @head_table:	table entry form 0th node, will not be a part of this loop
 * @offset:	offset of current table entry in table page in bytes [0 .. 4088]
 */
#define for_each_pgtable_replica(pos, table, head_table, offset)							\
	for (pos = llist_entry(virt_to_page(head_table)->replica_list_node.next, typeof(*pos), replica_list_node),	\
	     offset = offset_in_table(head_table), table = (typeof(table))get_table_ptr(pos, offset);			\
	     pos != virt_to_page(head_table);										\
	     pos = llist_entry((pos)->replica_list_node.next, typeof(*pos), replica_list_node),				\
	     table = (typeof(table))get_table_ptr(pos, offset))

/** Safe against removal pos
 * @pos:	struct page* of current replica
 * @n:		tmp storage
 * @table:	current table entry to write (virtual address)
 * @head_table:	table entry form 0th node, will not be a part of this loop
 * @offset:	offset of current table entry in table page in bytes [0 .. 4088]
 */
#define for_each_pgtable_replica_safe(pos, n, table, head_table, offset)						\
	for (pos = llist_entry(virt_to_page(head_table)->replica_list_node.next, typeof(*pos), replica_list_node),	\
	     n = llist_entry((pos)->replica_list_node.next, typeof(*pos), replica_list_node),				\
	     offset = offset_in_table(head_table), table = (typeof(table))get_table_ptr(pos, offset);			\
	     pos != virt_to_page(head_table);										\
	     pos = n, n = llist_entry((pos)->replica_list_node.next, typeof(*pos), replica_list_node),			\
	     table = (typeof(table))get_table_ptr(pos, offset))

static inline void pgd_populate_replicated(struct mm_struct *mm, pgd_t *pgdp, p4d_t *p4dp)
{
	pgd_populate(mm, pgdp, p4dp);

	if (!is_text_replicated())
		return;

	if (numa_pgtable_replicated(pgdp)) {
		unsigned long offset;
		struct page *curr;
		pgd_t *curr_pgd;

		for_each_pgtable_replica(curr, curr_pgd, pgdp, offset) {
			pgd_populate(mm, curr_pgd, p4dp);
		}
	}
}

static inline void p4d_populate_replicated(struct mm_struct *mm, p4d_t *p4dp, pud_t *pudp)
{
	p4d_populate(mm, p4dp, pudp);

	if (!is_text_replicated())
		return;

	if (numa_pgtable_replicated(p4dp)) {
		unsigned long offset;
		struct page *curr;
		p4d_t *curr_p4d;

		for_each_pgtable_replica(curr, curr_p4d, p4dp, offset) {
			p4d_populate(mm, curr_p4d, pudp);
		}
	}
}

static inline void pud_populate_replicated(struct mm_struct *mm, pud_t *pudp, pmd_t *pmdp)
{
	pud_populate(mm, pudp, pmdp);

	if (!is_text_replicated())
		return;

	if (numa_pgtable_replicated(pudp)) {
		unsigned long offset;
		struct page *curr;
		pud_t *curr_pud;

		for_each_pgtable_replica(curr, curr_pud, pudp, offset) {
			pud_populate(mm, curr_pud, pmdp);
		}
	}
}

static inline void pmd_populate_replicated(struct mm_struct *mm, pmd_t *pmdp, pgtable_t ptep)
{
	pmd_populate(mm, pmdp, ptep);

	if (!is_text_replicated())
		return;

	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			pmd_populate(mm, curr_pmd, ptep);
		}
	}
}

#else
#define this_node_pgd(mm) ((mm)->pgd)
#define per_node_pgd(mm, nid) ((mm)->pgd)

static inline bool numa_pgtable_replicated(void *table)
{
	return false;
}

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

#define pgd_populate_replicated pgd_populate
#define p4d_populate_replicated p4d_populate
#define pud_populate_replicated pud_populate
#define pmd_populate_replicated pmd_populate

#endif /*CONFIG_KERNEL_REPLICATION*/

#endif /*_LINUX_NUMA_REPLICATION_H*/
