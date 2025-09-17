// SPDX-License-Identifier: GPL-2.0-only
/*
 * GMEM physical memory management.
 *
 * Copyright (C) 2025- Huawei, Inc.
 * Author: Bin Wang
 *
 */

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>

#include <linux/gmem.h>
#include <linux/vm_object.h>

#define NUM_SWAP_PAGES		16
#define MAX_SWAP_RETRY_TIMES	10

static struct kmem_cache *gm_page_cachep;

DEFINE_SPINLOCK(hnode_lock);
struct hnode *hnodes[MAX_NUMNODES];

void __init hnuma_init(void)
{
	unsigned int node;

	spin_lock(&hnode_lock);
	for_each_node(node)
		node_set(node, hnode_map);
	spin_unlock(&hnode_lock);
}

unsigned int alloc_hnode_id(void)
{
	unsigned int node;

	node = first_unset_node(hnode_map);
	node_set(node, hnode_map);

	return node;
}

void free_hnode_id(unsigned int nid)
{
	node_clear(nid, hnode_map);
}

void hnode_init(struct hnode *hnode, unsigned int hnid, struct gm_dev *dev)
{
	hnode->id = hnid;
	hnode->dev = dev;
	INIT_LIST_HEAD(&hnode->freelist);
	INIT_LIST_HEAD(&hnode->activelist);
	spin_lock_init(&hnode->freelist_lock);
	spin_lock_init(&hnode->activelist_lock);
	atomic_set(&hnode->nr_free_pages, 0);
	atomic_set(&hnode->nr_active_pages, 0);
	hnode->import_failed = false;
	hnode->max_memsize = 0;

	node_set(hnid, dev->registered_hnodes);
	hnodes[hnid] = hnode;
}

void hnode_deinit(unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid]->id = 0;
	hnodes[hnid]->dev = NULL;
	node_clear(hnid, dev->registered_hnodes);
	hnodes[hnid] = NULL;
}

struct hnode *get_hnode(unsigned int hnid)
{
	if (!hnodes[hnid])
		gmem_err("h-NUMA node for hnode id %u is NULL.", hnid);
	return hnodes[hnid];
}

struct gm_dev *get_gm_dev(unsigned int nid)
{
	struct hnode *hnode;
	struct gm_dev *dev = NULL;

	spin_lock(&hnode_lock);
	hnode = get_hnode(nid);
	if (hnode)
		dev = hnode->dev;
	spin_unlock(&hnode_lock);
	return dev;
}

static void init_swapd(struct hnode *hnode);

int gm_dev_register_hnode(struct gm_dev *dev)
{
	unsigned int hnid;
	struct hnode *hnode = kmalloc(sizeof(struct hnode), GFP_KERNEL);
	int ret;

	if (!hnode)
		return -ENOMEM;

	spin_lock(&hnode_lock);
	hnid = alloc_hnode_id();
	spin_unlock(&hnode_lock);

	if (hnid == MAX_NUMNODES)
		goto free_hnode;

	ret = hnode_init_sysfs(hnid);
	if (ret)
		goto free_hnode;

	hnode_init(hnode, hnid, dev);
	init_swapd(hnode);

	return GM_RET_SUCCESS;

free_hnode:
	kfree(hnode);
	return -EBUSY;
}
EXPORT_SYMBOL_GPL(gm_dev_register_hnode);

int __init gm_page_cachep_init(void)
{
	gm_page_cachep = KMEM_CACHE(gm_page, 0);
	if (!gm_page_cachep)
		return -EINVAL;
	return 0;
}

void gm_page_cachep_destroy(void)
{
	kmem_cache_destroy(gm_page_cachep);
}

struct gm_page *alloc_gm_page_struct(void)
{
	struct gm_page *gm_page = kmem_cache_zalloc(gm_page_cachep, GFP_KERNEL);

	if (!gm_page)
		return NULL;
	atomic_set(&gm_page->refcount, 0);
	return gm_page;
}
EXPORT_SYMBOL(alloc_gm_page_struct);

void hnode_freelist_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->freelist_lock);
	list_add(&gm_page->gm_page_list, &hnode->freelist);
	spin_unlock(&hnode->freelist_lock);
}

void hnode_activelist_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	list_add_tail(&gm_page->gm_page_list, &hnode->activelist);
	spin_unlock(&hnode->activelist_lock);
}

void hnode_activelist_del(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	list_del(&gm_page->gm_page_list);
	spin_unlock(&hnode->activelist_lock);
}

void hnode_activelist_del_and_add(struct hnode *hnode, struct gm_page *gm_page)
{
	spin_lock(&hnode->activelist_lock);
	list_move_tail(&gm_page->gm_page_list, &hnode->activelist);
	spin_unlock(&hnode->activelist_lock);
}

void mark_gm_page_active(struct gm_page *gm_page)
{
	struct hnode *hnode = get_hnode(gm_page->hnid);

	if (!hnode)
		return;

	hnode_activelist_del_and_add(hnode, gm_page);
}

int gm_add_pages(unsigned int hnid, struct list_head *pages)
{
	struct hnode *hnode;
	struct gm_page *gm_page, *n;

	hnode = get_hnode(hnid);
	if (!hnode)
		return -EINVAL;

	list_for_each_entry_safe(gm_page, n, pages, gm_page_list) {
		list_del(&gm_page->gm_page_list);
		hnode_freelist_add(hnode, gm_page);
		hnode_free_pages_inc(hnode);
	}

	return 0;
}
EXPORT_SYMBOL(gm_add_pages);

void gm_free_page(struct gm_page *gm_page)
{
	struct hnode *hnode;

	hnode = get_hnode(gm_page->hnid);
	if (!hnode)
		return;
	hnode_freelist_add(hnode, gm_page);
	hnode_free_pages_inc(hnode);
}

static int gm_evict_page_locked(struct gm_page *gm_page)
{
	struct gm_dev *gm_dev;
	struct gm_mapping *gm_mapping;
	struct vm_area_struct *vma;
	struct mm_struct *mm = gm_page->mm;
	struct page *page;
	struct device *dma_dev;
	unsigned long va = gm_page->va;
	struct folio *folio = NULL;
	struct gm_fault_t gmf = {
		.mm = mm,
		.va = va,
		.size = HPAGE_SIZE,
		.copy = true
	};
	int ret = 0;

	gm_dev = get_gm_dev(gm_page->hnid);
	if (!gm_dev)
		return -EINVAL;

	vma = find_vma(mm, va);
	if (!vma || !vma->vm_obj) {
		gmem_err("%s: cannot find vma or vma->vm_obj is null for va %lx", __func__, va);
		return -EINVAL;
	}

	gm_mapping = vm_object_lookup(vma->vm_obj, va);
	if (!gm_mapping) {
		gmem_err("%s: no gm_mapping for va %lx", __func__, va);
		return -EINVAL;
	}

	mutex_lock(&gm_mapping->lock);
	if (!gm_mapping_device(gm_mapping)) {
		gmem_err("%s: evicting gm_page conflicts with unmap.", __func__);
		ret = 0;
		goto gm_mapping_unlock;
	}

	folio = vma_alloc_folio(GFP_TRANSHUGE, HPAGE_PMD_ORDER, vma, va, true);
	if (!folio) {
		gmem_err("%s: allocate host page failed.", __func__);
		ret = -ENOMEM;
		goto gm_mapping_unlock;
	}
	page = &folio->page;

	gmf.dev = gm_dev;
	gmf.pfn = gm_page->dev_pfn;
	dma_dev = gm_dev->dma_dev;
	gmf.dma_addr = dma_map_page(dma_dev, page, 0, HPAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dma_dev, gmf.dma_addr)) {
		gmem_err("%s: dma map failed.", __func__);
		ret = -EINVAL;
		goto gm_mapping_unlock;
	}

	ret = gm_dev->mmu->peer_unmap(&gmf);
	if (ret)
		gmem_err("%s: peer_unmap failed.", __func__);

	dma_unmap_page(dma_dev, gmf.dma_addr, HPAGE_SIZE, DMA_BIDIRECTIONAL);
	gm_mapping_flags_set(gm_mapping, GM_PAGE_CPU);
	gm_mapping->page = page;
	put_gm_page(gm_page);
gm_mapping_unlock:
	mutex_unlock(&gm_mapping->lock);
	return ret;
}

static int gm_evict_page(struct gm_page *gm_page)
{
	struct mm_struct *mm = gm_page->mm;
	int ret;

	mmap_read_lock(mm);
	ret = gm_evict_page_locked(gm_page);
	mmap_read_unlock(mm);
	return ret;
}

static void gm_do_swap(struct hnode *hnode)
{
	struct list_head swap_list;
	struct gm_page *gm_page, *n;
	unsigned int nr_swap_pages = 0;
	int ret;

	INIT_LIST_HEAD(&swap_list);

	spin_lock(&hnode->activelist_lock);
	list_for_each_entry_safe(gm_page, n, &hnode->activelist, gm_page_list) {
		/* Move gm_page to temporary list. */
		get_gm_page(gm_page);
		list_move(&gm_page->gm_page_list, &swap_list);
		nr_swap_pages++;
		if (nr_swap_pages >= NUM_SWAP_PAGES)
			break;
	}
	spin_unlock(&hnode->activelist_lock);

	list_for_each_entry_safe(gm_page, n, &swap_list, gm_page_list) {
		list_del(&gm_page->gm_page_list);
		ret = gm_evict_page_locked(gm_page);
		if (ret) {
			gmem_err("%s: evict gm_page %lx failed, va %lx", __func__,
				 (unsigned long)gm_page, gm_page->va);
			if (ret == -ENOMEM) {
				/*
				 * Failed to allocate host page, so return gm_page
				 * to activelist.
				 */
				hnode_activelist_add(hnode, gm_page);
			} else {
				/*
				 * Conflicts with process exit, so return gm_page
				 * to freelist to avoid memory leak.
				 */
				atomic_set(&gm_page->refcount, 0);
				hnode_freelist_add(hnode, gm_page);
				hnode_active_pages_dec(hnode);
				hnode_free_pages_inc(hnode);
			}
			put_gm_page(gm_page);
			continue;
		}

		hnode_active_pages_dec(hnode);
		put_gm_page(gm_page);
	}
};

static inline bool need_wake_up_swapd(struct hnode *hnode)
{
	return false;
}

static int swapd_func(void *data)
{
	struct hnode *hnode = (struct hnode *)data;

	while (!kthread_should_stop()) {
		if (!need_wake_up_swapd(hnode)) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		}

		gm_do_swap(hnode);
	}

	return 0;
};

static void init_swapd(struct hnode *hnode)
{
	hnode->swapd_task = kthread_run(swapd_func, NULL, "gm_swapd/%u", hnode->id);
	if (IS_ERR(hnode->swapd_task)) {
		gmem_err("%s: create swapd task failed", __func__);
		hnode->swapd_task = NULL;
	}
}

static void wake_up_swapd(struct hnode *hnode)
{
	if (likely(hnode->swapd_task))
		wake_up_process(hnode->swapd_task);
}

static bool can_import(struct hnode *hnode)
{
	unsigned long nr_pages;
	unsigned long used_mem;

	nr_pages = atomic_read(&hnode->nr_free_pages) + atomic_read(&hnode->nr_active_pages);
	used_mem = nr_pages * HPAGE_SIZE;

	/* GMEM usable memory is unlimited if max_memsize is zero. */
	if (!hnode->max_memsize)
		return true;
	return used_mem < hnode->max_memsize;
}

static struct gm_page *get_gm_page_from_freelist(struct hnode *hnode)
{
	struct gm_page *gm_page;

	spin_lock(&hnode->freelist_lock);
	gm_page = list_first_entry_or_null(&hnode->freelist, struct gm_page, gm_page_list);
	/* Delete from freelist. */
	if (gm_page) {
		list_del(&gm_page->gm_page_list);
		hnode_free_pages_dec(hnode);
		get_gm_page(gm_page);
		/* TODO: wakeup swapd if needed. */
		if (need_wake_up_swapd(hnode))
			wake_up_swapd(hnode);
	}
	spin_unlock(&hnode->freelist_lock);

	return gm_page;
}

/*
 * gm_alloc_page - Allocate a gm_page.
 *
 * Allocate a gm_page from hnode freelist. If failed to allocate gm_page, try
 * to import memory from device. And if failed to import memory, try to swap
 * several gm_pages to host and allocate gm_page again.
 */
struct gm_page *gm_alloc_page(struct mm_struct *mm, struct hnode *hnode)
{
	struct gm_page *gm_page;
	struct gm_dev *gm_dev;
	int retry_times = 0;
	int ret = 0;

	if (hnode->dev)
		gm_dev = hnode->dev;
	else
		return NULL;

retry:
	gm_page = get_gm_page_from_freelist(hnode);
	if (!gm_page && can_import(hnode) && !hnode->import_failed) {
		/* Import pages from device. */
		ret = gm_dev->mmu->import_phys_mem(mm, hnode->id, NUM_IMPORT_PAGES);
		if (!ret)
			goto retry;
		hnode->import_failed = true;
	}

	/* Try to swap pages. */
	if (!gm_page) {
		if (retry_times > MAX_SWAP_RETRY_TIMES)
			return NULL;
		gm_do_swap(hnode);
		retry_times++;
		goto retry;
	}

	return gm_page;
}

