#include <linux/numa_user_replication.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/kernel.h>

#include <asm/tlb.h>

#include "internal.h"


DEFINE_SPINLOCK(replication_candidates_lock);
LIST_HEAD(replication_candidates);

/* copypaste from mm/memory.c:195 */
static void add_mm_counter_fast(struct mm_struct *mm, int member, int val)
{
	struct task_struct *task = current;

	if (likely(task->mm == mm))
		task->rss_stat.count[member] += val;
	else
		add_mm_counter(mm, member, val);
}
#define inc_mm_counter_fast(mm, member) add_mm_counter_fast(mm, member, 1)
#define dec_mm_counter_fast(mm, member) add_mm_counter_fast(mm, member, -1)


static int pick_remaining_node(struct page *page, struct vm_area_struct *vma, unsigned long addr)
{
	return mpol_misplaced(page, vma, addr);
}

static int phys_deduplicate_pte_entry(struct mmu_gather *tlb, struct vm_area_struct *vma,
				      pmd_t *pmd, unsigned long addr, bool alloc_new_page, struct page **new_page)
{
	spinlock_t *ptl;
	struct pgtable_private zp;
	int nid;
	int remaining_node;
	int error = 0;
	struct page *remaining_page;
	pte_t new_entry;

	pte_t *head_pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);

	if (pte_present(*head_pte)) {
		struct page *page = vm_normal_page(vma, addr, *head_pte);

		if (!page) {
			pte_unmap_unlock(head_pte, ptl);
			return 0;
		}

		if (page && !PageReplicated(compound_head(page))) {
			pte_unmap_unlock(head_pte, ptl);
			return 0;
		}
	} else {
		pte_unmap_unlock(head_pte, ptl);
		return 0;
	}

	pgtable_update_pte(&zp, head_pte);

	for_each_memory_node(nid) {
		zp.replica_pages[nid] = vm_normal_page(vma, addr, *zp.pte_numa[nid]);
	}

	if (alloc_new_page) {
		remaining_node = NUMA_NO_NODE;
		remaining_page = *new_page;
		*new_page = NULL;

		inc_mm_counter_fast(vma->vm_mm, MM_ANONPAGES);
		reliable_page_counter(remaining_page, vma->vm_mm, 1);
	} else {
		remaining_node = pick_remaining_node(zp.replica_pages[first_memory_node], vma, addr);

		if (remaining_node == -1) {
			remaining_node = first_memory_node;
		}
		remaining_page = zp.replica_pages[remaining_node];
	}

	new_entry = pfn_pte(page_to_pfn(remaining_page), vma->vm_page_prot);

	if (alloc_new_page) {
		void *src_vaddr = page_to_virt(zp.replica_pages[first_memory_node]);
		void *dst_vaddr = page_to_virt(remaining_page);

		copy_page(dst_vaddr, src_vaddr);
	} else {
		ClearPageReplicated(remaining_page);
	}

	page_add_new_anon_rmap(remaining_page, vma, addr, false);
	lru_cache_add_inactive_or_unevictable(remaining_page, vma);

	for_each_memory_node(nid) {
		if (nid == remaining_node)
			continue;
		pte_clear(vma->vm_mm, addr, zp.pte_numa[nid]);
		set_pte_at(vma->vm_mm, addr, zp.pte_numa[nid], new_entry);
		tlb_remove_tlb_entry(tlb, zp.pte_numa[nid], addr);
	}


	pte_unmap_unlock(head_pte, ptl);

	for_each_memory_node(nid) {
		int res;

		if (nid == remaining_node)
			continue;

		dec_mm_counter_fast(vma->vm_mm, MM_ANONPAGES);
		reliable_page_counter(zp.replica_pages[nid], vma->vm_mm, -1);
		res = __tlb_remove_page(tlb, zp.replica_pages[nid]);

		if (unlikely(res))
			tlb_flush_mmu(tlb);
	}
	account_dereplicated_page(vma->vm_mm);
	return error;
}

static int prealloc_page_for_deduplication(struct vm_area_struct *vma, unsigned long addr, bool alloc_new_page, struct page **page)
{
	if (!alloc_new_page)
		return 0;
	if (*page)
		return 0;

	*page = alloc_zeroed_user_highpage_movable(vma, addr);

	if (!(*page))
		return -ENOMEM;

	return 0;
}

static int phys_deduplicate_pte_range(struct mmu_gather *tlb, struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end, bool alloc_new_page)
{
	int error = 0;
	struct page *prealloc_page = NULL;

	tlb_change_page_size(tlb, PAGE_SIZE);
	flush_tlb_batched_pending(vma->vm_mm);
	arch_enter_lazy_mmu_mode();
	do {
		error = prealloc_page_for_deduplication(vma, addr, alloc_new_page, &prealloc_page);
		if (error)
			goto out;

		error = phys_deduplicate_pte_entry(tlb, vma, pmd, addr, alloc_new_page, &prealloc_page);
		if (error)
			goto out;

	} while (addr += PAGE_SIZE, addr != end);
	arch_leave_lazy_mmu_mode();
out:
	if (prealloc_page)
		put_page(prealloc_page);

	return error;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

extern void zap_deposited_table(struct mm_struct *mm, pmd_t *pmd);
extern gfp_t alloc_hugepage_direct_gfpmask(struct vm_area_struct *vma);

static int prealloc_hugepage_for_deduplication(struct vm_area_struct *vma, unsigned long addr, bool alloc_new_page, struct page **page)
{
	gfp_t gfp;

	if (!alloc_new_page)
		return 0;
	if (*page)
		return 0;

	gfp = alloc_hugepage_direct_gfpmask(vma);
	*page = alloc_hugepage_vma(gfp, vma, addr, HPAGE_PMD_ORDER);

	if (!(*page))
		return -ENOMEM;

	prep_transhuge_page(*page);
	return 0;
}

static void copy_huge_page(void *to, void *from)
{
	int i;

	for (i = 0; i < HPAGE_PMD_NR; i++, to += PAGE_SIZE, from += PAGE_SIZE) {
		copy_page(to, from);
	}
}

static int phys_deduplicate_huge_pmd(struct mmu_gather *tlb, struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end, bool alloc_new_page, struct page **new_page)
{
	spinlock_t *ptl;
	int nid;
	struct page *curr;
	pmd_t *curr_pmd;
	unsigned long offset;
	bool start;
	struct page *remaining_page;
	pmd_t entry;
	int remaining_node;
	struct page *replica_pages[MAX_NUMNODES];

	if (prealloc_hugepage_for_deduplication(vma, addr, alloc_new_page, new_page))
		return -ENOMEM;

	ptl = __pmd_trans_huge_lock(pmd, vma);
	if (!ptl)
		return -EAGAIN;

	if (is_huge_zero_pmd(*pmd))
		goto out;

	if (!pmd_present(*pmd))
		goto out;

	if (!PageReplicated(pmd_page(*pmd)))
		goto out;

	for_each_pgtable(curr, curr_pmd, pmd, nid, offset, start) {
		replica_pages[nid] = pmd_page(*curr_pmd);
		if (curr_pmd != get_master_pmd(curr_pmd))
			zap_deposited_table(vma->vm_mm, curr_pmd);
	}

	if (alloc_new_page) {
		remaining_node = NUMA_NO_NODE;
		remaining_page = *new_page;
		*new_page = NULL;

		add_mm_counter(vma->vm_mm, MM_ANONPAGES, HPAGE_PMD_NR);
		reliable_page_counter(remaining_page, vma->vm_mm, HPAGE_PMD_NR);
	} else {
		remaining_node = pick_remaining_node(replica_pages[first_memory_node], vma, addr);

		if (remaining_node == -1) {
			remaining_node = first_memory_node;
		}

		remaining_page = replica_pages[remaining_node];
	}

	entry = mk_huge_pmd(remaining_page, vma->vm_page_prot);

	if (alloc_new_page) {
		void *src_vaddr = page_to_virt(replica_pages[first_memory_node]);
		void *dst_vaddr = page_to_virt(remaining_page);

		copy_huge_page(dst_vaddr, src_vaddr);
	} else {
		ClearPageReplicated(remaining_page);
	}

	page_add_new_anon_rmap(remaining_page, vma, addr, true);
	lru_cache_add_inactive_or_unevictable(remaining_page, vma);

	for_each_pgtable(curr, curr_pmd, pmd, nid, offset, start) {
		if (nid == remaining_node)
			continue;
		pmd_clear(curr_pmd);
		atomic_dec(compound_mapcount_ptr(replica_pages[nid]));
		set_pmd_at(vma->vm_mm, addr, curr_pmd, entry);
		tlb_remove_pmd_tlb_entry(tlb, curr_pmd, addr);
	}


	spin_unlock(ptl);

	for_each_memory_node(nid) {
		if (nid == remaining_node)
			continue;

		add_mm_counter(vma->vm_mm, MM_ANONPAGES, -HPAGE_PMD_NR);
		reliable_page_counter(replica_pages[nid], vma->vm_mm, -HPAGE_PMD_NR);
		tlb_remove_page_size(tlb, replica_pages[nid], HPAGE_PMD_SIZE);
	}
	account_dereplicated_hugepage(vma->vm_mm);

	return 0;
out:
	spin_unlock(ptl);
	return 0;
}

#else /* !CONFIG_TRANSPARENT_HUGEPAGE */

static int phys_deduplicate_huge_pmd(struct mmu_gather *tlb, struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end, bool alloc_new_page, struct page **new_page)
{
	return 0;
}

#endif

static int phys_deduplicate_pmd_range(struct mmu_gather *tlb, struct vm_area_struct *vma, pud_t *pud,
				      unsigned long addr, unsigned long end, bool alloc_new_page)
{
	pmd_t *pmd;
	unsigned long next;
	int error = 0;
	struct page *prealloc_hugepage = NULL;
retry:
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);

		if (pmd_none(*pmd))
			continue;
		if (is_swap_pmd(*pmd))
			continue;
		if (pmd_devmap(*pmd))
			BUG();
		if (pmd_trans_huge(*pmd)) {

			/* Same as in the phys_duplicate */
			BUG_ON(next - addr != HPAGE_PMD_SIZE);
			error = phys_deduplicate_huge_pmd(tlb, vma, pmd, addr, next, alloc_new_page, &prealloc_hugepage);
			if (error == -EAGAIN)
				goto retry;
		} else {
			error = phys_deduplicate_pte_range(tlb, vma, pmd, addr, next, alloc_new_page);
		}

		if (error)
			goto out;

		cond_resched();
	} while (pmd++, addr = next, addr != end);
out:
	if (prealloc_hugepage)
		put_page(prealloc_hugepage);

	return error;
}

static int phys_deduplicate_pud_range(struct mmu_gather *tlb, struct vm_area_struct *vma, p4d_t *p4d,
				      unsigned long addr, unsigned long end, bool alloc_new_page)
{
	pud_t *pud;
	unsigned long next;
	int error = 0;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		if (pud_none_or_clear_bad(pud))
			continue;

		error = phys_deduplicate_pmd_range(tlb, vma, pud, addr, next, alloc_new_page);
		if (error)
			goto out;

	} while (pud++, addr = next, addr != end);
out:
	return error;
}

static int phys_deduplicate_p4d_range(struct mmu_gather *tlb, struct vm_area_struct *vma, pgd_t *pgd,
				      unsigned long addr, unsigned long end, bool alloc_new_page)
{
	p4d_t *p4d;
	unsigned long next;
	int error = 0;

	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);

		if (p4d_none_or_clear_bad(p4d))
			continue;

		error = phys_deduplicate_pud_range(tlb, vma, p4d, addr, next, alloc_new_page);
		if (error)
			goto out;

	} while (p4d++, addr = next, addr != end);
out:
	return error;
}

static int phys_deduplicate_pgd_range(struct mmu_gather *tlb, struct vm_area_struct *vma, unsigned long addr,
				      unsigned long end, bool alloc_new_page)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	unsigned long start = addr;
	int error = 0;

	BUG_ON(addr >= end);

	addr = start;
	tlb_start_vma(tlb, vma);
	pgd = pgd_offset(mm, addr);
	do {
		next = pgd_addr_end(addr, end);

		if (pgd_none_or_clear_bad(pgd))
			continue;

		error = phys_deduplicate_p4d_range(tlb, vma, pgd, addr, next, alloc_new_page);
		if (error)
			goto out;

	} while (pgd++, addr = next, addr != end);

	tlb_end_vma(tlb, vma);
out:
	return error;
}

/*
 * Pages inside [addr; end) are 100% populated,
 * so we can't skip some checks and simplify code.
 */
static int phys_deduplicate_range(struct vm_area_struct *vma, unsigned long addr, unsigned long end, bool alloc_new_page)
{
	struct mmu_gather tlb;
	int error = 0;

	if (addr == end)
		return 0;

	tlb_gather_mmu(&tlb, vma->vm_mm, addr, end);

	error = phys_deduplicate_pgd_range(&tlb, vma, addr, end, alloc_new_page);

	tlb_finish_mmu(&tlb, addr, end);

	if (!error && printk_ratelimit()) {
		pr_info("Deduplicated range: 0x%016lx --- 0x%016lx,  mm: 0x%016lx, PID: %d name: %s\n",
			addr, end, (unsigned long)(vma->vm_mm), vma->vm_mm->owner->pid, vma->vm_mm->owner->comm);
	}

	BUG_ON(error && !alloc_new_page);

	return error;
}

int numa_remove_replicas(struct vm_area_struct *vma, unsigned long start, unsigned long end, bool alloc_new_page)
{
	int error = 0;

	start = start & PAGE_MASK;
	end = end & PAGE_MASK;

	error = phys_deduplicate_range(vma, start, end, alloc_new_page);


	return error;
}

int phys_deduplicate(struct vm_area_struct *vma, unsigned long start, size_t len, bool alloc_new_page)
{
	if (!vma) {
		pr_warn("%s -- %s:%d\n", __func__, __FILE__, __LINE__);
		return -EINVAL;
	}

	if (!(vma->vm_flags & VM_REPLICA_COMMIT))
		return -EINVAL;

	numa_remove_replicas(vma, vma->vm_start, vma->vm_end, alloc_new_page);

	return 0;
}

int __fixup_fault(struct vm_area_struct *vma, unsigned long addr)
{
	return (handle_mm_fault(vma, addr, FAULT_FLAG_INTERRUPTIBLE | FAULT_FLAG_KILLABLE |
					   FAULT_FLAG_RETRY_NOWAIT  | FAULT_FLAG_ALLOW_RETRY, NULL) & VM_FAULT_ERROR);
}


int fixup_fault(struct vm_area_struct *vma, unsigned long addr)
{
	vm_fault_t fault = __fixup_fault(vma, addr);

	if (fault & VM_FAULT_SIGBUS)
		return 0;
	return !!(fault & VM_FAULT_ERROR);
}

static int phys_duplicate_pte_entry(struct vm_area_struct *vma, pmd_t *pmd,
				    unsigned long addr, struct page **replica_page)
{
	struct page *new_page;
	struct page *orig_page;
	int nid;
	char *new_vaddr;
	struct page *pg;
	pte_t entry;
	int pte_nid;
	void *src_addr;
	int reason = 0;
	struct pgtable_private ptes;
	spinlock_t *ptl;
	pte_t *pte, *head_pte;
retry:
	head_pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);

	if (!numa_pgtable_replicated(head_pte)) {
		vm_fault_t fault;

		pte_unmap_unlock(head_pte, ptl);
		fault = __fixup_fault(vma, addr);

		if (fault & VM_FAULT_SIGBUS)
			return -EBUSY;
		if (fault & VM_FAULT_RETRY)
			return -EBUSY;
		if (fault)
			return -ENOMEM;

		goto retry;
	}


	pgtable_update_pte(&ptes, head_pte);

	/* It could happen on a not yet faulted vaddr. Now we require from user to
	 * put MAP_POPULATE manually, but add it with MAP_REPLICA silently.
	 */

	pte = ptes.pte_numa[first_memory_node];

	for_each_memory_node(nid) {
		/*
		 * For some unknown reasons, there are cases, when
		 * pte_level populated only on single node. This is not good,
		 * to avoid this check all ptes now, but this should not happening at all
		 */
		if (!pte_present(*ptes.pte_numa[nid])) {
			pte_unmap_unlock(head_pte, ptl);

			return -EBUSY;
		}
	}

	if (pte_write(*pte)) {
		reason = 2;
		goto bug;
	}


	/* We can handle this case only for 0th node table (I hope so),
	 * because we are under pte_lock, which serializes migration pte modifications
	 */

	orig_page = vm_normal_page(vma, addr, *pte);

	if (orig_page && PageReplicated(compound_head(orig_page))) {
		pte_unmap_unlock(head_pte, ptl);
		return -EBUSY;
	}

	for_each_memory_node(nid) {
		pte = ptes.pte_numa[nid];

		pg = pte_page(*pte);
		pte_nid = page_to_nid(pg);

		new_page = replica_page[nid];
		replica_page[nid] = NULL;

		new_vaddr = page_to_virt(new_page);
		src_addr = page_to_virt(pg);

		copy_page(new_vaddr, src_addr);

		__SetPageUptodate(new_page);

		inc_mm_counter(vma->vm_mm, MM_ANONPAGES);
		reliable_page_counter(new_page, vma->vm_mm, 1);
		entry = pfn_pte(page_to_pfn(new_page), vma->vm_page_prot);

		pte_clear(vma->vm_mm, addr, pte);
		set_pte_at(vma->vm_mm, addr, pte, entry);

		update_mmu_cache(vma, addr, pte);
	}
	account_replicated_page(vma->vm_mm);
	if (orig_page) {
		dec_mm_counter_fast(vma->vm_mm, mm_counter(orig_page));
		reliable_page_counter(orig_page, vma->vm_mm, -1);
		page_remove_rmap(orig_page, false);
	}

	pte_unmap_unlock(head_pte, ptl);

	if (orig_page) {
		free_pages_and_swap_cache(&orig_page, 1);
	}

	return 0;

bug:
	dump_mm_pgtables(vma->vm_mm, addr, addr + PAGE_SIZE * 4 - 1);

	pr_info("Died because BUG_ON #%d\n", reason);

	BUG();
}

static void release_prealloc_pages(struct page **pages)
{
	int nid;

	for_each_memory_node(nid) {
		if (pages[nid] != NULL) {
			put_page(pages[nid]);
			pages[nid] = NULL;
		}
	}
}

static int prealloc_pages_for_replicas(struct mm_struct *mm, struct page **pages, int order)
{
	int nid;
	gfp_t gfp = (GFP_HIGHUSER | __GFP_THISNODE) & (~__GFP_DIRECT_RECLAIM);

	if (order)
		gfp |= __GFP_COMP;
	for_each_memory_node(nid) {
		/*
		 * Do not reclaim in case of memory shortage, just fail
		 * We already don't have enough memory.
		 * Also, make replica pages unmovable
		 */
		pages[nid] = alloc_pages_node(nid, gfp, order);
		if (pages[nid] == NULL)
			goto fail;
		SetPageReplicated(pages[nid]);
		if (mem_cgroup_charge(pages[nid], mm, GFP_KERNEL))
			goto fail;
	}

	for_each_memory_node(nid) {
		cgroup_throttle_swaprate(pages[nid], GFP_KERNEL);
	}

	return 0;

fail:
	release_prealloc_pages(pages);
	return -ENOMEM;
}

/*
 * We must hold at least mmap_read_lock
 */
unsigned long phys_duplicate_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end)
{
	struct page *prealloc_pages[MAX_NUMNODES] = {};

	flush_tlb_batched_pending(vma->vm_mm);
	do {
		int ret = 0;

		if (prealloc_pages_for_replicas(vma->vm_mm, prealloc_pages, 0))
			break;
		ret = phys_duplicate_pte_entry(vma, pmd, addr, prealloc_pages);
		if (ret)
			release_prealloc_pages(prealloc_pages);
		if (ret == -ENOMEM)
			break;

	} while (addr += PAGE_SIZE, addr != end);


	return addr;
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

static void release_deposit_pgtables(struct mm_struct *mm, pgtable_t *pgtables)
{
	int nid;

	for_each_memory_node(nid) {
		if (pgtables[nid] != NULL) {
			pte_free(mm, pgtables[nid]);
			pgtables[nid] = NULL;
		}
	}
}

static int prealloc_deposit_pgtables(struct mm_struct *mm, pgtable_t *pgtables)
{
	int nid;

	for_each_memory_node(nid) {
		pgtables[nid] = pte_alloc_one_node(nid, mm);
		if (!pgtables[nid])
			goto fail;
	}

	return 0;
fail:
	release_deposit_pgtables(mm, pgtables);
	return -ENOMEM;
}

int phys_duplicate_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end)
{
	pgtable_t deposit_ptes[MAX_NUMNODES] = {};
	struct page *prealloc_pages[MAX_NUMNODES] = {};
	spinlock_t *ptl;
	int nid;
	struct page *curr;
	pmd_t *curr_pmd;
	unsigned long offset;
	bool start;
	unsigned long ret = 0;
	pmd_t orig_pmd;
	pmd_t entry;
	struct page *orig_page;

	if (prealloc_deposit_pgtables(vma->vm_mm, deposit_ptes)) {
		ret = -ENOMEM;
		goto out;
	}

	if (prealloc_pages_for_replicas(vma->vm_mm, prealloc_pages, HPAGE_PMD_ORDER)) {
		ret = -ENOMEM;
		goto out;
	}

	ptl = __pmd_trans_huge_lock(pmd, vma);

	if (!ptl) {
		ret = -EAGAIN;
		goto out;
	}

	for_each_pgtable(curr, curr_pmd, pmd, nid, offset, start) {
		/*
		 * For some unknown reasons, there are cases, when
		 * pte_level populated only on single node. This is not good,
		 * to avoid this check all ptes now, but this should not happening at all
		 */
		if (!pmd_present(*curr_pmd)) {
			spin_unlock(ptl);

			ret = 0;
			goto out;
		}
	}

	orig_pmd = *pmd;
	orig_page = pmd_page(orig_pmd);

	if (PageReplicated(orig_page)) {
		spin_unlock(ptl);
		ret = -EBUSY;
		goto out;
	}

	zap_deposited_table(vma->vm_mm, get_master_pmd(pmd));

	for_each_pgtable(curr, curr_pmd, pmd, nid, offset, start) {
		copy_huge_page(page_to_virt(prealloc_pages[nid]), page_to_virt(orig_page));
		prep_transhuge_page(prealloc_pages[nid]);
		SetPageReplicated(prealloc_pages[nid]);

		entry = mk_huge_pmd(prealloc_pages[nid], vma->vm_page_prot);

		atomic_inc(compound_mapcount_ptr(prealloc_pages[nid]));
		reliable_page_counter(prealloc_pages[nid], vma->vm_mm, HPAGE_PMD_NR);

		prealloc_pages[nid] = NULL;

		pgtable_trans_huge_deposit(vma->vm_mm, curr_pmd, deposit_ptes[nid]);
		deposit_ptes[nid] = NULL;


		pmd_clear(curr_pmd);

		set_pmd_at(vma->vm_mm, addr, curr_pmd, entry);
		add_mm_counter(vma->vm_mm, MM_ANONPAGES, HPAGE_PMD_NR);

		mm_inc_nr_ptes(vma->vm_mm);

	}
	account_replicated_hugepage(vma->vm_mm);
	if (!is_huge_zero_pmd(orig_pmd)) {
		add_mm_counter(vma->vm_mm, mm_counter(orig_page), -HPAGE_PMD_NR);
		reliable_page_counter(orig_page, vma->vm_mm, -HPAGE_PMD_NR);
		page_remove_rmap(orig_page, true);
	}

	spin_unlock(ptl);

	if (!is_huge_zero_pmd(orig_pmd)) {
		free_pages_and_swap_cache(&orig_page, 1);
	}

out:
	release_deposit_pgtables(vma->vm_mm, deposit_ptes);
	release_prealloc_pages(prealloc_pages);
	return ret;
}

#else /* !CONFIG_TRANSPARENT_HUGEPAGE */

int phys_duplicate_huge_pmd(struct vm_area_struct *vma, pmd_t *pmd,
					      unsigned long addr, unsigned long end)
{
	return 0;
}

#endif

static unsigned long phys_duplicate_pmd_range(struct vm_area_struct *vma, pud_t *pud,
					      unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next, last = addr;
retry:
	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);

		if (pmd_none(*pmd) || !numa_pgtable_replicated(pmd) || is_swap_pmd(*pmd)) {
			if (fixup_fault(vma, addr))
				break;
			goto retry;
		}

		if (pmd_devmap(*pmd)) {
			BUG(); // not supported right now, probably only trans_huge will be
		}

		if (pmd_trans_huge(*pmd)) {
			int ret;
			/*
			 * Leave this bug for now,
			 * need to carefully think how to handle this situation
			 */
			BUG_ON(next - addr != HPAGE_PMD_SIZE);
			ret = phys_duplicate_huge_pmd(vma, pmd, addr, next);
			if (ret == -EAGAIN)
				goto retry;
			if (!ret || ret == -EBUSY)
				last = next;
		} else {
			last = phys_duplicate_pte_range(vma, pmd, addr, next);
		}

		if (last != next)
			break;

		cond_resched();
	} while (pmd++, addr = next, addr != end);

	return last;
}

static unsigned long phys_duplicate_pud_range(struct vm_area_struct *vma, p4d_t *p4d,
					      unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next, last = addr;
retry:
	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		if (pud_none_or_clear_bad(pud) || !numa_pgtable_replicated(pud)) {
			if (fixup_fault(vma, addr))
				break;
			goto retry;
		}

		last = phys_duplicate_pmd_range(vma, pud, addr, next);

		if (last != next)
			break;

	} while (pud++, addr = next, addr != end);

	return last;
}

static unsigned long phys_duplicate_p4d_range(struct vm_area_struct *vma, pgd_t *pgd,
					      unsigned long addr, unsigned long end)
{
	p4d_t *p4d;
	unsigned long next, last = addr;
retry:
	p4d = p4d_offset(pgd, addr);
	do {
		next = p4d_addr_end(addr, end);

		if (p4d_none_or_clear_bad(p4d) || !numa_pgtable_replicated(p4d)) {
			if (fixup_fault(vma, addr))
				break;
			goto retry;
		}

		last = phys_duplicate_pud_range(vma, p4d, addr, next);

		if (last != next)
			break;

	} while (p4d++, addr = next, addr != end);

	return last;
}

static unsigned long phys_duplicate_pgd_range(struct vm_area_struct *vma, unsigned long addr, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	unsigned long start = addr;
	unsigned long last = addr;

	BUG_ON(addr >= end);

	addr = start;

	flush_cache_range(vma, addr, end);
	inc_tlb_flush_pending(mm);

	pgd = pgd_offset_pgd(this_node_pgd(mm), addr);
retry:

	do {
		next = pgd_addr_end(addr, end);

		if (pgd_none_or_clear_bad(pgd)) {
			if (fixup_fault(vma, addr))
				break;
			goto retry;
		}

		last = phys_duplicate_p4d_range(vma, pgd, addr, next);

		if (last != next)
			break;

	} while (pgd++, addr = next, addr != end);

	if (last == end)
		flush_tlb_range(vma, start, end);
	dec_tlb_flush_pending(mm);

	return last;

}

/*
 * We must hold at least mmap_read_lock
 */
static unsigned long phys_duplicate_range(struct vm_area_struct *vma, unsigned long addr, unsigned long end)
{
	unsigned long last;

	if (unlikely(anon_vma_prepare(vma)))
		return addr;

	last = phys_duplicate_pgd_range(vma, addr, end);

	return last;
}

int numa_clone_pte(struct vm_area_struct *vma, unsigned long start, unsigned long end)
{
	unsigned long last = 0;

	start = start & PAGE_MASK;
	end = end & PAGE_MASK;

	last = phys_duplicate_range(vma, start, end);

	if (last != end) {
		phys_deduplicate_range(vma, start, last, false);
		return -ENOMEM;
	}

	return 0;
}

int phys_duplicate(struct vm_area_struct *vma, unsigned long start, size_t len)
{
	int error = 0;

	if (!vma) {
		pr_warn("%s -- %s:%d\n", __func__, __FILE__, __LINE__);
		return -ENOMEM;
	}

	if (vma->vm_flags & VM_WRITE)
		return -EINVAL;


	if ((start < vma->vm_start) || (start + len > vma->vm_end)) {
		pr_warn("Replication is possible only inside vma\n");
		pr_warn("vma->vm_start %zx; len %zx\n", vma->vm_start,
				vma->vm_end - vma->vm_start);
		return -ENOMEM;
	}

	if (!numa_is_vma_replicant(vma)) {
		pr_warn("%s -- %s:%d\n", __func__, __FILE__, __LINE__);
		return -EINVAL;
	}

	error = numa_clone_pte(vma, start, start + len);
	if (!error) {
		// pr_info("Successfully replicated memory -- start:%zx; len:%zx PID: %d NAME: %s\n",
			// start, len, vma->vm_mm->owner->pid, vma->vm_mm->owner->comm);
		vma->vm_flags |= VM_REPLICA_COMMIT;
	}

	flush_tlb_range(vma, start, start + len);

	return error;
}

void numa_replication_remove_from_candidate_list(struct mm_struct *mm)
{


	if (!mm->replication_ctl->in_candidate_list)
		return;

	spin_lock(&mm->replication_ctl->lock);

	/* We are already not in this list */
	if (!mm->replication_ctl->in_candidate_list)
		goto out;

	spin_lock_nested(&replication_candidates_lock, SINGLE_DEPTH_NESTING);

	list_del(&mm->replication_ctl->replication_candidates);

	spin_unlock(&replication_candidates_lock);

	mm->replication_ctl->in_candidate_list = false;
out:
	spin_unlock(&mm->replication_ctl->lock);
}

void numa_mm_apply_replication(struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	if (get_user_replication_policy(mm))
		return;

	mmap_write_lock(mm);

	if (get_user_replication_policy(mm))
		goto out;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		vma->vm_flags |= VM_REPLICA_INIT;

		numa_replicate_pgtables_vma(vma);

		if (vma_might_be_replicated(vma)) {
			phys_duplicate(vma, vma->vm_start, vma->vm_end - vma->vm_start);
			/*
			 * Set this flag anyway, even if we failed.
			 * Our hopes are on numa balancer
			 */
			vma->vm_flags |= VM_REPLICA_COMMIT;
		}
	}

	set_user_replication_policy(mm, true);
	numa_replication_remove_from_candidate_list(mm);
out:
	mmap_write_unlock(mm);
}



static inline bool replicated_p4d_level(struct vm_fault *vmf)
{
	//TODO Do something better
	return mm_p4d_folded(vmf->vma->vm_mm) || vmf->p4d_replicated || pgd_none(*(vmf->pgd));
}

static inline bool replicated_pud_level(struct vm_fault *vmf)
{
	//TODO Do something better
	/* We don't have entries on this level, or they are not the same*/
	return mm_pud_folded(vmf->vma->vm_mm) || vmf->pud_replicated || p4d_none(*(vmf->p4d));
}

static inline bool replicated_pmd_level(struct vm_fault *vmf)
{
	//TODO Do something better
	/* We don't have entries on this level, or they are not the same*/
	return mm_pmd_folded(vmf->vma->vm_mm) || vmf->pmd_replicated || pud_none(*(vmf->pud));
}

static inline bool replicated_pte_level(struct vm_fault *vmf)
{
	//TODO Do something better
	/* We don't have entries on this level, or they are not the same*/
	return vmf->pte_replicated || pmd_none(*(vmf->pmd));
}

static inline bool overlap_pmd_entry(unsigned long address, unsigned long left, unsigned long right)
{
	return  ((address & PMD_MASK) == (left & PMD_MASK)) ||
		((address & PMD_MASK) == (right & PMD_MASK));
}

static inline bool overlap_pud_entry(unsigned long address, unsigned long left, unsigned long right)
{
	return  ((address & PUD_MASK) == (left & PUD_MASK)) ||
		((address & PUD_MASK) == (right & PUD_MASK));
}

static inline bool overlap_p4d_entry(unsigned long address, unsigned long left, unsigned long right)
{
	return  ((address & P4D_MASK) == (left & P4D_MASK)) ||
		((address & P4D_MASK) == (right & P4D_MASK));
}

static inline bool overlap_pgd_entry(unsigned long address, unsigned long left, unsigned long right)
{
	return  ((address & PGDIR_MASK) == (left & PGDIR_MASK)) ||
		((address & PGDIR_MASK) == (right & PGDIR_MASK));
}

static inline void get_replicant_neighbours(struct mm_struct *mm, struct vm_area_struct *vma,
			      unsigned long address, unsigned long *left, unsigned long *right)
{
	*left = ULONG_MAX;
	*right = ULONG_MAX;

	if (numa_is_vma_replicant(vma))
		*left = *right = address;

}

static inline void __replication_path_action(struct vm_fault *vmf, bool replicated)
{
	if (vmf->replica_action != REPLICA_NONE) {
		/*
		 * If we meet propagation action again, that means upper
		 * level has already been propagated and we don't have
		 * replicas anylower -- we need to completely switch
		 * to default handling.
		 */
		if (vmf->replica_action == REPLICA_PROPAGATE)
			vmf->replica_action = REPLICA_NONE;
		else
			vmf->replica_action = replicated ? REPLICA_KEEP : REPLICA_PROPAGATE;
	}
}

static bool replication_path_pgd(struct vm_fault *vmf)
{
	bool p4d_folded = mm_p4d_folded(vmf->vma->vm_mm), replicated;
	struct mm_struct *mm = vmf->vma->vm_mm;
	unsigned long address = vmf->real_address;
	/* There are replicated tables in our pgd entry or there is vma requiring it. Need to replicate next level.
	 * 5-level paging and folded p4d give us a lot of grief.
	 * If 5-level paging disabled, handle_mm_fault_pgd function doing nothing, except filling vmf->p4d_numa
	 * with same values as in vmf->pgd_numa and propagation will not work correctly.
	 * So we need to go in  __handle_mm_fault_p4d_replicant, because we might still want to propagate it.
	 */
	get_replicant_neighbours(mm, vmf->vma, address, &(vmf->left_replicant), &(vmf->right_replicant));
	if (!p4d_folded)
		vmf->p4d_replicated = !pgd_none(*(vmf->pgd)) &&
				       PageReplicated(virt_to_page(pgd_page_vaddr(*vmf->pgd)));
	replicated = p4d_folded || overlap_pgd_entry(address, vmf->left_replicant, vmf->right_replicant)
				|| vmf->p4d_replicated;
	/*
	 * Here replica_action may be REPLICA_NONE, so we ignore that,
	 * because we always replicate top level table.
	 */
	vmf->replica_action = replicated ? REPLICA_KEEP : REPLICA_PROPAGATE;
	return replicated;
}

static bool replication_path_p4d(struct vm_fault *vmf)
{
	bool pud_folded = mm_pud_folded(vmf->vma->vm_mm), replicated;
	unsigned long address = vmf->real_address;

	if (vmf->replica_action == REPLICA_PROPAGATE) {
		/*
		 * We have already propagated upper level,
		 * so we'll never use XXX_replicated values again
		 * during this fault.
		 */
		vmf->replica_action = REPLICA_NONE;
		return false;
	}

	if (!pud_folded)
		vmf->pud_replicated = !p4d_none(*(vmf->p4d)) &&
				      PageReplicated(virt_to_page(p4d_pgtable(*vmf->p4d)));
	replicated = pud_folded || overlap_p4d_entry(address, vmf->left_replicant, vmf->right_replicant)
				|| vmf->pud_replicated;
	__replication_path_action(vmf, replicated);
	return replicated;
}

static bool replication_path_pud(struct vm_fault *vmf)
{
	bool pmd_folded = mm_pmd_folded(vmf->vma->vm_mm), replicated;
	unsigned long address = vmf->real_address;

	if (vmf->replica_action == REPLICA_PROPAGATE) {
		/*
		 * We have already propagated upper level,
		 * so we'll never use XXX_replicated values again
		 * during this fault.
		 */
		vmf->replica_action = REPLICA_NONE;
		return false;
	}

	if (!pmd_folded)
		vmf->pmd_replicated = !pud_none(*(vmf->pud)) &&
				      PageReplicated(virt_to_page(pud_pgtable(*vmf->pud)));
	replicated = pmd_folded || overlap_pud_entry(address, vmf->left_replicant, vmf->right_replicant)
				|| vmf->pmd_replicated;
	__replication_path_action(vmf, replicated);
	return replicated;
}

static bool replication_path_pmd(struct vm_fault *vmf)
{
	bool replicated;
	unsigned long address = vmf->real_address;

	if (vmf->replica_action == REPLICA_PROPAGATE) {
		/*
		 * We have already propagated upper level,
		 * so we'll never use XXX_replicated values again
		 * during this fault.
		 */
		vmf->replica_action = REPLICA_NONE;
		return false;
	}
	vmf->pte_replicated = !pmd_none(*(vmf->pmd)) && !pmd_devmap_trans_unstable(vmf->pmd) &&
			      PageReplicated(pmd_pgtable(*vmf->pmd));
	replicated = overlap_pmd_entry(address, vmf->left_replicant, vmf->right_replicant)
			|| vmf->pte_replicated;
	__replication_path_action(vmf, replicated);
	return replicated;
}

static void
release_replicated_p4d_tables(int allocated_node, p4d_t **new, struct mm_struct *mm)
{
	int nid;

	for_each_memory_node(nid) {
		if (nid == allocated_node || new[nid] == NULL)
			continue;
		p4d_free(mm, new[nid]);
	}

}

static void
release_replicated_pud_tables(int allocated_node, pud_t **new, struct mm_struct *mm)
{
	int nid;

	for_each_memory_node(nid) {
		if (nid == allocated_node || new[nid] == NULL)
			continue;
		pud_free(mm, new[nid]);
	}

}

static void
release_replicated_pmd_tables(int allocated_node, pmd_t **new, struct mm_struct *mm)
{
	int nid;

	for_each_memory_node(nid) {
		if (nid == allocated_node || new[nid] == NULL)
			continue;
		pmd_free(mm, new[nid]);
	}

}

static void
release_replicated_pte_tables(int allocated_node, struct page **new, struct mm_struct *mm)
{
	int nid;

	for_each_memory_node(nid) {
		if (nid == allocated_node || new[nid] == NULL)
			continue;
		if (allocated_node == NUMA_NO_NODE) {
			ClearPageReplicated(new[nid]);
			new[nid]->replica_list_node.next = NULL;
		}
		pte_free(mm, new[nid]);
	}
}

static void
sync_replicated_p4d_tables(int allocated_node, p4d_t **new, pgd_t *start_pgd, struct mm_struct *mm)
{
	int nid;
	unsigned long offset;
	struct page *curr;
	pgd_t *curr_pgd;
	bool start;

	for_each_pgtable(curr, curr_pgd, start_pgd, nid, offset, start) {
		SetPageReplicated(virt_to_page(new[allocated_node]));

		memcg_account_replicated_p4d_page(mm, new[nid]);
		if (nid == allocated_node)
			continue;
		if (allocated_node != NUMA_NO_NODE)
			copy_page(new[nid], new[allocated_node]);

		SetPageReplicated(virt_to_page(new[nid]));

		smp_wmb();
		pgd_populate(mm, curr_pgd, new[nid]);
	}
#ifndef __PAGETABLE_P4D_FOLDED
	account_replicated_table(mm);
#endif
}

static void
sync_replicated_pud_tables(int allocated_node, pud_t **new, p4d_t *start_p4d, struct mm_struct *mm)
{
	int nid;
	unsigned long offset;
	struct page *curr;
	p4d_t *curr_p4d;
	bool start;
	/*
	 * Do not need locking from sync_replicated_pte_tables,
	 * because pud_lockptr == page_table_lock
	 */
	build_pud_chain(new);
	set_master_page_for_puds(allocated_node, new);
	for_each_pgtable(curr, curr_p4d, start_p4d, nid, offset, start) {
		SetPageReplicated(virt_to_page(new[nid]));
		memcg_account_replicated_pud_page(mm, new[nid]);
		if (nid == allocated_node)
			continue;
		if (allocated_node != NUMA_NO_NODE)
			copy_page(new[nid], new[allocated_node]);

		mm_inc_nr_puds(mm);
		smp_wmb();
		p4d_populate(mm, curr_p4d, new[nid]);
	}
	account_replicated_table(mm);
}

static void
sync_replicated_pmd_tables(int allocated_node, pmd_t **new, pud_t *start_pud, struct mm_struct *mm)
{
	int nid;
	unsigned long offset;
	struct page *curr;
	pud_t *curr_pud;
	bool start;
	/*
	 * Locking here the same as in the sync_replicated_pte_tables
	 */
	spinlock_t *ptl = NULL;

	if (allocated_node != NUMA_NO_NODE) {
		ptl = pmd_lockptr(mm, new[allocated_node]);
		spin_lock_nested(ptl, 1);
	}

	BUILD_BUG_ON(!USE_SPLIT_PMD_PTLOCKS);

	build_pmd_chain(new);
	set_master_page_for_pmds(allocated_node, new);
	for_each_pgtable(curr, curr_pud, start_pud, nid, offset, start) {
		SetPageReplicated(virt_to_page(new[nid]));
		memcg_account_replicated_pmd_page(mm, new[nid]);
		if (nid == allocated_node)
			continue;
		if (allocated_node != NUMA_NO_NODE)
			copy_page(new[nid], new[allocated_node]);

		mm_inc_nr_pmds(mm);
		smp_wmb();
		pud_populate(mm, curr_pud, new[nid]);

	}
	account_replicated_table(mm);

	if (ptl)
		spin_unlock(ptl);
}

#ifdef CONFIG_ARM64
static void
sync_replicated_pte_tables(int allocated_node, struct page **new, pmd_t *start_pmd, struct mm_struct *mm)
{
	int nid;
	unsigned long offset;
	struct page *curr;
	pmd_t *curr_pmd;
	bool start;
	spinlock_t *ptl = NULL;

	/* Why we need (sometimes) ptl from allocated_node here?
	 * If replicate existed table, concurrent page fault might
	 * observe replicated table which content was not copied
	 * from original table yet. At this point master_locks are
	 * already set (which is lock from original table), so we
	 * need to hold it here.
	 *
	 * Obviously, if there was no any table before,
	 * we do not need to hold any pte lock at all, everything will be propagated
	 * correctly via replica_list
	 */
	BUILD_BUG_ON(!USE_SPLIT_PTE_PTLOCKS);

	if (allocated_node != NUMA_NO_NODE) {
		ptl = ptlock_ptr(new[allocated_node]);
		spin_lock_nested(ptl, 1);

		build_pte_chain(new);
		set_master_page_for_ptes(allocated_node, new);

		for_each_memory_node(nid) {
			SetPageReplicated(new[nid]);
			if (nid == allocated_node)
				continue;
			copy_page(page_to_virt(new[nid]), page_to_virt(new[allocated_node]));
		}

	}

	smp_wmb();

	for_each_pgtable(curr, curr_pmd, start_pmd, nid, offset, start) {
		/*
		 * We are safe to set this flag here even for original table,
		 * because replica list have already been created.
		 * So, in the case if some propagation will be required,
		 * we are able to do it, even if not all upper tables are populated yet
		 */
		memcg_account_replicated_pte_page(mm, page_to_virt(new[nid]));
		if (nid == allocated_node)
			continue;

		mm_inc_nr_ptes(mm);

		WRITE_ONCE(*curr_pmd, __pmd(__phys_to_pmd_val(page_to_phys(new[nid])) | PMD_TYPE_TABLE));
	}

	dsb(ishst);
	isb();
	account_replicated_table(mm);

	if (ptl)
		spin_unlock(ptl);
}
#endif

static int
prepare_replicated_p4d_tables(int allocated_node, p4d_t **new, struct mm_struct *mm, unsigned long address)
{
	int nid;
	p4d_t *new_p4d;
	bool fail = false;

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		new_p4d = p4d_alloc_one_node(nid, mm, address);

		if (unlikely(!new_p4d))
			fail = true;

		new[nid] = new_p4d;
	}

	if (unlikely(fail)) {
		release_replicated_p4d_tables(allocated_node, new, mm);
		return -ENOMEM;
	}

	return 0;
}

static int
prepare_replicated_pud_tables(int allocated_node, pud_t **new, struct mm_struct *mm, unsigned long address)
{
	int nid;
	pud_t *new_pud;
	bool fail = false;

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		new_pud = pud_alloc_one_node(nid, mm, address);

		if (unlikely(!new_pud))
			fail = true;

		new[nid] = new_pud;
	}

	if (unlikely(fail)) {
		release_replicated_pud_tables(allocated_node, new, mm);
		return -ENOMEM;
	}

	return 0;
}

static int
prepare_replicated_pmd_tables(int allocated_node, pmd_t **new, struct mm_struct *mm, unsigned long address)
{
	int nid;
	pmd_t *new_pmd;
	bool fail = false;

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		new_pmd = pmd_alloc_one_node(nid, mm, address);

		if (unlikely(!new_pmd))
			fail = true;

		new[nid] = new_pmd;
	}

	if (unlikely(fail)) {
		release_replicated_pmd_tables(allocated_node, new, mm);
		return -ENOMEM;
	}

	return 0;
}

static int
prepare_replicated_pte_tables(int allocated_node, struct page **new, struct mm_struct *mm)
{
	int nid;
	struct page *new_pte;
	bool fail = false;

	for_each_memory_node(nid) {
		if (nid == allocated_node)
			continue;
		new_pte = pte_alloc_one_node(nid, mm);

		if (unlikely(!new_pte))
			fail = true;

		new[nid] = new_pte;
	}

	if (unlikely(fail)) {
		release_replicated_pte_tables(allocated_node, new, mm);
		return -ENOMEM;
	}

	if (allocated_node == NUMA_NO_NODE) {
		build_pte_chain(new);
		set_master_page_for_ptes(allocated_node, new);

		for_each_memory_node(nid) {
			SetPageReplicated(new[nid]);
		}
	}

	return 0;
}

vm_fault_t replication_handle_pgd_fault(struct vm_fault *vmf)
{
	unsigned long address = vmf->real_address;
	struct mm_struct *mm = vmf->vma->vm_mm;

	vmf->pgd = pgd_offset_pgd(mm->pgd, address);

	return 0;
}

/* TODO Need to clarify, how this going to work with and without 5-level paging*/
static vm_fault_t replication_handle_p4d_fault(struct vm_fault *vmf)
{
	int ret;
	p4d_t *p4d_tables[MAX_NUMNODES];
	unsigned long address = vmf->real_address;
	struct mm_struct *mm = vmf->vma->vm_mm;

retry:

	/* See replication_handle_pgd_fault in mm/numa_replication.c */
	if (replicated_p4d_level(vmf)) {
		if (!pgd_none(*vmf->pgd)) {
			vmf->p4d = p4d_offset(vmf->pgd, address);
			return 0;
		}
		ret = prepare_replicated_p4d_tables(NUMA_NO_NODE, p4d_tables, mm, address);
		if (ret)
			goto fault_oom;

		spin_lock(&mm->page_table_lock);
		if (pgd_present(*vmf->pgd)) {
			/* Someone else has replicated this level */
			release_replicated_p4d_tables(NUMA_NO_NODE, p4d_tables, mm);
			if (!PageReplicated(virt_to_page(pgd_page_vaddr(*(vmf->pgd))))) {
				spin_unlock(&mm->page_table_lock);
				goto retry;
			}
		} else
			sync_replicated_p4d_tables(NUMA_NO_NODE, p4d_tables, vmf->pgd, mm);
		spin_unlock(&mm->page_table_lock);

	} else {
		p4d_t *table_page = (p4d_t *)pgd_page_vaddr(*(vmf->pgd));
		int p4d_node = page_to_nid(virt_to_page(table_page));

		p4d_tables[p4d_node] = table_page;
		ret = prepare_replicated_p4d_tables(p4d_node, p4d_tables, mm, address);
		if (ret)
			goto fault_oom;

		spin_lock(&mm->page_table_lock);
		if (PageReplicated(virt_to_page(table_page)))
			/* Someone else has replicated this level */
			release_replicated_p4d_tables(p4d_node, p4d_tables, mm);
		else
			sync_replicated_p4d_tables(p4d_node, p4d_tables, vmf->pgd, mm);
		spin_unlock(&mm->page_table_lock);
	}

	vmf->p4d = p4d_offset(vmf->pgd, address);

	return 0;

fault_oom:
	vmf->replica_action = REPLICA_FAIL;
	return VM_FAULT_OOM;
}

static vm_fault_t replication_handle_pud_fault(struct vm_fault *vmf)
{
	int ret;
	pud_t *pud_tables[MAX_NUMNODES];
	unsigned long address = vmf->real_address;
	struct mm_struct *mm = vmf->vma->vm_mm;

retry:

	/* See replication_handle_pgd_fault in mm/numa_replication.c */
	if (replicated_pud_level(vmf)) {
		if (!p4d_none(*vmf->p4d)) {
			vmf->pud = pud_offset(vmf->p4d, address);
			return 0;
		}
		ret = prepare_replicated_pud_tables(NUMA_NO_NODE, pud_tables, mm, address);
		if (ret)
			goto fault_oom;

		spin_lock(&mm->page_table_lock);
		if (p4d_present(*vmf->p4d)) {
			/* Someone else has replicated this level */
			release_replicated_pud_tables(NUMA_NO_NODE, pud_tables, mm);
			/* Concurrent normal fault and replicated (for example hugetlbfs fault for now or spurious on 6.6)*/
			if (!PageReplicated(virt_to_page(p4d_pgtable(*(vmf->p4d))))) {
				spin_unlock(&mm->page_table_lock);
				goto retry;
			}
		} else
			sync_replicated_pud_tables(NUMA_NO_NODE, pud_tables, vmf->p4d, mm);
		spin_unlock(&mm->page_table_lock);
	} else {
		pud_t *table_page = p4d_pgtable(*(vmf->p4d));
		int pud_node = page_to_nid(virt_to_page(table_page));

		pud_tables[pud_node] = table_page;
		ret = prepare_replicated_pud_tables(pud_node, pud_tables, mm, address);
		if (ret)
			goto fault_oom;

		spin_lock(&mm->page_table_lock);
		if (PageReplicated(virt_to_page(table_page)))
			/* Someone else has replicated this level */
			release_replicated_pud_tables(pud_node, pud_tables, mm);
		else
			sync_replicated_pud_tables(pud_node, pud_tables, vmf->p4d, mm);
		spin_unlock(&mm->page_table_lock);
	}

	vmf->pud = pud_offset(vmf->p4d, address);

	return 0;

fault_oom:
	vmf->replica_action = REPLICA_FAIL;
	return VM_FAULT_OOM;
}

static vm_fault_t replication_handle_pmd_fault(struct vm_fault *vmf)
{
	int ret;
	pmd_t *pmd_tables[MAX_NUMNODES];
	unsigned long address = vmf->real_address;
	struct mm_struct *mm = vmf->vma->vm_mm;
	spinlock_t *ptl;

retry:
	/* See replication_handle_pgd_fault in mm/numa_replication.c */
	if (replicated_pmd_level(vmf)) {
		if (!pud_none(*vmf->pud)) {
			vmf->pmd = pmd_offset(vmf->pud, address);
			return 0;
		}
		ret = prepare_replicated_pmd_tables(NUMA_NO_NODE, pmd_tables, mm, address);
		if (ret)
			goto fault_oom;

		ptl = pud_lock(mm, vmf->pud);
		if (pud_present(*vmf->pud)) {
			/* Someone else has replicated this level */
			release_replicated_pmd_tables(NUMA_NO_NODE, pmd_tables, mm);
			if (!PageReplicated(virt_to_page(pud_pgtable(*(vmf->pud))))) {
				spin_unlock(ptl);
				goto retry;
			}

		} else
			sync_replicated_pmd_tables(NUMA_NO_NODE, pmd_tables, vmf->pud, mm);
		spin_unlock(ptl);
	} else {
		pmd_t *table_page = pud_pgtable(*(vmf->pud));
		int pmd_node = page_to_nid(virt_to_page(table_page));

		pmd_tables[pmd_node] = table_page;
		ret = prepare_replicated_pmd_tables(pmd_node, pmd_tables, mm, address);
		if (ret)
			goto fault_oom;

		ptl = pud_lock(mm, vmf->pud);
		if (PageReplicated(virt_to_page(table_page)))
			/* Someone else has replicated this level */
			release_replicated_pmd_tables(pmd_node, pmd_tables, mm);
		else
			sync_replicated_pmd_tables(pmd_node, pmd_tables, vmf->pud, mm);
		spin_unlock(ptl);
	}

	vmf->pmd = pmd_offset(vmf->pud, address);

	return 0;

fault_oom:
	vmf->replica_action = REPLICA_FAIL;
	return VM_FAULT_OOM;
}

static vm_fault_t replication_handle_pte_fault(struct vm_fault *vmf)
{
	int ret;
	struct mm_struct *mm = vmf->vma->vm_mm;
	struct page *pte_tables[MAX_NUMNODES];
	spinlock_t *ptl;

retry:

	if (!pmd_none(*vmf->pmd) && pmd_devmap_trans_unstable(vmf->pmd))
		return 0;

	if (replicated_pte_level(vmf)) {
		/*
		 * If pmd from 0th node populated and PageReplciated flag is set,
		 * we don't care whether other nodes are populated or not,
		 * beacause pgtable lists are already built and we can use them
		 */
		if (!pmd_none(*vmf->pmd))
			return 0;
		ret = prepare_replicated_pte_tables(NUMA_NO_NODE, pte_tables, mm);
		if (ret)
			goto fault_oom;
		ptl = pmd_lock(mm, vmf->pmd);
		if (unlikely(pmd_present(*vmf->pmd))) {
			spin_unlock(ptl);
			/* Someone else has replicated this level */
			release_replicated_pte_tables(NUMA_NO_NODE, pte_tables, mm);
			if (!PageReplicated(pmd_pgtable(*(vmf->pmd)))) {
				goto retry;
			}
		} else {
			sync_replicated_pte_tables(NUMA_NO_NODE, pte_tables, vmf->pmd, mm);
			spin_unlock(ptl);
		}
	} else {
		struct page *table_page = pmd_pgtable(*(vmf->pmd));
		int pte_node = page_to_nid(table_page);

		pte_tables[pte_node] = table_page;
		ret = prepare_replicated_pte_tables(pte_node, pte_tables, mm);
		if (ret)
			goto fault_oom;

		ptl = pmd_lock(mm, vmf->pmd);
		if (unlikely(pmd_devmap_trans_unstable(vmf->pmd) || PageReplicated(table_page))) {
			spin_unlock(ptl);
			/* Someone else has replicated this level */
			release_replicated_pte_tables(pte_node, pte_tables, mm);
		} else {
			sync_replicated_pte_tables(pte_node, pte_tables, vmf->pmd, mm);
			spin_unlock(ptl);
		}
	}

	return 0;

fault_oom:
	vmf->replica_action = REPLICA_FAIL;
	return VM_FAULT_OOM;
}

pgd_t *fault_pgd_offset(struct vm_fault *vmf, unsigned long address)
{
	vmf->pgd = pgd_offset_pgd(this_node_pgd(vmf->vma->vm_mm), address);
	return vmf->pgd;
}

p4d_t *fault_p4d_alloc(struct vm_fault *vmf, struct mm_struct *mm, pgd_t *pgd, unsigned long address)
{
	if (replication_path_pgd(vmf)) {
		if (replication_handle_p4d_fault(vmf))
			return NULL;
	} else {
		vmf->p4d = p4d_alloc(mm, pgd, address);
	}
	return vmf->p4d;
}

pud_t *fault_pud_alloc(struct vm_fault *vmf, struct mm_struct *mm, p4d_t *p4d, unsigned long address)
{
	if (vmf->replica_action != REPLICA_NONE && replication_path_p4d(vmf)) {
		if (replication_handle_pud_fault(vmf))
			return NULL;
	} else {
		vmf->pud = pud_alloc(mm, p4d, address);
	}
	return vmf->pud;
}

pmd_t *fault_pmd_alloc(struct vm_fault *vmf, struct mm_struct *mm, pud_t *pud, unsigned long address)
{
	if (vmf->replica_action != REPLICA_NONE && replication_path_pud(vmf)) {
		if (replication_handle_pmd_fault(vmf))
			return NULL;
	} else {
		vmf->pmd = pmd_alloc(mm, pud, address);
	}
	return vmf->pmd;
}

int fault_pte_alloc(struct vm_fault *vmf)
{
	if (vmf->replica_action != REPLICA_NONE && replication_path_pmd(vmf))
		return replication_handle_pte_fault(vmf);
	return 0;
}

pte_t *cpr_alloc_pte_map(struct mm_struct *mm, unsigned long addr,
			 pmd_t *src_pmd, pmd_t *dst_pmd)
{
	struct page *pte_tables[MAX_NUMNODES];
	struct page *src_pte = pmd_pgtable(*src_pmd);
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	bool pte_replicated = numa_pgtable_replicated(page_to_virt(src_pte));
	bool pmd_replicated_dst = numa_pgtable_replicated(dst_pmd);

	if (pte_replicated && pmd_replicated_dst) {
		if (!pmd_none(*dst_pmd)) {
			return pte_offset_map(dst_pmd, addr);
		}
		if (prepare_replicated_pte_tables(NUMA_NO_NODE, pte_tables, mm))
			return NULL;

		spin_lock(&mm->page_table_lock);
		sync_replicated_pte_tables(NUMA_NO_NODE, pte_tables, dst_pmd, mm);
		spin_unlock(&mm->page_table_lock);

		return pte_offset_map(dst_pmd, addr);

	} else {
		return pte_alloc_map(mm, dst_pmd, addr);
	}
}

pte_t *cpr_alloc_pte_map_lock(struct mm_struct *mm, unsigned long addr,
				     pmd_t *src_pmd, pmd_t *dst_pmd, spinlock_t **ptl)
{
	struct page *pte_tables[MAX_NUMNODES];
	struct page *src_pte = pmd_pgtable(*src_pmd);
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	bool pte_replicated_src = numa_pgtable_replicated(page_to_virt(src_pte));
	bool pmd_replicated_dst = numa_pgtable_replicated(dst_pmd);

	if (pte_replicated_src && pmd_replicated_dst) {
		if (!pmd_none(*dst_pmd)) {
			return pte_offset_map_lock(mm, dst_pmd, addr, ptl);
		}
		if (prepare_replicated_pte_tables(NUMA_NO_NODE, pte_tables, mm))
			return NULL;

		spin_lock(&mm->page_table_lock);
		sync_replicated_pte_tables(NUMA_NO_NODE, pte_tables, dst_pmd, mm);
		spin_unlock(&mm->page_table_lock);

		return pte_offset_map_lock(mm, dst_pmd, addr, ptl);

	} else {
		return pte_alloc_map_lock(mm, dst_pmd, addr, ptl);
	}
}

pmd_t *cpr_alloc_pmd(struct mm_struct *mm, unsigned long addr,
			    pud_t *src_pud, pud_t *dst_pud)
{
	pmd_t *pmd_tables[MAX_NUMNODES];
	pmd_t *src_pmd = pud_pgtable(*src_pud);
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	bool pmd_replicated_src = numa_pgtable_replicated(src_pmd);
	bool pud_replicated_dst = numa_pgtable_replicated(dst_pud);

	if (pmd_replicated_src && pud_replicated_dst) {
		if (!pud_none(*dst_pud)) {
			return pmd_offset(dst_pud, addr);
		}
		if (prepare_replicated_pmd_tables(NUMA_NO_NODE, pmd_tables, mm, addr))
			return NULL;

		spin_lock(&mm->page_table_lock);
		sync_replicated_pmd_tables(NUMA_NO_NODE, pmd_tables, dst_pud, mm);
		spin_unlock(&mm->page_table_lock);

		return pmd_offset(dst_pud, addr);

	} else {
		return pmd_alloc(mm, dst_pud, addr);
	}
}

pud_t *cpr_alloc_pud(struct mm_struct *mm, unsigned long addr,
			    p4d_t *src_p4d, p4d_t *dst_p4d)
{
	pud_t *pud_tables[MAX_NUMNODES];
	pud_t *src_pud = p4d_pgtable(*src_p4d);
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	bool pud_replicated_src = numa_pgtable_replicated(src_pud);
	bool p4d_replicated_dst = numa_pgtable_replicated(dst_p4d);

	if (pud_replicated_src && p4d_replicated_dst) {
		if (!p4d_none(*dst_p4d)) {
			return pud_offset(dst_p4d, addr);
		}
		if (prepare_replicated_pud_tables(NUMA_NO_NODE, pud_tables, mm, addr))
			return NULL;

		spin_lock(&mm->page_table_lock);
		sync_replicated_pud_tables(NUMA_NO_NODE, pud_tables, dst_p4d, mm);
		spin_unlock(&mm->page_table_lock);

		return pud_offset(dst_p4d, addr);

	} else {
		return pud_alloc(mm, dst_p4d, addr);
	}
}

p4d_t *cpr_alloc_p4d(struct mm_struct *mm, unsigned long addr,
			    pgd_t *src_pgd, pgd_t *dst_pgd)
{
#if CONFIG_PGTABLE_LEVELS == 5
	p4d_t *p4d_tables[MAX_NUMNODES];
	p4d_t *src_p4d = pgd_pgtable(*src_pgd);
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	bool p4d_replicated_src = numa_pgtable_replicated(src_p4d);

	if (p4d_replicated_src) {
		if (!pgd_none(*dst_pgd)) {
			return p4d_offset(dst_pgd, addr);
		}
		if (prepare_replicated_p4d_tables(NUMA_NO_NODE, p4d_tables, mm, addr))
			return NULL;

		spin_lock(&mm->page_table_lock);
		sync_replicated_p4d_tables(NUMA_NO_NODE, p4d_tables, dst_pgd, mm);
		spin_unlock(&mm->page_table_lock);

		return p4d_offset(dst_pgd, addr);

	} else {
		return p4d_alloc(mm, dst_pgd, addr);
	}
#else
	return p4d_offset(dst_pgd, addr);
#endif
}

static pte_t *fault_pte_alloc_map(struct vm_fault *vmf, struct mm_struct *mm, pmd_t *pmd, unsigned long address)
{
	if (vmf->replica_action != REPLICA_NONE && replication_path_pmd(vmf)) {
		if (!replication_handle_pte_fault(vmf))
			return pte_offset_map(pmd, address);
		else
			return NULL;
	} else {
		return pte_alloc_map(mm, pmd, address);
	}
}

pte_t *huge_pte_alloc_replica(struct mm_struct *mm, struct vm_area_struct *vma,
			unsigned long addr, unsigned long sz)
{
	struct vm_fault vmf = {
		.vma = vma,
		.address = addr & PAGE_MASK,
		.real_address = addr,
		.pte = NULL
	};

	vmf.pgd = fault_pgd_offset(&vmf, addr);
	vmf.p4d = fault_p4d_alloc(&vmf, mm, vmf.pgd, addr);
	vmf.pud = fault_pud_alloc(&vmf, mm, vmf.p4d, addr);
	if (!vmf.pud)
		return NULL;

	if (sz == PUD_SIZE) {
		vmf.pte = (pte_t *)vmf.pud;
	} else if (sz == (CONT_PTE_SIZE)) {
		vmf.pmd = fault_pmd_alloc(&vmf, mm, vmf.pud, addr);
		if (!vmf.pmd)
			return NULL;

		WARN_ON(addr & (sz - 1));
		/*
		 * Note that if this code were ever ported to the
		 * 32-bit arm platform then it will cause trouble in
		 * the case where CONFIG_HIGHPTE is set, since there
		 * will be no pte_unmap() to correspond with this
		 * pte_alloc_map().
		 */
		vmf.pte = fault_pte_alloc_map(&vmf, mm, vmf.pmd, addr);
	} else if (sz == PMD_SIZE) {
		if (IS_ENABLED(CONFIG_ARCH_WANT_HUGE_PMD_SHARE) &&
		    pud_none(READ_ONCE(*vmf.pud)) && !numa_is_vma_replicant(vma))
			vmf.pte = huge_pmd_share(mm, addr, vmf.pud);
		else
			vmf.pte = (pte_t *)fault_pmd_alloc(&vmf, mm, vmf.pud, addr);
	} else if (sz == (CONT_PMD_SIZE)) {
		vmf.pmd = fault_pmd_alloc(&vmf, mm, vmf.pud, addr);
		WARN_ON(addr & (sz - 1));
		return (pte_t *)vmf.pmd;
	}

	return vmf.pte;
}

static unsigned long squared_norm(unsigned long *numa_vec)
{
	int nid;
	unsigned long result = 0;

	for_each_memory_node(nid)
		result += (numa_vec[nid] * numa_vec[nid]);
	return result;
}

static unsigned long numa_calculate_uniformity_value(struct numa_context_switch_stat *stats)
{
	unsigned long dot = 0;
	unsigned long squared_norm_val = 0;
	unsigned long sqrt = 0;
	int nid;

	for_each_memory_node(nid) {
		dot += stats->total_stats[nid];
	}

	squared_norm_val = squared_norm(stats->total_stats);

	if (!squared_norm_val)
		return 0;

	sqrt = int_sqrt(squared_norm_val * replica_count);

	return (dot * 1000UL) / sqrt;
}

void free_numa_replication_ctl(struct mm_struct *mm)
{
	if (!mm->replication_ctl)
		return;

	if (mm->replication_ctl->in_candidate_list) {
		spin_lock(&replication_candidates_lock);

		list_del(&mm->replication_ctl->replication_candidates);

		spin_unlock(&replication_candidates_lock);
	}

	free_percpu(mm->replication_ctl->pcp_dereplicated_tables);
	free_percpu(mm->replication_ctl->pcp_replicated_tables);
	free_percpu(mm->replication_ctl->pcp_dereplicated_pages);
	free_percpu(mm->replication_ctl->pcp_replicated_pages);

	kfree(mm->replication_ctl);
	mm->replication_ctl = NULL;
}

static ssize_t show_candidates(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	ssize_t total = 0;
	struct numa_replication_control *ctl;

	spin_lock(&replication_candidates_lock);
	list_for_each_entry(ctl, &replication_candidates, replication_candidates) {
		struct task_struct *tsk;

		rcu_read_lock();
		tsk = rcu_dereference(ctl->owner->owner);
		if (!tsk) {
			rcu_read_unlock();
			continue;
		}

		total += sprintf(buf + total, "%d\n", tsk->pid);

		rcu_read_unlock();
	}
	spin_unlock(&replication_candidates_lock);

	return total;
}

static struct kobj_attribute candidates_attr =
	__ATTR(candidates, 0444, show_candidates, NULL);

static struct attribute *numa_replication_attr[] = {
	&candidates_attr.attr,
	NULL,
};

static const struct attribute_group numa_replication_attr_group = {
	.attrs = numa_replication_attr,
};

int numa_replication_init_sysfs(void)
{
	int err;
	struct kobject *kobj = kobject_create_and_add("numa_replication", mm_kobj);

	if (unlikely(!kobj)) {
		pr_err("failed to create numa_replication kobject\n");
		return -ENOMEM;
	}

	err = sysfs_create_group(kobj, &numa_replication_attr_group);
	if (err) {
		pr_err("failed to register numa_replication group\n");
		goto delete_obj;
	}

	return 0;

delete_obj:
	kobject_put(kobj);
	return err;
}



void numa_replication_add_candidate(struct mm_struct *mm)
{
	if (mm->replication_ctl->in_candidate_list)
		return;

	spin_lock(&mm->replication_ctl->lock);

	/* We are already in this list */
	if (mm->replication_ctl->in_candidate_list)
		goto out;

	spin_lock_nested(&replication_candidates_lock, SINGLE_DEPTH_NESTING);

	list_add(&mm->replication_ctl->replication_candidates, &replication_candidates);

	spin_unlock(&replication_candidates_lock);

	mm->replication_ctl->in_candidate_list = true;
out:
	spin_unlock(&mm->replication_ctl->lock);
}


void numa_account_switch(struct mm_struct *mm)
{
	if (!mm->context_switch_stats)
		return;
	this_cpu_inc(*(mm->context_switch_stats->pcp_stats));
}


void numa_accumulate_switches(struct mm_struct *mm)
{
	int cpu;
	int nid;
	unsigned long numa_val;

	if (!mm->context_switch_stats)
		return;

	/*
	 * Replication is enabled, we do not need to do anything (due to this piece of work or cgroup)
	 * In case of races we only perform redundant calculations of replication score  - not a big deal
	 */
	if (get_user_replication_policy(mm))
		return;
	if (mm->replication_ctl->in_candidate_list)
		return;

	spin_lock(&(mm->context_switch_stats->lock));

	for_each_possible_cpu(cpu) {
		unsigned long *ptr = per_cpu_ptr(mm->context_switch_stats->pcp_stats, cpu);

		mm->context_switch_stats->last_stats[cpu_to_node(cpu)] += *ptr;
		*ptr = 0;
	}

	for_each_memory_node(nid) {
		mm->context_switch_stats->total_stats[nid] = (mm->context_switch_stats->total_stats[nid] * 7) / 10 +
							      mm->context_switch_stats->last_stats[nid];
		mm->context_switch_stats->last_stats[nid] = 0;
	}

	numa_val = numa_calculate_uniformity_value(mm->context_switch_stats);

	spin_unlock(&(mm->context_switch_stats->lock));

	/*
	 * 960 is a magic number.
	 * Tunable and we need to revaluate it more carefully
	 */
	if (numa_val > 960) {
		pr_info("%d New candidate for pid: %d, comm: %s, replication score: %lu\n", current->pid, mm->owner->pid, mm->owner->comm, numa_val);

		numa_replication_add_candidate(mm);
	}
}

static int replicate_alloc_pte(struct mm_struct *mm, unsigned long addr, pmd_t *pmd)
{
	struct page *pte_tables[MAX_NUMNODES];
	struct page *pte = pmd_pgtable(*pmd);
	spinlock_t *ptl;
	int nid = page_to_nid(pte);

	pte_tables[nid] = pte;
	/*
	 * Because tt structure of src and dst mm must be the same,
	 * it doesn't matter which pgtable check for being replicated
	 */
	if (numa_pgtable_replicated(page_to_virt(pte))) {
		return 0;
	}

	if (prepare_replicated_pte_tables(nid, pte_tables, mm))
		return -ENOMEM;

	ptl = pmd_lock(mm, pmd);
	sync_replicated_pte_tables(nid, pte_tables, pmd, mm);
	spin_unlock(ptl);

	return 0;
}

static unsigned long numa_replicate_pgtables_pmd_range(struct vm_area_struct *vma, pud_t *pud,
					      unsigned long addr, unsigned long end)
{
	pmd_t *pmd;
	unsigned long next, last = addr;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);

		if (pmd_none(*pmd) || is_swap_pmd(*pmd) || pmd_devmap(*pmd) || pmd_trans_huge(*pmd))
			continue;

		if (replicate_alloc_pte(vma->vm_mm, addr, pmd))
			break;

		cond_resched();
	} while (pmd++, last = next, addr = next, addr != end);

	return last;
}

static int replicate_alloc_pmd(struct mm_struct *mm, unsigned long addr, pud_t *pud)
{
	pmd_t *pmd_tables[MAX_NUMNODES];
	pmd_t *pmd = pud_pgtable(*pud);
	spinlock_t *ptl;
	int nid = page_to_nid(virt_to_page(pmd));

	pmd_tables[nid] = pmd;

	if (numa_pgtable_replicated(pmd))
		return 0;

	if (prepare_replicated_pmd_tables(nid, pmd_tables, mm, addr))
		return -ENOMEM;

	ptl = pud_lock(mm, pud);
	sync_replicated_pmd_tables(nid, pmd_tables, pud, mm);
	spin_unlock(ptl);

	return 0;

}

static unsigned long numa_replicate_pgtables_pud_range(struct vm_area_struct *vma, p4d_t *p4d,
					      unsigned long addr, unsigned long end)
{
	pud_t *pud;
	unsigned long next, last = addr;

	pud = pud_offset(p4d, addr);
	do {
		next = pud_addr_end(addr, end);

		if (pud_none_or_clear_bad(pud))
			continue;

		if (replicate_alloc_pmd(vma->vm_mm, addr, pud))
			break;

		last = numa_replicate_pgtables_pmd_range(vma, pud, addr, next);

		if (last != next)
			break;

	} while (pud++, last = next, addr = next, addr != end);

	return last;
}

static int replicate_alloc_pud(struct mm_struct *mm, unsigned long addr, p4d_t *p4d)
{
	pud_t *pud_tables[MAX_NUMNODES];
	pud_t *pud = p4d_pgtable(*p4d);
	int nid = page_to_nid(virt_to_page(pud));

	pud_tables[nid] = pud;

	if (numa_pgtable_replicated(pud))
		return 0;

	if (prepare_replicated_pud_tables(nid, pud_tables, mm, addr))
		return -ENOMEM;

	spin_lock(&mm->page_table_lock);
	sync_replicated_pud_tables(nid, pud_tables, p4d, mm);
	spin_unlock(&mm->page_table_lock);

	return 0;

}

static unsigned long numa_replicate_pgtables_p4d_range(struct vm_area_struct *vma, pgd_t *pgd,
					      unsigned long addr, unsigned long end)
{
	p4d_t *p4d;
	unsigned long next, last = addr;

	p4d = p4d_offset(pgd, addr);


	do {
		next = p4d_addr_end(addr, end);

		if (p4d_none_or_clear_bad(p4d))
			continue;

		if (replicate_alloc_pud(vma->vm_mm, addr, p4d))
			break;

		last = numa_replicate_pgtables_pud_range(vma, p4d, addr, next);

		if (last != next)
			break;

	} while (p4d++, last = next, addr = next, addr != end);

	return last;
}

static int replicate_alloc_p4d(struct mm_struct *mm, unsigned long addr, pgd_t *pgd)
{
#if CONFIG_PGTABLE_LEVELS == 5
	p4d_t *p4d_tables[MAX_NUMNODES];
	p4d_t *p4d = pgd_pgtable(*pgd);
	int nid = page_to_nid(virt_to_page(p4d));

	p4d_tables[nid] = p4d;

	if (numa_pgtable_replicated(p4d))
		return 0;

	if (prepare_replicated_p4d_tables(nid, p4d_tables, mm, addr))
		return -ENOMEM;

	spin_lock(&mm->page_table_lock);
	sync_replicated_p4d_tables(nid, p4d_tables, pgd, mm);
	spin_unlock(&mm->page_table_lock);

#endif
	return 0;

}

static unsigned long numa_replicate_pgtables_pgd_range(struct vm_area_struct *vma, unsigned long addr, unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	unsigned long next;
	unsigned long last = addr;

	BUG_ON(addr >= end);

	pgd = pgd_offset_pgd(this_node_pgd(mm), addr);

	do {
		next = pgd_addr_end(addr, end);

		if (pgd_none_or_clear_bad(pgd))
			continue;

		if (replicate_alloc_p4d(vma->vm_mm, addr, pgd))
			break;

		last = numa_replicate_pgtables_p4d_range(vma, pgd, addr, next);

		if (last != next)
			break;

	} while (pgd++, last = next, addr = next, addr != end);

	return last;

}

int numa_replicate_pgtables_vma(struct vm_area_struct *vma)
{
	unsigned long last = numa_replicate_pgtables_pgd_range(vma, vma->vm_start, vma->vm_end);

	if (last != vma->vm_end)
		return -ENOMEM;

	flush_tlb_range(vma, vma->vm_start, vma->vm_end);

	return 0;

}

static void dereplicate_pgtables_pte_range(struct mmu_gather *tlb, pmd_t *pmd,
			   unsigned long addr)
{
	unsigned long offset;
	struct page *curr, *tmp;
	pmd_t *curr_pmd;
	pte_t *curr_pte;
	spinlock_t *lock;
	pgtable_t token = pmd_pgtable(*pmd)->master_table;
	bool pte_replicated = numa_pgtable_replicated(page_to_virt(token));

	if (!pte_replicated)
		return;

	lock = pmd_lock(tlb->mm, pmd);

	pmd_populate(tlb->mm, pmd, token);

	for_each_pgtable_replica(curr, curr_pmd, pmd, offset) {
		pmd_populate(tlb->mm, curr_pmd, token);
	}

	spin_unlock(lock);

	for_each_pgtable_replica_safe(curr, tmp, curr_pte, page_to_virt(token), offset) {
		memcg_account_dereplicated_pgtable_page(curr_pte);
		cleanup_pte_list(curr);
		pte_free_tlb(tlb, curr, addr);
		mm_dec_nr_ptes(tlb->mm);
	}
	memcg_account_dereplicated_pgtable_page(page_to_virt(token));
	account_dereplicated_table(tlb->mm);
	cleanup_pte_list(token);
	ClearPageReplicated(token);
}

static inline void __free_pgtables_replica_pmd(struct mmu_gather *tlb, pud_t *pud,
				unsigned long addr)
{
	unsigned long offset;
	struct page *curr, *tmp;
	pud_t *curr_pud;
	pmd_t *curr_pmd;
	spinlock_t *lock;
	pmd_t *pmd = get_master_pmd(pmd_offset(pud, addr));

	lock = pud_lock(tlb->mm, pud);

	pud_populate(tlb->mm, pud, pmd);


	for_each_pgtable_replica(curr, curr_pud, pud, offset) {
		pud_populate(tlb->mm, curr_pud, pmd);
	}

	spin_unlock(lock);

	for_each_pgtable_replica_safe(curr, tmp, curr_pmd, pmd, offset) {
		memcg_account_dereplicated_pgtable_page(curr_pmd);
		cleanup_pmd_list(curr);
		pmd_free_tlb(tlb, curr_pmd, addr);
		mm_dec_nr_pmds(tlb->mm);
	}
	memcg_account_dereplicated_pgtable_page(pmd);
	account_dereplicated_table(tlb->mm);
	cleanup_pmd_list(virt_to_page(pmd));
	ClearPageReplicated(virt_to_page(pmd));

}

static inline void dereplicate_pgtables_pmd_range(struct mmu_gather *tlb, pud_t *pud,
				unsigned long addr, unsigned long end,
				unsigned long floor, unsigned long ceiling)
{
	pmd_t *pmd;
	unsigned long next;
	unsigned long start;

	start = addr;
	pmd = pmd_offset(pud, addr);

	if (!numa_pgtable_replicated(pmd))
		return;

	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none(*pmd) || is_swap_pmd(*pmd) || pmd_devmap(*pmd) || pmd_trans_huge(*pmd))
			continue;
		dereplicate_pgtables_pte_range(tlb, pmd, addr);
	} while (pmd++, addr = next, addr != end);

	start &= PUD_MASK;
	if (start < floor)
		return;
	if (ceiling) {
		ceiling &= PUD_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		return;

	__free_pgtables_replica_pmd(tlb, pud, start);
}

static inline void __free_pgtables_replica_pud(struct mmu_gather *tlb, p4d_t *p4d,
				unsigned long addr)
{
	unsigned long offset;
	struct page *curr, *tmp;
	p4d_t *curr_p4d;
	pud_t *curr_pud;
	pud_t *pud = get_master_pud(pud_offset(p4d, addr));

	spin_lock(&tlb->mm->page_table_lock);

	p4d_populate(tlb->mm, p4d, pud);

	for_each_pgtable_replica(curr, curr_p4d, p4d, offset) {
		p4d_populate(tlb->mm, curr_p4d, pud);
	}

	spin_unlock(&tlb->mm->page_table_lock);

	for_each_pgtable_replica_safe(curr, tmp, curr_pud, pud, offset) {
		memcg_account_dereplicated_pgtable_page(curr_pud);
		cleanup_pud_list(curr);
		pud_free_tlb(tlb, curr_pud, addr);
		mm_dec_nr_puds(tlb->mm);
	}
	memcg_account_dereplicated_pgtable_page(pud);
	account_dereplicated_table(tlb->mm);
	cleanup_pud_list(virt_to_page(pud));
	ClearPageReplicated(virt_to_page(pud));

}

static inline void dereplicate_pgtables_pud_range(struct mmu_gather *tlb, p4d_t *p4d,
				unsigned long addr, unsigned long end,
				unsigned long floor, unsigned long ceiling)
{
	pud_t *pud;
	unsigned long next;
	unsigned long start;

	start = addr;
	pud = pud_offset(p4d, addr);

	if (!numa_pgtable_replicated(pud))
		return;

	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		dereplicate_pgtables_pmd_range(tlb, pud, addr, next, floor, ceiling);
	} while (pud++, addr = next, addr != end);

	start &= P4D_MASK;
	if (start < floor)
		return;
	if (ceiling) {
		ceiling &= P4D_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		return;

	__free_pgtables_replica_pud(tlb, p4d, start);
}

static inline void dereplicate_pgtables_p4d_range(struct mmu_gather *tlb, pgd_t *pgd,
				unsigned long addr, unsigned long end,
				unsigned long floor, unsigned long ceiling)
{
	p4d_t *p4d;
	unsigned long next;
	unsigned long start;

	start = addr;
	p4d = p4d_offset(pgd, addr);

	if (!numa_pgtable_replicated(p4d))
		return;

	do {
		next = p4d_addr_end(addr, end);
		if (p4d_none_or_clear_bad(p4d))
			continue;
		dereplicate_pgtables_pud_range(tlb, p4d, addr, next, floor, ceiling);
	} while (p4d++, addr = next, addr != end);

	start &= PGDIR_MASK;
	if (start < floor)
		return;
	if (ceiling) {
		ceiling &= PGDIR_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		return;

	/* TODO
	__free_pgtables_replica_p4d(tlb, pgd, start);
	 */
}

/*
 * This function frees user-level page tables of a process.
 */
static inline void dereplicate_pgtables_pgd_range(struct mmu_gather *tlb,
			unsigned long addr, unsigned long end,
			unsigned long floor, unsigned long ceiling)
{
	pgd_t *pgd;
	unsigned long next;

	addr &= PMD_MASK;
	if (addr < floor) {
		addr += PMD_SIZE;
		if (!addr)
			return;
	}
	if (ceiling) {
		ceiling &= PMD_MASK;
		if (!ceiling)
			return;
	}
	if (end - 1 > ceiling - 1)
		end -= PMD_SIZE;
	if (addr > end - 1)
		return;

	tlb_change_page_size(tlb, PAGE_SIZE);
	pgd = pgd_offset(tlb->mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		dereplicate_pgtables_p4d_range(tlb, pgd, addr, next, floor, ceiling);
	} while (pgd++, addr = next, addr != end);
}

void dereplicate_pgtables(struct mm_struct *mm)
{
	struct mmu_gather tlb;
	struct vm_area_struct *vma;
	unsigned long start = 0;
	unsigned long end = mm->mmap_base;

	tlb_gather_mmu(&tlb, mm, start, end);

	down_write(&mm->replication_ctl->rmap_lock);

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		BUG_ON(vma_has_replicas(vma));
		vma->vm_flags &= ~(VM_REPLICA_INIT);
		dereplicate_pgtables_pgd_range(&tlb, vma->vm_start, vma->vm_end, FIRST_USER_ADDRESS,
					vma->vm_next ? vma->vm_next->vm_start : USER_PGTABLES_CEILING);
	}

	tlb_finish_mmu(&tlb, start, end);

	up_write(&mm->replication_ctl->rmap_lock);

}

static inline struct vm_area_struct *find_next_replicant_vma(struct vm_area_struct *vma)
{
	if (!vma)
		return NULL;
	vma = vma->vm_next;
	while (vma && !numa_is_vma_replicant(vma)) {
		vma = vma->vm_next;
	}
	return vma;
}

static inline struct vm_area_struct *find_first_replicant_vma(struct mm_struct *mm)
{
	struct vm_area_struct *vma = mm->mmap;

	while (vma && !numa_is_vma_replicant(vma)) {
		vma = vma->vm_next;
	}
	return vma;
}

void dereplicate_rw_pgtables(struct mm_struct *mm)
{
	struct mmu_gather tlb;
	struct vm_area_struct *vma, *prev, *next;
	unsigned long start = 0;
	unsigned long end = mm->mmap_base;

	tlb_gather_mmu(&tlb, mm, start, end);

	down_write(&mm->replication_ctl->rmap_lock);

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (!vma_replica_candidate(vma)) {
			BUG_ON(vma_has_replicas(vma));
			vma->vm_flags &= ~(VM_REPLICA_INIT);
		}
	}

	next = find_first_replicant_vma(mm);
	prev = NULL;

	do {
		dereplicate_pgtables_pgd_range(&tlb,
					prev ? prev->vm_end : FIRST_USER_ADDRESS,
					next ? next->vm_start : mm->mmap_base,
					prev ? prev->vm_end : FIRST_USER_ADDRESS,
					next ? next->vm_start : USER_PGTABLES_CEILING);

	} while (prev = next, next = find_next_replicant_vma(next), prev != NULL);


	tlb_finish_mmu(&tlb, start, end);

	up_write(&mm->replication_ctl->rmap_lock);
}

static int numa_mm_table_replication_none(struct mm_struct *mm)
{
	int ret = 0;

	mmap_write_lock(mm);

	switch (get_table_replication_policy(mm)) {
	case TABLE_REPLICATION_NONE: {
		goto out;
	}
	case TABLE_REPLICATION_MINIMAL:
	case TABLE_REPLICATION_ALL: {
		dereplicate_pgtables(mm);
		break;
	}
	default: {
		BUG();
	}
	}
	set_table_replication_policy(mm, TABLE_REPLICATION_NONE);
out:
	mmap_write_unlock(mm);
	return ret;
}

static int numa_mm_table_replication_minimal_from_all(struct mm_struct *mm)
{
	dereplicate_rw_pgtables(mm);
	return 0;
}

static int numa_mm_table_replication_minimal_from_none(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	int ret = 0;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_replica_candidate(vma)) {
			vma->vm_flags |= VM_REPLICA_INIT;

			ret = numa_replicate_pgtables_vma(vma);
			if (ret)
				return ret;
		}
	}

	numa_replication_remove_from_candidate_list(mm);
	return ret;
}

static int numa_mm_table_replication_minimal(struct mm_struct *mm)
{
	int ret = 0;

	mmap_write_lock(mm);

	switch (get_table_replication_policy(mm)) {
	case TABLE_REPLICATION_NONE: {
		ret = numa_mm_table_replication_minimal_from_none(mm);
		break;
	}
	case TABLE_REPLICATION_MINIMAL: {
		goto out;
	}
	case TABLE_REPLICATION_ALL: {
		ret = numa_mm_table_replication_minimal_from_all(mm);
		break;
	}
	default: {
		BUG();
	}
	}
	set_table_replication_policy(mm, TABLE_REPLICATION_MINIMAL);
out:
	mmap_write_unlock(mm);

	return ret;
}

static int numa_mm_table_replication_all(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_write_lock(mm);

	if (get_table_replication_policy(mm) == TABLE_REPLICATION_ALL) {
		goto out;
	}

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (!numa_is_vma_replicant(vma)) {
			vma->vm_flags |= VM_REPLICA_INIT;

			ret = numa_replicate_pgtables_vma(vma);
			if (ret)
				goto out;
		}
	}

	set_table_replication_policy(mm, TABLE_REPLICATION_ALL);
	numa_replication_remove_from_candidate_list(mm);
out:
	mmap_write_unlock(mm);

	return ret;
}

/* CMD:
 * 0 - no table replication
 * 1 - replicate minimal amount to support RO-data replication
 * 2 - replicate all tables
 *
 * Transitions between all states are supported,
 */
int numa_dispatch_table_replication_request(struct mm_struct *mm, int cmd)
{
	switch (cmd) {
	case TABLE_REPLICATION_NONE:
		return numa_mm_table_replication_none(mm);
	case TABLE_REPLICATION_MINIMAL:
		return numa_mm_table_replication_minimal(mm);
	case TABLE_REPLICATION_ALL:
		return numa_mm_table_replication_all(mm);
	default:
		return -EINVAL;
	}
}

static int numa_mm_disable_replication(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_write_lock(mm);

	if (get_data_replication_policy(mm) == DATA_REPLICATION_NONE) {
		goto out;
	}

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_has_replicas(vma)) {
			ret = phys_deduplicate(vma, vma->vm_start, vma->vm_end - vma->vm_start, true);
			if (ret)
				goto out;

			vma->vm_flags &= ~VM_REPLICA_COMMIT;
		}
	}

	set_data_replication_policy(mm, DATA_REPLICATION_NONE);
out:
	mmap_write_unlock(mm);

	return ret;

}

static int numa_mm_on_demand_data_replication(struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_write_lock(mm);

	if (get_table_replication_policy(mm) == TABLE_REPLICATION_NONE) {
		ret = -EINVAL;
		goto out;
	}

	if (get_data_replication_policy(mm) != DATA_REPLICATION_NONE) {
		goto out;
	}

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_might_be_replicated(vma)) {
			vma->vm_flags |= VM_REPLICA_COMMIT;
		}
	}

	set_data_replication_policy(mm, DATA_REPLICATION_ON_DEMAND);
out:
	mmap_write_unlock(mm);

	return ret;
}

static int numa_mm_all_data_replication(struct mm_struct *mm, data_replication_policy_t policy)
{
	struct vm_area_struct *vma;
	int ret = 0;

	mmap_write_lock(mm);

	if (get_table_replication_policy(mm) == TABLE_REPLICATION_NONE) {
		ret = -EINVAL;
		goto out;
	}

	if (get_data_replication_policy(mm) == policy)
		goto out;

	if (get_data_replication_policy(mm) == DATA_REPLICATION_ALL
				&& policy == DATA_REPLICATION_ALL_MAPPED_ON_DEMAND)
		goto out_set;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma_might_be_replicated(vma)) {
			vma->vm_flags |= VM_REPLICA_COMMIT;
			ret = phys_duplicate(vma, vma->vm_start, vma->vm_end - vma->vm_start);
			if (ret)
				break;
		}
	}

out_set:
	set_data_replication_policy(mm, policy);
out:
	mmap_write_unlock(mm);

	return ret;
}


/* CMD:
 * 0 - no data replicas at all
 * 1 - replicas are created on demand via numa balancer
 * 2 - all mapped ro-data will be replicated, new ro-data will be replicated on demand
 * 3 - all ro-data always replicated.
 *
 * All transitions are supported, however without rollback
 * (because it doesn't make any sense) and sometimes they won't do anything
 * meaningful (for example, 2 -> 1 doesn't make sense, but it will work anyway)
 * If we are starting application from cgroup, some options are the same
 * (for new process states 2 and 1 are identical)
 */
int numa_dispatch_data_replication_request(struct mm_struct *mm, int cmd)
{
	switch (cmd) {
	case DATA_REPLICATION_NONE:
		return numa_mm_disable_replication(mm);
	case DATA_REPLICATION_ON_DEMAND:
		return numa_mm_on_demand_data_replication(mm);
	case DATA_REPLICATION_ALL_MAPPED_ON_DEMAND:
		return numa_mm_all_data_replication(mm, DATA_REPLICATION_ALL_MAPPED_ON_DEMAND);
	case DATA_REPLICATION_ALL:
		return numa_mm_all_data_replication(mm, DATA_REPLICATION_ALL);
	default:
		return -EINVAL;
	}
}

void numa_replication_post_mprotect(struct vm_area_struct *vma)
{
	if (numa_is_vma_replicant(vma)) {
		numa_replicate_pgtables_vma(vma);
	}
	if (vma_might_be_replicated(vma) && get_data_replication_policy(vma->vm_mm) == DATA_REPLICATION_ALL) {
		phys_duplicate(vma, vma->vm_start, vma->vm_end - vma->vm_start);
	}

}
