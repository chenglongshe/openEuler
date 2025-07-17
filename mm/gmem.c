// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/rwsem.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/spinlock.h>
#include <linux/xxhash.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/memory.h>
#include <linux/mmu_notifier.h>
#include <linux/swap.h>
#include <linux/ksm.h>
#include <linux/hashtable.h>
#include <linux/freezer.h>
#include <linux/oom.h>
#include <linux/numa.h>
#include <linux/mempolicy.h>
#include <linux/gmem.h>
#include <linux/xarray.h>
#include <linux/syscalls.h>
#include <linux/dma-mapping.h>
#include <linux/vm_object.h>
#include <linux/dma-direct.h>
#include <linux/workqueue.h>
#include <linux/proc_fs.h>

DEFINE_STATIC_KEY_FALSE(gmem_status);
EXPORT_SYMBOL_GPL(gmem_status);

static struct kmem_cache *gm_as_cache;
static struct kmem_cache *gm_dev_cache;
static struct kmem_cache *gm_ctx_cache;
static struct kmem_cache *gm_region_cache;
static DEFINE_XARRAY_ALLOC(gm_dev_id_pool);

static bool enable_gmem;

static inline unsigned long pe_mask(unsigned int order)
{
	if (order == 0)
		return PAGE_MASK;
	if (order == PMD_ORDER)
		return HPAGE_PMD_MASK;
	if (order == PUD_ORDER)
		return HPAGE_PUD_MASK;
	return 0;
}

static struct percpu_counter g_gmem_stats[NR_GMEM_STAT_ITEMS];

void gmem_stats_counter(enum gmem_stats_item item, int val)
{
	if (!gmem_is_enabled())
		return;

	if (WARN_ON_ONCE(unlikely(item >= NR_GMEM_STAT_ITEMS)))
		return;

	percpu_counter_add(&g_gmem_stats[item], val);
}

static int gmem_stat_init(void)
{
	int i, rc;

	for (i = 0; i < NR_GMEM_STAT_ITEMS; i++) {
		rc = percpu_counter_init(&g_gmem_stats[i], 0, GFP_KERNEL);
		if (rc) {
			for (i--; i >= 0; i--)
				percpu_counter_destroy(&g_gmem_stats[i]);

			break;	/* break the initialization process */
		}
	}

	return rc;
}

#ifdef CONFIG_PROC_FS
static int gmem_stats_show(struct seq_file *m, void *arg)
{
	if (!gmem_is_enabled())
		return 0;

	seq_printf(
		m, "migrating H2D     : %lld\n",
		percpu_counter_read_positive(&g_gmem_stats[NR_PAGE_MIGRATING_H2D]));
	seq_printf(
		m, "migrating D2H     : %lld\n",
		percpu_counter_read_positive(&g_gmem_stats[NR_PAGE_MIGRATING_D2H]));

	return 0;
}
#endif /* CONFIG_PROC_FS */

static struct workqueue_struct *prefetch_wq;
static struct workqueue_struct *hmemcpy_wq;

#define GM_WORK_CONCURRENCY 4

static int __init gmem_init(void)
{
	int err = -ENOMEM;

	if (!enable_gmem)
		return 0;

	gm_as_cache = KMEM_CACHE(gm_as, 0);
	if (!gm_as_cache)
		goto out;

	gm_dev_cache = KMEM_CACHE(gm_dev, 0);
	if (!gm_dev_cache)
		goto free_as;

	gm_ctx_cache = KMEM_CACHE(gm_context, 0);
	if (!gm_ctx_cache)
		goto free_dev;

	gm_region_cache = KMEM_CACHE(gm_region, 0);
	if (!gm_region_cache)
		goto free_ctx;

	err = vm_object_init();
	if (err)
		goto free_region;

	err = gmem_stat_init();
	if (err)
		goto free_region;

	prefetch_wq = alloc_workqueue("prefetch",
		__WQ_LEGACY | WQ_UNBOUND | WQ_HIGHPRI | WQ_CPU_INTENSIVE, GM_WORK_CONCURRENCY);
	if (!prefetch_wq) {
		gmem_err("fail to alloc workqueue prefetch_wq\n");
		err = -EFAULT;
		goto free_region;
	}

	hmemcpy_wq = alloc_workqueue("hmemcpy", __WQ_LEGACY | WQ_UNBOUND
			| WQ_HIGHPRI | WQ_CPU_INTENSIVE, GM_WORK_CONCURRENCY);
	if (!hmemcpy_wq) {
		gmem_err("fail to alloc workqueue hmemcpy_wq\n");
		err = -EFAULT;
		destroy_workqueue(prefetch_wq);
		goto free_region;
	}

#ifdef CONFIG_PROC_FS
	proc_create_single("gmemstat", 0444, NULL, gmem_stats_show);
#endif

	static_branch_enable(&gmem_status);

	return 0;

free_region:
	kmem_cache_destroy(gm_region_cache);
free_ctx:
	kmem_cache_destroy(gm_ctx_cache);
free_dev:
	kmem_cache_destroy(gm_dev_cache);
free_as:
	kmem_cache_destroy(gm_as_cache);
out:
	return -ENOMEM;
}
subsys_initcall(gmem_init);

static int __init setup_gmem(char *str)
{
	strtobool(str, &enable_gmem);

	return 1;
}
__setup("gmem=", setup_gmem);

/*
 * Create a GMEM device, register its MMU function and the page table.
 * The returned device pointer will be passed by new_dev.
 * A unique id will be assigned to the GMEM device, using Linux's xarray.
 */
enum gm_ret gm_dev_create(struct gm_mmu *mmu, void *dev_data, unsigned long cap,
		       struct gm_dev **new_dev)
{
	struct gm_dev *dev;

	if (!gmem_is_enabled())
		return GM_RET_FAILURE_UNKNOWN;

	dev = kmem_cache_alloc(gm_dev_cache, GFP_KERNEL);
	if (!dev)
		return GM_RET_NOMEM;

	if (xa_alloc(&gm_dev_id_pool, &dev->id, dev, xa_limit_32b,
		     GFP_KERNEL)) {
		kmem_cache_free(gm_dev_cache, dev);
		return GM_RET_NOMEM;
	}

	dev->capability = cap;
	dev->mmu = mmu;
	dev->dev_data = dev_data;
	dev->current_ctx = NULL;
	INIT_LIST_HEAD(&dev->gm_ctx_list);
	*new_dev = dev;
	nodes_clear(dev->registered_hnodes);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_dev_create);

// Destroy a GMEM device and reclaim the resources.
enum gm_ret gm_dev_destroy(struct gm_dev *dev)
{
	// TODO: implement it
	xa_erase(&gm_dev_id_pool, dev->id);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_dev_destroy);

/* Handle the page fault triggered by a given device */
enum gm_ret gm_dev_fault(struct mm_struct *mm, unsigned long addr, struct gm_dev *dev,
		      int behavior)
{
	enum gm_ret ret = GM_RET_SUCCESS;
	struct gm_mmu *mmu = dev->mmu;
	struct device *dma_dev = dev->dma_dev;
	struct vm_area_struct *vma;
	struct vm_object *obj;
	struct gm_mapping *gm_mapping;
	unsigned long size = HPAGE_SIZE;
	struct gm_fault_t gmf = {
		.mm = mm,
		.va = addr,
		.dev = dev,
		.size = size,
		.copy = false,
		.behavior = behavior
	};
	struct page *page = NULL;

	mmap_read_lock(mm);

	vma = find_vma(mm, addr);
	if (!vma || vma->vm_start > addr) {
		gmem_err("%s failed to find vma by addr %p\n", __func__, (void *)addr);
		pr_info("gmem: %s no vma\n", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto mmap_unlock;
	}
	obj = vma->vm_obj;
	if (!obj) {
		gmem_err("%s no vm_obj\n", __func__);
		ret = GM_RET_FAILURE_UNKNOWN;
		goto mmap_unlock;
	}

	xa_lock(obj->logical_page_table);
	gm_mapping = vm_object_lookup(obj, addr);
	if (!gm_mapping) {
		vm_object_mapping_create(obj, addr);
		gm_mapping = vm_object_lookup(obj, addr);
	}
	xa_unlock(obj->logical_page_table);

	if (unlikely(!gm_mapping)) {
		gmem_err("OOM when creating vm_obj!\n");
		ret = GM_RET_NOMEM;
		goto mmap_unlock;
	}
	mutex_lock(&gm_mapping->lock);
	if (gm_mapping_nomap(gm_mapping)) {
		goto peer_map;
	} else if (gm_mapping_device(gm_mapping)) {
		if (behavior == MADV_WILLNEED || behavior == MADV_PINNED) {
			goto peer_map;
		} else {
			ret = 0;
			goto unlock;
		}
	} else if (gm_mapping_cpu(gm_mapping)) {
		page = gm_mapping->page;
		if (!page) {
			gmem_err("host gm_mapping page is NULL. Set nomap\n");
			gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
			goto unlock;
		}
		get_page(page);
		/* zap_page_range_single can be used in Linux 6.4 and later versions. */
		zap_page_range_single(vma, addr, size, NULL);
		gmf.dma_addr =
			dma_map_page(dma_dev, page, 0, size, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dma_dev, gmf.dma_addr))
			gmem_err("dma map failed\n");

		gmf.copy = true;
	}

peer_map:
	ret = mmu->peer_map(&gmf);
	if (ret != GM_RET_SUCCESS) {
		if (ret == GM_RET_MIGRATING) {
			/*
			 * gmem page is migrating due to overcommit.
			 * update page to willneed and this will stop page evicting
			 */
			gm_mapping_flags_set(gm_mapping, GM_PAGE_WILLNEED);
			gmem_stats_counter(NR_PAGE_MIGRATING_D2H, 1);
			ret = GM_RET_SUCCESS;
		} else {
			gmem_err("peer map failed\n");
			if (page) {
				gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
				put_page(page);
			}
		}
		goto unlock;
	}

	if (page) {
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		put_page(page);
	}

	gm_mapping_flags_set(gm_mapping, GM_PAGE_DEVICE);
	gm_mapping->dev = dev;
unlock:
	mutex_unlock(&gm_mapping->lock);
mmap_unlock:
	mmap_read_unlock(mm);
	return ret;
}
EXPORT_SYMBOL_GPL(gm_dev_fault);

vm_fault_t gm_host_fault_locked(struct vm_fault *vmf,
				unsigned int order)
{
	vm_fault_t ret = 0;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address & pe_mask(order);
	struct vm_object *obj = vma->vm_obj;
	struct gm_mapping *gm_mapping;
	unsigned long size = HPAGE_SIZE;
	struct gm_dev *dev;
	struct device *dma_dev;
	struct gm_fault_t gmf = {
		.mm = vma->vm_mm,
		.va = addr,
		.size = size,
		.copy = true,
	};

	gm_mapping = vm_object_lookup(obj, addr);
	if (!gm_mapping) {
		gmem_err("host fault gm_mapping should not be NULL\n");
		return VM_FAULT_SIGBUS;
	}

	dev = gm_mapping->dev;
	gmf.dev = dev;
	dma_dev = dev->dma_dev;
	gmf.dma_addr =
		dma_map_page(dma_dev, vmf->page, 0, size, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dma_dev, gmf.dma_addr)) {
		gmem_err("host fault dma mapping error\n");
		return VM_FAULT_SIGBUS;
	}
	if (dev->mmu->peer_unmap(&gmf) != GM_RET_SUCCESS) {
		gmem_err("peer unmap failed\n");
		dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
		return VM_FAULT_SIGBUS;
	}

	dma_unmap_page(dma_dev, gmf.dma_addr, size, DMA_BIDIRECTIONAL);
	return ret;
}

/*
 * Register the local physical memory of a gmem device.
 * This implies dynamically creating
 * the struct page data structures.
 */
enum gm_ret gm_dev_register_physmem(struct gm_dev *dev, unsigned long begin, unsigned long end)
{
	struct gm_mapping *mapping;
	unsigned long addr = PAGE_ALIGN(begin);
	unsigned int nid;
	int i, page_num = (end - addr) >> PAGE_SHIFT;
	struct hnode *hnode = kmalloc(sizeof(struct hnode), GFP_KERNEL);

	if (!hnode)
		goto err;

	nid = alloc_hnode_id();
	if (nid == MAX_NUMNODES)
		goto free_hnode;
	hnode_init(hnode, nid, dev);

	mapping = kvmalloc_array(page_num, sizeof(struct gm_mapping), GFP_KERNEL);
	if (!mapping)
		goto deinit_hnode;

	for (i = 0; i < page_num; i++, addr += PAGE_SIZE) {
		mapping[i].pfn = addr >> PAGE_SHIFT;
		mapping[i].flag = 0;
	}

	xa_lock(&hnode->pages);
	for (i = 0; i < page_num; i++) {
		if (xa_err(__xa_store(&hnode->pages, i, mapping + i,
				      GFP_KERNEL))) {
			/* Probably nomem */
			kvfree(mapping);
			xa_unlock(&hnode->pages);
			goto deinit_hnode;
		}
		__xa_set_mark(&hnode->pages, i, XA_MARK_0);
	}
	xa_unlock(&hnode->pages);

	return GM_RET_SUCCESS;

deinit_hnode:
	hnode_deinit(nid, dev);
	free_hnode_id(nid);
free_hnode:
	kfree(hnode);
err:
	return -ENOMEM;
}
EXPORT_SYMBOL_GPL(gm_dev_register_physmem);

void gm_dev_unregister_physmem(struct gm_dev *dev, unsigned int nid)
{
	struct hnode *hnode = get_hnode(nid);
	struct gm_mapping *mapping = xa_load(&hnode->pages, 0);

	kvfree(mapping);
	hnode_deinit(nid, dev);
	free_hnode_id(nid);
	kfree(hnode);
}
EXPORT_SYMBOL_GPL(gm_dev_unregister_physmem);

struct gm_mapping *gm_mappings_alloc(unsigned int nid, unsigned int order)
{
	struct gm_mapping *mapping;
	struct hnode *node = get_hnode(nid);
	XA_STATE(xas, &node->pages, 0);

	/* TODO: support order > 0 */
	if (order != 0)
		return ERR_PTR(-EINVAL);

	xa_lock(&node->pages);
	mapping = xas_find_marked(&xas, ULONG_MAX, XA_MARK_0);
	if (!mapping) {
		xa_unlock(&node->pages);
		return ERR_PTR(-ENOMEM);
	}

	xas_clear_mark(&xas, XA_MARK_0);
	xa_unlock(&node->pages);

	return mapping;
}
EXPORT_SYMBOL_GPL(gm_mappings_alloc);

/* GMEM Virtual Address Space API */
enum gm_ret gm_as_create(unsigned long begin, unsigned long end, enum gm_as_alloc policy,
			unsigned long cache_quantum, struct gm_as **new_as)
{
	struct gm_as *as;

	if (!new_as)
		return -EINVAL;

	as = kmem_cache_alloc(gm_as_cache, GFP_ATOMIC);
	if (!as)
		return -ENOMEM;

	spin_lock_init(&as->rbtree_lock);
	as->rbroot = RB_ROOT;
	as->start_va = begin;
	as->end_va = end;
	as->policy = policy;

	INIT_LIST_HEAD(&as->gm_ctx_list);

	*new_as = as;
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_create);

enum gm_ret gm_as_destroy(struct gm_as *as)
{
	struct gm_context *ctx, *tmp_ctx;

	list_for_each_entry_safe(ctx, tmp_ctx, &as->gm_ctx_list, gm_as_link)
		kfree(ctx);

	kmem_cache_free(gm_as_cache, as);

	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_destroy);

enum gm_ret gm_as_attach(struct gm_as *as, struct gm_dev *dev, enum gm_mmu_mode mode,
			bool activate, struct gm_context **out_ctx)
{
	struct gm_context *ctx;
	int nid;
	int ret;

	ctx = kmem_cache_alloc(gm_ctx_cache, GFP_KERNEL);
	if (!ctx)
		return GM_RET_NOMEM;

	ctx->as = as;
	ctx->dev = dev;
	ctx->pmap = NULL;
	ret = dev->mmu->pmap_create(dev, &ctx->pmap);
	if (ret) {
		kmem_cache_free(gm_ctx_cache, ctx);
		return ret;
	}

	INIT_LIST_HEAD(&ctx->gm_dev_link);
	INIT_LIST_HEAD(&ctx->gm_as_link);
	list_add_tail(&dev->gm_ctx_list, &ctx->gm_dev_link);
	list_add_tail(&ctx->gm_as_link, &as->gm_ctx_list);

	if (activate) {
		/*
		 * Here we should really have a callback function to perform the context switch
		 * for the hardware. E.g. in x86 this function is effectively
		 * flushing the CR3 value. Currently we do not care time-sliced context switch,
		 * unless someone wants to support it.
		 */
		dev->current_ctx = ctx;
	}
	*out_ctx = ctx;

	/*
	 * gm_as_attach will be used to attach device to process address space.
	 * Handle this case and add hnodes registered by device to process mems_allowed.
	 */
	for_each_node_mask(nid, dev->registered_hnodes)
		node_set(nid, current->mems_allowed);
	return GM_RET_SUCCESS;
}
EXPORT_SYMBOL_GPL(gm_as_attach);

DEFINE_SPINLOCK(hnode_lock);
struct hnode *hnodes[MAX_NUMNODES];

void __init hnuma_init(void)
{
	unsigned int node;

	for_each_node(node)
		node_set(node, hnode_map);
}

unsigned int alloc_hnode_id(void)
{
	unsigned int node;

	spin_lock(&hnode_lock);
	node = first_unset_node(hnode_map);
	node_set(node, hnode_map);
	spin_unlock(&hnode_lock);

	return node;
}

void free_hnode_id(unsigned int nid)
{
	node_clear(nid, hnode_map);
}

void hnode_init(struct hnode *hnode, unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid] = hnode;
	hnodes[hnid]->id = hnid;
	hnodes[hnid]->dev = dev;
	node_set(hnid, dev->registered_hnodes);
	xa_init(&hnodes[hnid]->pages);
}

void hnode_deinit(unsigned int hnid, struct gm_dev *dev)
{
	hnodes[hnid]->id = 0;
	hnodes[hnid]->dev = NULL;
	node_clear(hnid, dev->registered_hnodes);
	xa_destroy(&hnodes[hnid]->pages);
	hnodes[hnid] = NULL;
}

struct prefetch_data {
	struct mm_struct *mm;
	struct gm_dev *dev;
	unsigned long addr;
	size_t size;
	struct work_struct work;
	int *res;
};

static void prefetch_work_cb(struct work_struct *work)
{
	struct prefetch_data *d =
		container_of(work, struct prefetch_data, work);
	unsigned long addr = d->addr, end = d->addr + d->size;
	int page_size = HPAGE_SIZE;
	int ret;

	do {
		/* MADV_WILLNEED: dev will soon access this addr. */
		ret = gm_dev_fault(d->mm, addr, d->dev, MADV_WILLNEED);
		if (ret == GM_RET_PAGE_EXIST) {
			gmem_err("%s: device has done page fault, ignore prefetch\n",
				__func__);
		} else if (ret != GM_RET_SUCCESS) {
			*d->res = -EFAULT;
			gmem_err("%s: call dev fault error %d\n", __func__, ret);
		}
	} while (addr += page_size, addr != end);

	kfree(d);
}

static int hmadvise_do_prefetch(struct gm_dev *dev, unsigned long addr, size_t size)
{
	unsigned long start, end, per_size;
	int page_size = HPAGE_SIZE;
	struct prefetch_data *data;
	struct vm_area_struct *vma;
	int res = GM_RET_SUCCESS;
	unsigned long old_start;

	/* overflow */
	if (check_add_overflow(addr, size, &end)) {
		gmem_err("addr plus size will cause overflow!\n");
		return -EINVAL;
	}

	old_start = end;

	/* Align addr by rounding outward to make page cover addr. */
	end = round_up(end, page_size);
	start = round_down(addr, page_size);
	size = end - start;

	if (!end && old_start) {
		gmem_err("end addr align up 2M causes invalid addr %p\n", (void *)end);
		return -EINVAL;
	}

	if (size == 0)
		return 0;

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, start);
	if (!vma || start < vma->vm_start || end > vma->vm_end) {
		mmap_read_unlock(current->mm);
		gmem_err("failed to find vma by invalid start %p or size 0x%zx.\n",
			(void *)start, size);
		return GM_RET_FAILURE_UNKNOWN;
	}  else if (!vma_is_peer_shared(vma)) {
		mmap_read_unlock(current->mm);
		gmem_err("%s the vma does not use VM_PEER_SHARED\n", __func__);
		return GM_RET_FAILURE_UNKNOWN;
	}
	mmap_read_unlock(current->mm);

	per_size = (size / GM_WORK_CONCURRENCY) & ~(page_size - 1);

	while (start < end) {
		data = kzalloc(sizeof(struct prefetch_data), GFP_KERNEL);
		if (!data) {
			flush_workqueue(prefetch_wq);
			return GM_RET_NOMEM;
		}

		INIT_WORK(&data->work, prefetch_work_cb);
		data->mm = current->mm;
		data->dev = dev;
		data->addr = start;
		data->res = &res;
		if (per_size == 0)
			data->size = size;
		else
			/* Process (1.x * per_size) for the last time */
			data->size = (end - start < 2 * per_size) ?
					     (end - start) :
					     per_size;
		queue_work(prefetch_wq, &data->work);
		start += data->size;
	}

	flush_workqueue(prefetch_wq);
	return res;
}

static int gmem_unmap_vma_pages(struct vm_area_struct *vma, unsigned long start,
				unsigned long end, int page_size)
{
	struct gm_fault_t gmf = {
		.mm = current->mm,
		.size = page_size,
		.copy = false,
	};
	struct gm_mapping *gm_mapping;
	struct vm_object *obj;
	int ret;

	obj = vma->vm_obj;
	if (!obj) {
		gmem_err("peer-shared vma should have vm_object\n");
		return -EINVAL;
	}

	for (; start < end; start += page_size) {
		xa_lock(obj->logical_page_table);
		gm_mapping = vm_object_lookup(obj, start);
		if (!gm_mapping) {
			xa_unlock(obj->logical_page_table);
			continue;
		}
		xa_unlock(obj->logical_page_table);
		mutex_lock(&gm_mapping->lock);
		if (gm_mapping_nomap(gm_mapping)) {
			mutex_unlock(&gm_mapping->lock);
			continue;
		} else if (gm_mapping_cpu(gm_mapping)) {
			zap_page_range_single(vma, start, page_size, NULL);
		} else {
			gmf.va = start;
			gmf.dev = gm_mapping->dev;
			ret = gm_mapping->dev->mmu->peer_unmap(&gmf);
			if (ret) {
				gmem_err("peer_unmap failed. ret %d\n", ret);
				mutex_unlock(&gm_mapping->lock);
				continue;
			}
		}
		gm_mapping_flags_set(gm_mapping, GM_PAGE_NOMAP);
		mutex_unlock(&gm_mapping->lock);
	}

	return 0;
}

static int hmadvise_do_eagerfree(unsigned long addr, size_t size)
{
	unsigned long start, end, i_start, i_end;
	int page_size = HPAGE_SIZE;
	struct vm_area_struct *vma;
	int ret = GM_RET_SUCCESS;
	unsigned long old_start;

	/* overflow */
	if (check_add_overflow(addr, size, &end)) {
		gmem_err("addr plus size will cause overflow!\n");
		return -EINVAL;
	}

	old_start = addr;

	/* Align addr by rounding inward to avoid excessive page release. */
	end = round_down(end, page_size);
	start = round_up(addr, page_size);
	if (start >= end) {
		pr_debug("gmem:start align up 2M >= end align down 2M.\n");
		return ret;
	}

	/* Check to see whether len was rounded up from small -ve to zero */
	if (old_start && !start) {
		gmem_err("start addr align up 2M causes invalid addr %p", (void *)start);
		return -EINVAL;
	}

	mmap_read_lock(current->mm);
	do {
		vma = find_vma_intersection(current->mm, start, end);
		if (!vma) {
			gmem_err("gmem: there is no valid vma\n");
			break;
		}

		if (!vma_is_peer_shared(vma)) {
			pr_debug("gmem:not peer-shared vma %p-%p, skip dontneed\n",
				(void *)vma->vm_start, (void *)vma->vm_end);
			start = vma->vm_end;
			continue;
		}

		i_start = start > vma->vm_start ? start : vma->vm_start;
		i_end = end < vma->vm_end ? end : vma->vm_end;
		ret = gmem_unmap_vma_pages(vma, i_start, i_end, page_size);
		if (ret)
			break;

		start = vma->vm_end;
	} while (start < end);

	mmap_read_unlock(current->mm);
	return ret;
}

static bool check_hmadvise_behavior(int behavior)
{
	return behavior == MADV_DONTNEED;
}

int hmadvise_inner(int hnid, unsigned long start, size_t len_in, int behavior)
{
	int error = -EINVAL;
	struct hnode *node;

	if (hnid == -1) {
		if (check_hmadvise_behavior(behavior)) {
			goto no_hnid;
		} else {
			gmem_err("hmadvise: behavior %d need hnid or is invalid\n",
				behavior);
			return error;
		}
	}

	if (hnid < 0) {
		gmem_err("hmadvise: invalid hnid %d < 0\n", hnid);
		return error;
	}

	if (!is_hnode(hnid) || !is_hnode_allowed(hnid)) {
		gmem_err("hmadvise: can't find hnode by hnid:%d or hnode is not allowed\n", hnid);
		return error;
	}

	node = get_hnode(hnid);
	if (!node) {
		gmem_err("hmadvise: hnode id %d is invalid\n", hnid);
		return error;
	}

no_hnid:
	switch (behavior) {
	case MADV_PREFETCH:
		return hmadvise_do_prefetch(node->dev, start, len_in);
	case MADV_DONTNEED:
		return hmadvise_do_eagerfree(start, len_in);
	default:
		gmem_err("hmadvise: unsupported behavior %d\n", behavior);
	}

	return error;
}
EXPORT_SYMBOL_GPL(hmadvise_inner);

struct hmemcpy_data {
	struct mm_struct *mm;
	int hnid;
	unsigned long src;
	unsigned long dest;
	size_t size;
	struct work_struct work;
};

static bool hnid_match_dest(int hnid, struct gm_mapping *dest)
{
	return (hnid < 0) ? gm_mapping_cpu(dest) : gm_mapping_device(dest);
}

static void do_hmemcpy(struct mm_struct *mm, int hnid, unsigned long dest,
		unsigned long src, size_t size)
{
	enum gm_ret ret;
	int page_size = HPAGE_SIZE;
	struct vm_area_struct *vma_dest, *vma_src;
	struct gm_mapping *gm_mmaping_dest, *gm_mmaping_src;
	struct gm_dev *dev = NULL;
	struct hnode *node;
	struct gm_memcpy_t gmc = {0};

	if (size == 0)
		return;

	vma_dest = find_vma(mm, dest);
	vma_src = find_vma(mm, src);

	gm_mmaping_dest = vm_object_lookup(vma_dest->vm_obj, dest & ~(page_size - 1));
	gm_mmaping_src = vm_object_lookup(vma_src->vm_obj, src & ~(page_size - 1));

	if (!gm_mmaping_src) {
		gmem_err("%s: gm_mmaping_src is NULL, src=%p; size=0x%zx\n",
			__func__, (void *)src, size);
		return;
	}

	if (hnid != -1) {
		node = get_hnode(hnid);
		if (node)
			dev = node->dev;
		if (!dev) {
			gmem_err("%s: hnode's dev is NULL\n", __func__);
			return;
		}
	}

	// Trigger dest page fault on host or device
	if (!gm_mmaping_dest || gm_mapping_nomap(gm_mmaping_dest)
		|| !hnid_match_dest(hnid, gm_mmaping_dest)) {
		if (hnid == -1) {
			mmap_read_lock(mm);
			handle_mm_fault(vma_dest, dest & ~(page_size - 1), FAULT_FLAG_USER |
					FAULT_FLAG_INSTRUCTION | FAULT_FLAG_WRITE, NULL);
			mmap_read_unlock(mm);
		} else {
			ret = gm_dev_fault(mm, dest & ~(page_size - 1), dev, MADV_WILLNEED);
			if (ret != GM_RET_SUCCESS) {
				gmem_err("%s: gm_dev_fault failed\n", __func__);
				return;
			}
		}
	}
	if (!gm_mmaping_dest)
		gm_mmaping_dest = vm_object_lookup(vma_dest->vm_obj, round_down(dest, page_size));

	if (gm_mmaping_dest && gm_mmaping_dest != gm_mmaping_src)
		mutex_lock(&gm_mmaping_dest->lock);
	mutex_lock(&gm_mmaping_src->lock);
	// Use memcpy when there is no device address, otherwise use peer_memcpy
	if (hnid == -1) {
		if (gm_mapping_cpu(gm_mmaping_src)) { // host to host
			memcpy(page_to_virt(gm_mmaping_dest->page) + (dest & (page_size - 1)),
				page_to_virt(gm_mmaping_src->page) + (src & (page_size - 1)),
				size);
			goto unlock;
		} else { // device to host
			dev = gm_mmaping_src->dev;
			gmc.dma_addr = phys_to_dma(dev->dma_dev,
				page_to_phys(gm_mmaping_dest->page) + (dest & (page_size - 1)));
			gmc.src = src;
		}
	} else {
		if (gm_mapping_cpu(gm_mmaping_src)) { // host to device
			gmc.dest = dest;
			gmc.dma_addr = phys_to_dma(dev->dma_dev,
				page_to_phys(gm_mmaping_src->page) + (src & (page_size - 1)));
		} else { // device to device
			if (dev == gm_mmaping_src->dev) { // same device
				gmc.dest = dest;
				gmc.src = src;
			} else { // TODO: different devices
				gmem_err("%s: device to device is unimplemented\n", __func__);
				goto unlock;
			}
		}
	}
	gmc.mm = mm;
	gmc.dev = dev;
	gmc.size = size;
	dev->mmu->peer_hmemcpy(&gmc);

unlock:
	mutex_unlock(&gm_mmaping_src->lock);
	if (gm_mmaping_dest && gm_mmaping_dest != gm_mmaping_src)
		mutex_unlock(&gm_mmaping_dest->lock);
}

/*
 * Each page needs to be copied in three parts when the address is not aligned.
 * |         <--a-->|                |
 * |         -------|---------       |
 * |        /      /|  /     /       |
 * |       /      / | /     /        |
 * |      /      /  |/     /         |
 * |      ----------|------          |
 * |      <----b--->|                |
 * |<----page x---->|<----page y---->|
 */

static void hmemcpy_work_cb(struct work_struct *work)
{
	size_t i;
	int remain, a, b, page_size = HPAGE_SIZE;
	struct hmemcpy_data *d = container_of(work, struct hmemcpy_data, work);
	unsigned long src = d->src, dest = d->dest;

	a = min(page_size - (src & (page_size - 1)), page_size - (dest & (page_size - 1)));
	b = max(page_size - (src & (page_size - 1)), page_size - (dest & (page_size - 1)));

	for (i = page_size; i < d->size; i += page_size) {
		if (a != 0)
			do_hmemcpy(d->mm, d->hnid, dest, src, a);
		if (b - a != 0)
			do_hmemcpy(d->mm, d->hnid, dest + a, src + a, b - a);
		if (page_size - b != 0)
			do_hmemcpy(d->mm, d->hnid, dest + b, src + b, page_size - b);
		src += page_size;
		dest += page_size;
	}

	remain = d->size + page_size - i;
	if (remain == 0)
		goto out;

	if (remain < a) {
		do_hmemcpy(d->mm, d->hnid, dest, src, remain);
	} else if (remain < b) {
		do_hmemcpy(d->mm, d->hnid, dest, src, a);
		do_hmemcpy(d->mm, d->hnid, dest + a, src + a, remain - a);
	} else {
		do_hmemcpy(d->mm, d->hnid, dest, src, a);
		do_hmemcpy(d->mm, d->hnid, dest + a, src + a, b - a);
		do_hmemcpy(d->mm, d->hnid, dest + b, src + b, remain - b);
	}

out:
	kfree(d);
}

int hmemcpy(int hnid, unsigned long dest, unsigned long src, size_t size)
{
	int page_size = HPAGE_SIZE;
	unsigned long per_size, copied = 0;
	struct hmemcpy_data *data;
	struct vm_area_struct *vma_dest, *vma_src;

	if (hnid < 0) {
		if (hnid != -1) {
			gmem_err("hmadvise: invalid hnid %d < 0\n", hnid);
			return -EINVAL;
		}
	} else if (!is_hnode(hnid) || !is_hnode_allowed(hnid)) {
		gmem_err(
			"hmadvise: can't find hnode by hnid:%d or hnode is not allowed\n",
			hnid);
		return -EINVAL;
	}

	vma_dest = find_vma(current->mm, dest);
	vma_src = find_vma(current->mm, src);

	if (!vma_src || vma_src->vm_start > src || !vma_is_peer_shared(vma_src)
		|| vma_src->vm_end < (src + size)) {
		gmem_err("failed to find peer_shared vma by invalid src:%p or size :0x%zx",
			(void *)src, size);
		return -EINVAL;
	}

	if (!vma_dest || vma_dest->vm_start > dest || !vma_is_peer_shared(vma_dest)
		|| vma_dest->vm_end < (dest + size)) {
		gmem_err("failed to find peer_shared vma by invalid dest:%p or size :0x%zx",
			(void *)dest, size);
		return -EINVAL;
	}

	if (!(vma_dest->vm_flags & VM_WRITE)) {
		gmem_err("dest is not writable.\n");
		return -EINVAL;
	}

	if (!(vma_dest->vm_flags & VM_WRITE)) {
		gmem_err("dest is not writable.\n");
		return -EINVAL;
	}

	per_size = (size / GM_WORK_CONCURRENCY) & ~(page_size - 1);

	while (copied < size) {
		data = kzalloc(sizeof(struct hmemcpy_data), GFP_KERNEL);
		if (data == NULL) {
			flush_workqueue(hmemcpy_wq);
			return GM_RET_NOMEM;
		}
		INIT_WORK(&data->work, hmemcpy_work_cb);
		data->mm = current->mm;
		data->hnid = hnid;
		data->src = src;
		data->dest = dest;
		if (per_size == 0) {
			data->size = size;
		} else {
			// Process (1.x * per_size) for the last time
			data->size = (size - copied < 2 * per_size) ? (size - copied) : per_size;
		}

		queue_work(hmemcpy_wq, &data->work);
		src += data->size;
		dest += data->size;
		copied += data->size;
	}

	flush_workqueue(hmemcpy_wq);
	return 0;
}
EXPORT_SYMBOL_GPL(hmemcpy);
