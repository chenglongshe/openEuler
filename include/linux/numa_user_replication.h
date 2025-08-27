/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_NUMA_USER_REPLICATION_H
#define _LINUX_NUMA_USER_REPLICATION_H

#include <linux/kabi.h>
#include <linux/numa_kernel_replication.h>

/* Same as in numa_kernel_replication.h */
#if defined(linux)
#define tmp_linux_value linux
#undef linux
#endif

#include KABI_HIDE_INCLUDE(<linux/mmu_notifier.h>)
#include KABI_HIDE_INCLUDE(<linux/memcontrol.h>)
#include KABI_HIDE_INCLUDE(<trace/events/kmem.h>)

#if defined(tmp_linux_value)
#define linux tmp_linux_value
#undef tmp_linux_value
#endif

/*
 * pgwlk_for_each_replica_page - Iterates over pgwlk->replica_pages.
 *
 * @walk: the owner of replcia_pages (a.k.a pgtable_walker)
 * @page: assigns each page from replica_pages to this variable.
 * @nid: assigns each node ID of _memory_ nodes to this variable.
 *
 * Note that page can be NULL if replica_pages has not been assigned
 * yet. Caller must check each page for NULL if needed.
 */
#define pgwlk_for_each_replica_page(zp, page, nid)								\
	for (nid = first_node(node_states[N_MEMORY]), (page) = (zp)->replica_pages[first_memory_node];		\
	     nid != MAX_NUMNODES;										\
	     nid = next_node(nid, node_states[N_MEMORY]), (page) = (zp)->replica_pages[nid])

#ifdef CONFIG_USER_REPLICATION_DEBUG
#define UREPLICA_DEBUG(code) code;
#else
#define UREPLICA_DEBUG(code) {}
#endif

#ifdef CONFIG_USER_REPLICATION

extern unsigned long replica_count;

struct pgtable_private {
	pte_t *pte_numa[MAX_NUMNODES];
	struct page *replica_pages[MAX_NUMNODES];
	bool pte_replicated;
};

static inline void pgtable_pte_step(struct pgtable_private *zp, int nr)
{
	int nid;

	if (zp->pte_replicated)
		for_each_memory_node(nid)
			zp->pte_numa[nid] += nr;
}

static inline void pgtable_update_pte(struct pgtable_private *zp, pte_t *pte)
{

	zp->pte_numa[page_to_nid(virt_to_page(pte))] = pte;
	zp->pte_replicated = false;
	if (numa_pgtable_replicated(pte)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;
		int nid;

		zp->pte_replicated = true;
		for_each_pgtable_replica(curr, curr_pte, pte, offset) {
			nid = page_to_nid(curr);
			zp->pte_numa[nid] = curr_pte;
		}
	}
}

static inline pmd_t *get_master_pmd(pmd_t *pmd)
{
	return (pmd_t *)get_table_ptr(virt_to_page(pmd)->master_table, offset_in_table(pmd));
}

static inline pud_t *get_master_pud(pud_t *pud)
{
	return (pud_t *)get_table_ptr(virt_to_page(pud)->master_table, offset_in_table(pud));
}

static inline void set_master_page_for_puds(int allocated_node, pud_t **new)
{
	int nid;
	struct page *master_table;
	struct page *curr_table;

	if (allocated_node == NUMA_NO_NODE)
		allocated_node = first_memory_node;

	master_table = virt_to_page(new[allocated_node]);

	for_each_memory_node(nid) {
		curr_table = virt_to_page(new[nid]);
		curr_table->master_table = master_table;
	}
}

static inline void set_master_page_for_pmds(int allocated_node, pmd_t **new)
{
	int nid;
	struct page *master_table;
	struct page *curr_table;

	if (allocated_node == NUMA_NO_NODE)
		allocated_node = first_memory_node;

	master_table = virt_to_page(new[allocated_node]);

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		curr_table = virt_to_page(new[nid]);
		curr_table->master_table = master_table;
	}
}

static inline void set_master_page_for_ptes(int allocated_node, struct page **new)
{
	int nid;
	struct page *master_table;
	struct page *curr_table;

	if (allocated_node == NUMA_NO_NODE)
		allocated_node = first_memory_node;

	master_table = new[allocated_node];

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		curr_table = new[nid];
		curr_table->master_table = master_table;
	}
}

void numa_mm_apply_replication(struct mm_struct *mm);
int numa_clone_pte(struct vm_area_struct *vma, unsigned long start, unsigned long end);
int numa_remove_replicas(struct vm_area_struct *vma, unsigned long start, unsigned long end, bool alloc_new_page);
int phys_duplicate(struct vm_area_struct *vma, unsigned long start, size_t len);
int phys_deduplicate(struct vm_area_struct *vma, unsigned long start, size_t len, bool alloc_new_page);
unsigned long phys_duplicate_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end);
int phys_duplicate_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end);

static inline int numa_is_vma_replicant(struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_REPLICA_INIT)
		return 1;

	return 0;
}

static inline bool vma_has_replicas(struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_REPLICA_COMMIT;
}

static inline bool vma_might_be_replicated(struct vm_area_struct *vma)
{
	return (vma->vm_file || vma_is_anonymous(vma)) && ((vma->vm_flags & VM_REPLICA_INIT) && vma_is_accessible(vma) && !(vma->vm_flags & (VM_WRITE | VM_SHARED)));
}

static inline bool __vm_flags_replica_candidate(struct vm_area_struct *vma, unsigned long flags)
{
	return (vma->vm_file || vma_is_anonymous(vma)) && ((flags & VM_ACCESS_FLAGS) && !(flags & (VM_WRITE | VM_SHARED)));
}

static inline bool vma_replica_candidate(struct vm_area_struct *vma)
{
	return __vm_flags_replica_candidate(vma, vma->vm_flags);
}

/*
 * Arch specific implementation
 * TODO: remove these functions from generic header
 */
#ifdef CONFIG_ARM64
static inline void set_pte_replicated(pte_t *ptep, pte_t pte)
{
	WRITE_ONCE(*ptep, pte);

	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			WRITE_ONCE(*curr_pte, pte);
		}
	}

	if (pte_valid_not_user(pte)) {
		dsb(ishst);
		isb();
	}
}

static inline void pte_clear_replicated(struct mm_struct *mm, unsigned long addr,
			     pte_t *ptep)
{
	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	set_pte_replicated(ptep, __pte(0));
	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_pte_clear(end - start);)
}

static inline void set_pte_at_replicated(struct mm_struct *mm,
					  unsigned long address,
					  pte_t *ptep, pte_t pte)
{
	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());

	if (pte_present(pte) && pte_user_exec(pte) && !pte_special(pte))
		__sync_icache_dcache(pte);

	if (system_supports_mte() &&
	    pte_present(pte) && pte_tagged(pte) && !pte_special(pte))
		mte_sync_tags(ptep, pte);

	__check_racy_pte_update(mm, ptep, pte);

	set_pte_replicated(ptep, pte);

	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_set_pte_at(end - start);)

}

static inline pte_t ptep_get_and_clear_replicated(struct mm_struct *mm,
						  unsigned long address,
						  pte_t *pte)
{
	pte_t pteval;

	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	pteval = ptep_get_and_clear(mm, address, pte);
	if (numa_pgtable_replicated(pte)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, pte, offset) {
			pteval = ptep_get_and_clear(mm, address, curr_pte);
		}
	}
	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_pte_get_and_clear(end - start);)
	return pteval;
}

static inline pte_t ptep_clear_flush_replicated(struct vm_area_struct *vma,
						 unsigned long address,
						 pte_t *ptep)
{
	struct mm_struct *mm = (vma)->vm_mm;
	pte_t pte;

	pte = ptep_get_and_clear_replicated(mm, address, ptep);
	if (pte_accessible(mm, pte))
		flush_tlb_page(vma, address);
	return pte;
}

static inline int ptep_test_and_clear_young_replicated(struct vm_area_struct *vma,
						     unsigned long address,
						     pte_t *pte)
{
	int ret;

	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	ret = ptep_test_and_clear_young(vma, address, pte);
	if (numa_pgtable_replicated(pte)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, pte, offset) {
			ret |= ptep_test_and_clear_young(vma, address, curr_pte);
		}
	}
	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_ptep_test_and_clear_young(end - start);)
	return ret;
}

static inline int ptep_clear_flush_young_replicated(struct vm_area_struct *vma,
							    unsigned long address,
							    pte_t *ptep)
{
	int young = ptep_test_and_clear_young_replicated(vma, address, ptep);

	if (young) {
		/*
		 * We can elide the trailing DSB here since the worst that can
		 * happen is that a CPU continues to use the young entry in its
		 * TLB and we mistakenly reclaim the associated page. The
		 * window for such an event is bounded by the next
		 * context-switch, which provides a DSB to complete the TLB
		 * invalidation.
		 */
		flush_tlb_page_nosync(vma, address);
	}

	return young;
}

#ifdef CONFIG_MMU_NOTIFIER
#define set_pte_at_notify_replicated(__mm, __address, __ptep, __pte)	\
({									\
	struct mm_struct *___mm = __mm;					\
	unsigned long ___address = __address;				\
	pte_t ___pte = __pte;						\
									\
	mmu_notifier_change_pte(___mm, ___address, ___pte);		\
	set_pte_at_replicated(___mm, ___address, __ptep, ___pte);	\
})

#define ptep_clear_flush_young_notify_replicated(__vma, __address, __ptep)	\
({										\
	int __young;								\
	struct vm_area_struct *___vma = __vma;					\
	unsigned long ___address = __address;					\
	__young = ptep_clear_flush_young_replicated(___vma, ___address, __ptep);\
	__young |= mmu_notifier_clear_flush_young(___vma->vm_mm,		\
						  ___address,			\
						  ___address +			\
							PAGE_SIZE);		\
	__young;								\
})

#define	ptep_clear_flush_notify_replicated(__vma, __address, __ptep)	\
({									\
	unsigned long ___addr = __address & PAGE_MASK;			\
	struct mm_struct *___mm = (__vma)->vm_mm;			\
	pte_t ___pte;							\
									\
	___pte = ptep_clear_flush_replicated(__vma, __address, __ptep);	\
	mmu_notifier_invalidate_range(___mm, ___addr,			\
					___addr + PAGE_SIZE);		\
									\
	___pte;								\
})

#define ptep_clear_young_notify_replicated(__vma, __address, __ptep)			\
({											\
	int __young;									\
	struct vm_area_struct *___vma = __vma;						\
	unsigned long ___address = __address;						\
	__young = ptep_test_and_clear_young_replicated(___vma, ___address, __ptep);	\
	__young |= mmu_notifier_clear_young(___vma->vm_mm, ___address,			\
					    ___address + PAGE_SIZE);			\
	__young;									\
})

#else
#define set_pte_at_notify_replicated set_pte_at_replicated
#define ptep_clear_flush_young_notify_replicated ptep_clear_flush_young_replicated
#define ptep_clear_flush_notify_replicated ptep_clear_flush_replicated
#define ptep_clear_young_notify_replicated ptep_test_and_clear_young_replicated
#endif

static inline pte_t ptep_get_and_clear_full_replicated(struct mm_struct *mm,
						       unsigned long address,
						       pte_t *pte, int full)
{
	return ptep_get_and_clear_replicated(mm, address, pte);
}

static inline void pte_clear_not_present_full_replicated(struct mm_struct *mm,
							 unsigned long address,
							 pte_t *ptep,
							 int full)
{
	pte_clear_replicated(mm, address, ptep);
}

static inline void ptep_set_wrprotect_replicated(struct mm_struct *mm,
						 unsigned long addr,
						 pte_t *pte)
{
	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	ptep_set_wrprotect(mm, addr, pte);
	if (numa_pgtable_replicated(pte)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, pte, offset) {
			ptep_set_wrprotect(mm, addr, curr_pte);
		}
	}
	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_ptep_set_wrprotect(end - start);)
}

static inline int ptep_set_access_flags_nosync(struct vm_area_struct *vma,
					       unsigned long address, pte_t *ptep,
					       pte_t entry, int dirty)
{
	pteval_t old_pteval, pteval;
	pte_t pte = READ_ONCE(*ptep);

	if (pte_same(pte, entry))
		return 0;

	/* only preserve the access flags and write permission */
	pte_val(entry) &= PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;

	/*
	 * Setting the flags must be done atomically to avoid racing with the
	 * hardware update of the access/dirty state. The PTE_RDONLY bit must
	 * be set to the most permissive (lowest value) of *ptep and entry
	 * (calculated as: a & b == ~(~a | ~b)).
	 */
	pte_val(entry) ^= PTE_RDONLY;
	pteval = pte_val(pte);
	do {
		old_pteval = pteval;
		pteval ^= PTE_RDONLY;
		pteval |= pte_val(entry);
		pteval ^= PTE_RDONLY;
		pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
	} while (pteval != old_pteval);

	/* Invalidate a stale read-only entry */

	return 1;
}

static inline int ptep_set_access_flags_replicated(struct vm_area_struct *vma,
						   unsigned long address, pte_t *ptep,
						   pte_t entry, int dirty)
{
	int res;

	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	res = ptep_set_access_flags_nosync(vma, address, ptep, entry, dirty);

	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;
		unsigned long flags = pte_val(entry) & (~PTE_ADDR_MASK);

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			pte_t new_entry;

			WARN_ON(!pte_present(*curr_pte));

			new_entry = __pte((pte_val(*curr_pte) & PTE_ADDR_MASK) | flags);
			res |= ptep_set_access_flags_nosync(vma, address, curr_pte, new_entry, dirty);
		}
	}

	if (dirty)
		flush_tlb_page(vma, address);

	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_ptep_set_access_flags(end - start);)
	return res;
}

static inline pte_t ptep_modify_prot_start_replicated(struct vm_area_struct *vma,
						      unsigned long addr, pte_t *ptep)
{
	pte_t res;

	UREPLICA_DEBUG(ktime_t start;
		ktime_t end;)
	UREPLICA_DEBUG(start = ktime_get());
	res = ptep_modify_prot_start(vma, addr, ptep);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			ptep_modify_prot_start(vma, addr, curr_pte);
		}
	}
	UREPLICA_DEBUG(end = ktime_get();
		       trace_mm_ureplica_cost_ptep_modify_prot_start(end - start);)
	return res;
}

static inline void ptep_modify_prot_commit_replicated(struct vm_area_struct *vma,
						      unsigned long addr,
						      pte_t *ptep, pte_t old_pte, pte_t pte)
{
	set_pte_at_replicated(vma->vm_mm, addr, ptep, pte);
}

static inline void set_huge_pte_at_replicated(struct mm_struct *mm, unsigned long addr,
					      pte_t *ptep, pte_t pte)
{
	set_huge_pte_at(mm, addr, ptep, pte);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			set_huge_pte_at(mm, addr, curr_pte, pte);
		}
	}
}

static inline int huge_ptep_set_access_flags_replicated(struct vm_area_struct *vma,
							unsigned long addr, pte_t *ptep,
							pte_t pte, int dirty)
{
	int ret = huge_ptep_set_access_flags(vma, addr, ptep, pte, dirty);

	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			ret |= huge_ptep_set_access_flags(vma, addr, curr_pte, pte, dirty);
		}
	}
	return ret;
}

static inline void huge_ptep_clear_flush_replicated(struct vm_area_struct *vma,
						    unsigned long addr, pte_t *ptep)
{
	huge_ptep_clear_flush(vma, addr, ptep);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			huge_ptep_clear_flush(vma, addr, curr_pte);
		}
	}
}

static inline void set_huge_swap_pte_at_replicated(struct mm_struct *mm, unsigned long addr,
						   pte_t *ptep, pte_t pte, unsigned long sz)
{
	set_huge_swap_pte_at(mm, addr, ptep, pte, sz);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			set_huge_swap_pte_at(mm, addr, curr_pte, pte, sz);
		}
	}
}

static inline pte_t huge_ptep_modify_prot_start_replicated(struct vm_area_struct *vma,
							   unsigned long addr, pte_t *ptep)
{
	pte_t res = huge_ptep_modify_prot_start(vma, addr, ptep);

	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			res = huge_ptep_modify_prot_start(vma, addr, curr_pte);
		}
	}
	return res;
}

static inline void huge_ptep_modify_prot_commit_replicated(struct vm_area_struct *vma,
							   unsigned long addr, pte_t *ptep,
							   pte_t old_pte, pte_t pte)
{
	huge_ptep_modify_prot_commit(vma, addr, ptep, old_pte, pte);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			huge_ptep_modify_prot_commit(vma, addr, curr_pte, old_pte, pte);
		}
	}
}

static inline void huge_pte_clear_replicated(struct mm_struct *mm, unsigned long addr,
					     pte_t *ptep, unsigned long sz)
{
	huge_pte_clear(mm, addr, ptep, sz);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			huge_pte_clear(mm, addr, curr_pte, sz);
		}
	}
}

static inline pte_t huge_ptep_get_and_clear_replicated(struct mm_struct *mm,
						       unsigned long addr, pte_t *ptep)
{
	pte_t ret = huge_ptep_get_and_clear(mm, addr, ptep);

	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			ret = huge_ptep_get_and_clear(mm, addr, curr_pte);
		}
	}
	return ret;
}

static inline void huge_ptep_set_wrprotect_replicated(struct mm_struct *mm,
		unsigned long addr, pte_t *ptep)
{
	huge_ptep_set_wrprotect(mm, addr, ptep);
	if (numa_pgtable_replicated(ptep)) {
		unsigned long offset;
		struct page *curr;
		pte_t *curr_pte;

		for_each_pgtable_replica(curr, curr_pte, ptep, offset) {
			huge_ptep_set_wrprotect(mm, addr, curr_pte);
		}
	}
}

static inline void pmd_clear_replicated(pmd_t *pmdp)
{
	pmd_clear(pmdp);

	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			pmd_clear(curr_pmd);
		}
	}
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline pmd_t pmdp_huge_get_and_clear_replicated(struct mm_struct *mm,
					    unsigned long address, pmd_t *pmdp)
{
	pmd_t pmdval;

	pmdval = pmdp_huge_get_and_clear(mm, address, pmdp);
	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			pmdval = pmdp_huge_get_and_clear(mm, address, curr_pmd);
		}
	}
	return pmdval;
}

static inline void pmdp_set_wrprotect_replicated(struct mm_struct *mm,
				      unsigned long address, pmd_t *pmdp)
{
	pmdp_set_wrprotect(mm, address, pmdp);
	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			pmdp_set_wrprotect(mm, address, curr_pmd);
		}
	}
}

#endif

static inline void huge_pmd_set_accessed_replicated(struct vm_fault *vmf)
{
	pmd_t entry;
	unsigned long haddr;
	pmd_t orig_pmd = vmf->orig_pmd;
	bool write = vmf->flags & FAULT_FLAG_WRITE;
	bool update = false;

	vmf->ptl = pmd_lock(vmf->vma->vm_mm, vmf->pmd);
	if (unlikely(!pmd_same(*vmf->pmd, orig_pmd)))
		goto skip_orig;

	entry = pmd_mkyoung(orig_pmd);
	if (write)
		entry = pmd_mkdirty(entry);
	haddr = vmf->address & HPAGE_PMD_MASK;
	update |= pmdp_set_access_flags(vmf->vma, haddr, vmf->pmd, entry, write);

skip_orig:

	if (numa_pgtable_replicated(vmf->pmd)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, vmf->pmd, offset) {
			entry = pmd_mkyoung(*curr_pmd);
			if (write)
				entry = pmd_mkdirty(entry);
			update |= pmdp_set_access_flags(vmf->vma, haddr, curr_pmd, entry, write);
		}
	}

	if (update)
		update_mmu_cache_pmd(vmf->vma, vmf->address, vmf->pmd);
	spin_unlock(vmf->ptl);
}

static inline int pmdp_set_access_flags_replicated(struct vm_area_struct *vma,
						   unsigned long address, pmd_t *pmdp,
						   pmd_t entry, int dirty)
{
	int res = pmdp_set_access_flags(vma, address, pmdp, entry, dirty);

	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			res |= pmdp_set_access_flags(vma, address, curr_pmd, entry, dirty);
		}
	}
	return res;
}

static inline void set_pmd_at_replicated(struct mm_struct *mm, unsigned long addr,
			      pmd_t *pmdp, pmd_t pmd)
{
	set_pmd_at(mm, addr, pmdp, pmd);
	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			set_pmd_at(mm, addr, curr_pmd, pmd);
		}
	}
}

static inline pmd_t pmdp_invalidate_replicated(struct vm_area_struct *vma, unsigned long address,
		     pmd_t *pmdp)
{
	pmd_t pmdval;

	pmdval = pmdp_invalidate(vma, address, pmdp);
	if (numa_pgtable_replicated(pmdp)) {
		unsigned long offset;
		struct page *curr;
		pmd_t *curr_pmd;

		for_each_pgtable_replica(curr, curr_pmd, pmdp, offset) {
			pmdval = pmdp_invalidate(vma, address, curr_pmd);
		}
	}
	return pmdval;
}

static inline void pud_clear_replicated(pud_t *pudp)
{
	pud_clear(pudp);

	if (numa_pgtable_replicated(pudp)) {
		unsigned long offset;
		struct page *curr;
		pud_t *curr_pud;

		for_each_pgtable_replica(curr, curr_pud, pudp, offset) {
			pud_clear(curr_pud);
		}
	}
}
#endif

static inline void build_pte_chain(struct page **tables)
{
	int nid;
	int prev_node = -1;

	for_each_memory_node(nid) {
		tables[nid]->replica_list_head.first = NULL;
		if (prev_node != -1) {
			llist_add(&tables[nid]->replica_list_node, &tables[prev_node]->replica_list_head);
		} else {
			tables[nid]->replica_list_node.next = &tables[nid]->replica_list_node;
		}
		prev_node = nid;
	}
}

static inline void build_pmd_chain(pmd_t **tables)
{
	int nid;
	int prev_node = -1;

	for_each_memory_node(nid) {
		virt_to_page(tables[nid])->replica_list_head.first = NULL;
		if (prev_node != -1) {
			llist_add(&virt_to_page(tables[nid])->replica_list_node, &virt_to_page(tables[prev_node])->replica_list_head);
		} else {
			virt_to_page(tables[nid])->replica_list_node.next = &virt_to_page(tables[nid])->replica_list_node;
		}
		prev_node = nid;
	}
}

static inline void build_pud_chain(pud_t **tables)
{
	int nid;
	int prev_node = -1;

	for_each_memory_node(nid) {
		virt_to_page(tables[nid])->replica_list_head.first = NULL;
		if (prev_node != -1) {
			llist_add(&virt_to_page(tables[nid])->replica_list_node, &virt_to_page(tables[prev_node])->replica_list_head);
		} else {
			virt_to_page(tables[nid])->replica_list_node.next = &virt_to_page(tables[nid])->replica_list_node;
		}
		prev_node = nid;
	}
}

static inline void build_p4d_chain(p4d_t **tables)
{
	int nid;
	int prev_node = -1;

	for_each_memory_node(nid) {
		virt_to_page(tables[nid])->replica_list_head.first = NULL;
		if (prev_node != -1) {
			llist_add(&virt_to_page(tables[nid])->replica_list_node, &virt_to_page(tables[prev_node])->replica_list_head);
		} else {
			virt_to_page(tables[nid])->replica_list_node.next = &virt_to_page(tables[nid])->replica_list_node;
		}
		prev_node = nid;
	}
}


pgd_t *fault_pgd_offset(struct vm_fault *vmf, unsigned long address);
p4d_t *fault_p4d_alloc(struct vm_fault *vmf, struct mm_struct *mm, pgd_t *pgd, unsigned long address);
pud_t *fault_pud_alloc(struct vm_fault *vmf, struct mm_struct *mm, p4d_t *p4d, unsigned long address);
pmd_t *fault_pmd_alloc(struct vm_fault *vmf, struct mm_struct *mm, pud_t *pud, unsigned long address);
int fault_pte_alloc(struct vm_fault *vmf);
pte_t *huge_pte_alloc_copy_tables(struct mm_struct *dst, struct mm_struct *src,
				  unsigned long addr, unsigned long sz);
pte_t *huge_pte_alloc_replica(struct mm_struct *mm, struct vm_area_struct *vma,
			      unsigned long addr, unsigned long sz);

pte_t *cpr_alloc_pte_map(struct mm_struct *mm, unsigned long addr,
			      pmd_t *src_pmd, pmd_t *dst_pmd);
pte_t *cpr_alloc_pte_map_lock(struct mm_struct *mm, unsigned long addr,
			      pmd_t *src_pmd, pmd_t *dst_pmd, spinlock_t **ptl);
pmd_t *cpr_alloc_pmd(struct mm_struct *mm, unsigned long addr,
		     pud_t *src_pud, pud_t *dst_pud);
pud_t *cpr_alloc_pud(struct mm_struct *mm, unsigned long addr,
		     p4d_t *src_p4d, p4d_t *dst_p4d);
p4d_t *cpr_alloc_p4d(struct mm_struct *mm, unsigned long addr,
		     pgd_t *src_pgd, pgd_t *dst_pgd);

/*
 * Copied from rmap.c
 * We need to only to decrease compound_mapcount
 * and if this was the last thp mapping decrease mapcount
 * of tail pages
 */
static inline void dec_compound_mapcount(struct page *head)
{
	int i, nr;

	if (!atomic_add_negative(-1, compound_mapcount_ptr(head)))
		return;
	if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE))
		return;
	if (TestClearPageDoubleMap(head)) {
		/*
		 * Subpages can be mapped with PTEs too. Check how many of
		 * them are still mapped.
		 */
		for (i = 0, nr = 0; i < thp_nr_pages(head); i++) {
			if (atomic_add_negative(-1, &head[i]._mapcount))
				nr++;
		}

		/*
		 * Queue the page for deferred split if at least one small
		 * page of the compound page is unmapped, but at least one
		 * small page is still mapped.
		 */
		if (nr && nr < thp_nr_pages(head)) {
			if (!PageHotreplace(head))
				deferred_split_huge_page(head);
		}
	}
}

static inline void cleanup_pte_list(pgtable_t table)
{
	table->replica_list_node.next = NULL;
}

static inline void cleanup_pmd_list(struct page *table)
{
#ifndef __PAGETABLE_PMD_FOLDED
	table->replica_list_node.next = NULL;
#endif
}

static inline void cleanup_pud_list(struct page *table)
{
#ifndef __PAGETABLE_PUD_FOLDED
	table->replica_list_node.next = NULL;
#endif
}

static inline void cleanup_p4d_list(struct page *table)
{
#ifndef __PAGETABLE_P4D_FOLDED
	table->replica_list_node.next = NULL;
#endif
}

struct numa_context_switch_stat {
	unsigned long __percpu *pcp_stats;
	unsigned long *last_stats;
	unsigned long *total_stats;
	spinlock_t lock;
};

static inline int mm_init_numa_stats(struct mm_struct *mm)
{
	mm->context_switch_stats = (struct numa_context_switch_stat *)kmalloc(sizeof(struct numa_context_switch_stat), GFP_KERNEL);
	if (!mm->context_switch_stats)
		goto fail1;

	mm->context_switch_stats->last_stats = (unsigned long *)kmalloc(sizeof(unsigned long) * MAX_NUMNODES, GFP_KERNEL | __GFP_ZERO);
	if (!mm->context_switch_stats->last_stats)
		goto fail2;

	mm->context_switch_stats->total_stats = (unsigned long *)kmalloc(sizeof(unsigned long) * MAX_NUMNODES, GFP_KERNEL | __GFP_ZERO);
	if (!mm->context_switch_stats->total_stats)
		goto fail3;

	mm->context_switch_stats->pcp_stats = alloc_percpu_gfp(unsigned long, GFP_KERNEL | __GFP_ZERO);
	if (!mm->context_switch_stats->pcp_stats)
		goto fail4;

	spin_lock_init(&(mm->context_switch_stats->lock));

	return 0;
fail4:
	kfree(mm->context_switch_stats->total_stats);
fail3:
	kfree(mm->context_switch_stats->last_stats);
fail2:
	kfree(mm->context_switch_stats);
	mm->context_switch_stats = NULL;
fail1:
	return -ENOMEM;
}

static inline void mm_free_numa_stats(struct mm_struct *mm)
{
	free_percpu(mm->context_switch_stats->pcp_stats);
	kfree(mm->context_switch_stats->total_stats);
	kfree(mm->context_switch_stats->last_stats);
	kfree(mm->context_switch_stats);
	mm->context_switch_stats = NULL;
}
void numa_account_switch(struct mm_struct *mm);

void numa_accumulate_switches(struct mm_struct *mm);

struct numa_replication_control {
	bool user_replication_active;
	fork_policy_t fork_policy;
	table_replication_policy_t table_policy;
	data_replication_policy_t data_policy;
	spinlock_t lock; // serializes modification of numa_replication_control::in_candidate_list
	bool in_candidate_list;
	struct list_head replication_candidates;
	struct mm_struct *owner;
	struct rw_semaphore rmap_lock;
	unsigned long __percpu *pcp_replicated_pages;
	unsigned long __percpu *pcp_dereplicated_pages;
	unsigned long __percpu *pcp_replicated_tables;
	unsigned long __percpu *pcp_dereplicated_tables;
};

static inline bool get_user_replication_policy(struct mm_struct *mm)
{
	return mm->replication_ctl->user_replication_active;
}

static inline fork_policy_t get_fork_policy(struct mm_struct *mm)
{
	return mm->replication_ctl->fork_policy;
}

static inline table_replication_policy_t get_table_replication_policy(struct mm_struct *mm)
{
	return mm->replication_ctl->table_policy;
}

static inline data_replication_policy_t get_data_replication_policy(struct mm_struct *mm)
{
	return mm->replication_ctl->data_policy;
}

static inline void set_user_replication_policy(struct mm_struct *mm, bool value)
{
	mm->replication_ctl->user_replication_active = value;
}

static inline void set_fork_policy(struct mm_struct *mm, fork_policy_t value)
{
	mm->replication_ctl->fork_policy = value;
}

static inline void set_table_replication_policy(struct mm_struct *mm, table_replication_policy_t value)
{
	mm->replication_ctl->table_policy = value;
}

static inline void set_data_replication_policy(struct mm_struct *mm, data_replication_policy_t value)
{
	mm->replication_ctl->data_policy = value;
}

static inline void __account_replicated_data_size(struct mm_struct *mm, long long size)
{
	if (size == 0)
		return;

	if (size > 0) {
		this_cpu_add(*(mm->replication_ctl->pcp_replicated_pages), size);
	} else {
		this_cpu_add(*(mm->replication_ctl->pcp_dereplicated_pages), -size);
	}
}

static inline void __account_replicated_table_size(struct mm_struct *mm, long long size)
{
	if (size == 0)
		return;

	if (size > 0) {
		this_cpu_add(*(mm->replication_ctl->pcp_replicated_tables), size);
	} else {
		this_cpu_add(*(mm->replication_ctl->pcp_dereplicated_tables), -size);
	}
}

static inline void account_replicated_page(struct mm_struct *mm)
{
	__account_replicated_data_size(mm, PAGE_SIZE * replica_count);
}

static inline void account_replicated_hugepage(struct mm_struct *mm)
{
	__account_replicated_data_size(mm, PMD_SIZE * replica_count);
}

static inline void account_dereplicated_page(struct mm_struct *mm)
{
	__account_replicated_data_size(mm, -PAGE_SIZE * replica_count);
}

static inline void account_dereplicated_hugepage(struct mm_struct *mm)
{
	__account_replicated_data_size(mm, -PMD_SIZE * replica_count);
}

static inline void account_replicated_table(struct mm_struct *mm)
{
	__account_replicated_table_size(mm, PAGE_SIZE * replica_count);
}

static inline void account_dereplicated_table(struct mm_struct *mm)
{
	__account_replicated_table_size(mm, -PAGE_SIZE * replica_count);
}

static inline void __memcg_account_replicated_data_size(struct mem_cgroup *memcg, long long size)
{
	if (size == 0)
		return;

	css_get(&memcg->css);
	if (size > 0) {
		this_cpu_add(*(memcg->replication_ctl->pcp_replicated_pages), size);
	} else {
		this_cpu_add(*(memcg->replication_ctl->pcp_dereplicated_pages), -size);
	}
	css_put(&memcg->css);
}

static inline void memcg_account_replicated_pages(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	__memcg_account_replicated_data_size(memcg, PAGE_SIZE * nr_pages);
}

static inline void memcg_account_dereplicated_pages(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	__memcg_account_replicated_data_size(memcg, -PAGE_SIZE * nr_pages);
}

static inline void memcg_account_replicated_pgtable_page(struct mm_struct *mm, void *pgtable)
{
	struct page *page = virt_to_page(pgtable);
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);

	css_get(&memcg->css);
	BUG_ON(page->memcg_data);

	page->memcg_data = (unsigned long)memcg;


	this_cpu_add(*(memcg->replication_ctl->pcp_replicated_tables), PAGE_SIZE);

	css_put(&memcg->css);

}

static inline void memcg_account_dereplicated_pgtable_page(void *pgtable)
{
	struct page *page = virt_to_page(pgtable);
	struct mem_cgroup *memcg = __page_memcg(page);

	css_get(&memcg->css);
	this_cpu_add(*(memcg->replication_ctl->pcp_replicated_tables), -PAGE_SIZE);
	page->memcg_data = 0;
	css_put(&memcg->css);
}

static inline void memcg_account_replicated_p4d_page(struct mm_struct *mm, void *pgtable)
{
#ifndef __PAGETABLE_P4D_FOLDED
	memcg_account_replicated_pgtable_page(mm, pgtable);
#endif
}

static inline void memcg_account_dereplicated_p4d_page(void *pgtable)
{
#ifndef __PAGETABLE_P4D_FOLDED
	memcg_account_dereplicated_pgtable_page(pgtable);
#endif
}

static inline void memcg_account_replicated_pud_page(struct mm_struct *mm, void *pgtable)
{
#ifndef __PAGETABLE_PUD_FOLDED
	memcg_account_replicated_pgtable_page(mm, pgtable);
#endif
}

static inline void memcg_account_dereplicated_pud_page(void *pgtable)
{
#ifndef __PAGETABLE_PUD_FOLDED
	memcg_account_dereplicated_pgtable_page(pgtable);
#endif
}

static inline void memcg_account_replicated_pmd_page(struct mm_struct *mm, void *pgtable)
{
#ifndef __PAGETABLE_PMD_FOLDED
	memcg_account_replicated_pgtable_page(mm, pgtable);
#endif
}

static inline void memcg_account_dereplicated_pmd_page(void *pgtable)
{
#ifndef __PAGETABLE_PMD_FOLDED
	memcg_account_dereplicated_pgtable_page(pgtable);
#endif
}

static inline void memcg_account_replicated_pte_page(struct mm_struct *mm, void *pgtable)
{
	memcg_account_replicated_pgtable_page(mm, pgtable);
}

static inline void memcg_account_dereplicated_pte_page(void *pgtable)
{
	memcg_account_dereplicated_pgtable_page(pgtable);
}

static inline long long __total_replicated_data(unsigned long __percpu *pcp_replicated,
						unsigned long __percpu *pcp_dereplicated)
{
	long long result = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		unsigned long *ptr = per_cpu_ptr(pcp_replicated, cpu);

		result += *ptr;
		ptr = per_cpu_ptr(pcp_dereplicated, cpu);
		result -= *ptr;
	}
	return result;
}

static inline unsigned long total_replicated_data_bytes_mm(struct mm_struct *mm)
{
	return __total_replicated_data(mm->replication_ctl->pcp_replicated_pages, mm->replication_ctl->pcp_dereplicated_pages);
}

static inline long long total_replicated_data_bytes_memecg(struct mem_cgroup *memcg)
{
	return __total_replicated_data(memcg->replication_ctl->pcp_replicated_pages, memcg->replication_ctl->pcp_dereplicated_pages);
}

static inline unsigned long total_replicated_table_bytes_mm(struct mm_struct *mm)
{
	return __total_replicated_data(mm->replication_ctl->pcp_replicated_tables, mm->replication_ctl->pcp_dereplicated_tables);
}

static inline long long total_replicated_table_bytes_memecg(struct mem_cgroup *memcg)
{
	return __total_replicated_data(memcg->replication_ctl->pcp_replicated_tables, memcg->replication_ctl->pcp_dereplicated_tables);
}

static inline int alloc_numa_replication_ctl(struct mm_struct *mm)
{
	mm->replication_ctl = kmalloc(sizeof(struct numa_replication_control), GFP_KERNEL);
	if (!mm->replication_ctl)
		return -ENOMEM;

	mm->replication_ctl->owner = mm;
	mm->replication_ctl->in_candidate_list = false;
	mm->replication_ctl->user_replication_active = get_mem_cgroup_from_mm(mm)->replication_ctl->table_policy != TABLE_REPLICATION_NONE;
	mm->replication_ctl->fork_policy = get_mem_cgroup_from_mm(mm)->replication_ctl->fork_policy;
	mm->replication_ctl->table_policy = get_mem_cgroup_from_mm(mm)->replication_ctl->table_policy;
	mm->replication_ctl->data_policy = get_mem_cgroup_from_mm(mm)->replication_ctl->data_policy;
	spin_lock_init(&mm->replication_ctl->lock);
	init_rwsem(&mm->replication_ctl->rmap_lock);

	mm->replication_ctl->pcp_replicated_pages = alloc_percpu_gfp(unsigned long, GFP_KERNEL | __GFP_ZERO);
	if (!mm->replication_ctl->pcp_replicated_pages)
		goto fail1;

	mm->replication_ctl->pcp_dereplicated_pages = alloc_percpu_gfp(unsigned long, GFP_KERNEL | __GFP_ZERO);
	if (!mm->replication_ctl->pcp_dereplicated_pages)
		goto fail2;

	mm->replication_ctl->pcp_replicated_tables = alloc_percpu_gfp(unsigned long, GFP_KERNEL | __GFP_ZERO);
	if (!mm->replication_ctl->pcp_replicated_tables)
		goto fail3;


	mm->replication_ctl->pcp_dereplicated_tables = alloc_percpu_gfp(unsigned long, GFP_KERNEL | __GFP_ZERO);
	if (!mm->replication_ctl->pcp_dereplicated_tables)
		goto fail4;

	return 0;

fail4:
	free_percpu(mm->replication_ctl->pcp_replicated_tables);
fail3:
	free_percpu(mm->replication_ctl->pcp_dereplicated_pages);
fail2:
	free_percpu(mm->replication_ctl->pcp_replicated_pages);
fail1:
	kfree(mm->replication_ctl);
	mm->replication_ctl = NULL;

	return -ENOMEM;

}

void free_numa_replication_ctl(struct mm_struct *mm);

int numa_replication_init_sysfs(void);

void numa_replication_add_candidate(struct mm_struct *mm);

int numa_replicate_pgtables_vma(struct vm_area_struct *vma);

int numa_dispatch_table_replication_request(struct mm_struct *mm, int cmd);
int numa_dispatch_data_replication_request(struct mm_struct *mm, int cmd);

void numa_replication_post_mprotect(struct vm_area_struct *vma);

static inline bool vma_want_table_replica(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	table_replication_policy_t policy = get_table_replication_policy(mm);

	if (policy == TABLE_REPLICATION_ALL)
		return true;

	if (policy == TABLE_REPLICATION_MINIMAL && vma_replica_candidate(vma))
		return true;

	return false;
}

static inline void numa_mprotect_vm_flags_modify(unsigned long *newflags, struct vm_area_struct *vma)
{
	table_replication_policy_t table_policy = get_table_replication_policy(vma->vm_mm);
	data_replication_policy_t data_policy = get_data_replication_policy(vma->vm_mm);

	switch (table_policy) {
	case TABLE_REPLICATION_MINIMAL: {
		if (__vm_flags_replica_candidate(vma, *newflags))
			*newflags |= VM_REPLICA_INIT;
		break;
	}
	case TABLE_REPLICATION_ALL: {
		*newflags |= VM_REPLICA_INIT;
		break;
	}
	case TABLE_REPLICATION_NONE: {
		return;
	}
	default:
		BUG();
	}

	switch (data_policy) {
	case DATA_REPLICATION_NONE: {
		return;
	}
	case DATA_REPLICATION_ON_DEMAND:
	case DATA_REPLICATION_ALL_MAPPED_ON_DEMAND:
	case DATA_REPLICATION_ALL: {
		if (__vm_flags_replica_candidate(vma, *newflags))
			*newflags |= VM_REPLICA_COMMIT;
		break;
	}
	default:
		BUG();
	}

	return;
}

#else /* !CONFIG_USER_REPLICATION */

struct pgtable_private {
	pte_t **pte_numa;
	struct page **replica_pages;
};

static inline void pgtable_pte_step(struct pgtable_private *zp, int nr) { }
static inline void pgtable_update_pte(struct pgtable_private *zp, pte_t *pte) { }

static inline pmd_t *get_master_pmd(pmd_t *pmd)
{
	return pmd;
}

static inline int numa_is_vma_replicant(struct vm_area_struct *vma)
{
	return 0;
}

static inline bool vma_has_replicas(struct vm_area_struct *vma)
{
	return 0;
}

static inline bool vma_might_be_replicated(struct vm_area_struct *vma)
{
	return 0;
}

static inline int numa_replication_init_sysfs(void)
{
	return 0;
}

static inline void mm_free_numa_stats(struct mm_struct *mm) { }

static inline int mm_init_numa_stats(struct mm_struct *mm)
{
	return 0;
}

static inline int alloc_numa_replication_ctl(struct mm_struct *mm)
{
	return 0;
}

static inline data_replication_policy_t get_data_replication_policy(struct mm_struct *mm)
{
	return DATA_REPLICATION_NONE;
}

#define pte_clear_replicated pte_clear
#define set_pte_at_notify_replicated set_pte_at_notify
#define set_pte_at_replicated set_pte_at
#define ptep_clear_flush_replicated ptep_clear_flush
#define ptep_clear_flush_young_notify_replicated ptep_clear_flush_young_notify
#define ptep_clear_flush_notify_replicated ptep_clear_flush_notify
#define ptep_clear_young_notify_replicated ptep_clear_young_notify
#define ptep_get_and_clear_replicated ptep_get_and_clear
#define ptep_get_and_clear_full_replicated ptep_get_and_clear_full
#define pte_clear_not_present_full_replicated pte_clear_not_present_full
#define ptep_set_wrprotect_replicated ptep_set_wrprotect
#define ptep_modify_prot_start_replicated ptep_modify_prot_start
#define ptep_modify_prot_commit_replicated ptep_modify_prot_commit
#define set_huge_pte_at_replicated set_huge_pte_at
#define huge_ptep_set_access_flags_replicated huge_ptep_set_access_flags
#define huge_ptep_clear_flush_replicated huge_ptep_clear_flush
#define set_huge_swap_pte_at_replicated set_huge_swap_pte_at
#define huge_ptep_modify_prot_start_replicated huge_ptep_modify_prot_start
#define huge_ptep_modify_prot_commit_replicated huge_ptep_modify_prot_commit
#define huge_pte_clear_replicated huge_pte_clear
#define huge_ptep_get_and_clear_replicated huge_ptep_get_and_clear
#define huge_ptep_set_wrprotect_replicated huge_ptep_set_wrprotect
#define ptep_set_access_flags_replicated ptep_set_access_flags
#define pmd_clear_replicated pmd_clear
#define huge_pte_alloc_copy_tables(dst, src, addr, sz) huge_pte_alloc(dst, addr, sz)
#define huge_pte_alloc_replica(mm, vma, addr, sz) huge_pte_alloc(mm, addr, sz)

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define pmdp_huge_get_and_clear_replicated pmdp_huge_get_and_clear
#define pmdp_set_wrprotect_replicated pmdp_set_wrprotect
#endif

#define pmdp_set_access_flags_replicated pmdp_set_access_flags
#define huge_pmd_set_accessed_replicated huge_pmd_set_accessed
#define set_pmd_at_replicated set_pmd_at
#define pmdp_invalidate_replicated pmdp_invalidate
#define pud_clear_replicated pud_clear

#endif /* CONFIG_USER_REPLICATION */

#endif /* _LINUX_NUMA_USER_REPLICATION_H */