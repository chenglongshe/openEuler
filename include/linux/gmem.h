/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Generalized Memory Management.
 *
 * Copyright (C) 2023- Huawei, Inc.
 * Author: Weixi Zhu
 *
 */
#ifndef _GMEM_H
#define _GMEM_H

#include <linux/mm.h>

struct hnode;

/*
 * enum gm_ret - The return value of GMEM KPI that can be used to tell
 * the core VM or peripheral driver whether the GMEM KPI was
 * executed successfully.
 *
 * @GM_RET_SUCCESS:	The invoked GMEM KPI behaved as expected.
 * @GM_RET_FAILURE_UNKNOWN:	The GMEM KPI failed with unknown reason.
 * Any external status related to this KPI invocation changes must be rolled back.
 */
enum gm_ret {
	GM_RET_SUCCESS = 0,
	GM_RET_NOMEM,
	GM_RET_PAGE_EXIST,
	GM_RET_DMA_ERROR,
	GM_RET_MIGRATING,
	GM_RET_FAILURE_UNKNOWN,
	GM_RET_UNIMPLEMENTED,
};

/*
 * Defines a contiguous range of virtual addresses inside a struct gm_as
 * As an analogy, this is conceptually similar as virtual_address_struct
 */
struct gm_region {
	unsigned long start_va;
	unsigned long end_va;
	struct rb_node node;
	struct gm_as *as; /* The address space that it belongs to */

	/* Do we need another list_node to maintain a tailQ of allocated VMAs inside a gm_as? */
	struct list_head mapping_set_link;

	void (*callback_op)(void *args);
	void *cb_args;
};

/* This holds a list of regions that must not be concurrently manipulated. */
struct gm_mapping_set {
	unsigned int region_cnt;
	struct list_head gm_region_list;
};

/**
 * enum gm_mmu_mode - defines the method to share a physical page table.
 *
 * @GM_MMU_MODE_SHARE: Literally share a physical page table with another
 * attached device's MMU. Nothing is guaranteed about the allocated address.
 * @GM_MMU_MODE_COHERENT_EXCLUSIVE: Maintain a coherent page table that holds
 * exclusive mapping entries, so that device memory accesses can trigger fault-driven
 * migration for automatic data locality optimizations.
 * @GM_MMU_MODE_REPLICATE: Maintain a coherent page table that replicates physical
 * mapping entries whenever a physical mapping is installed inside the address space, so
 * that it may minimize the page faults to be triggered by this device.
 */
enum gm_mmu_mode {
	GM_MMU_MODE_SHARE,
	GM_MMU_MODE_COHERENT_EXCLUSIVE,
	GM_MMU_MODE_REPLICATE,
};

/*
 * This is the parameter list of peer_map/unmap mmu operations.
 * if device should copy data to/from host, set copy and dma_addr
 */
struct gm_fault_t {
	struct mm_struct *mm;
	struct gm_dev *dev;
	unsigned long pfn;
	unsigned long va;
	unsigned long size;
	unsigned long prot;
	bool copy;
	dma_addr_t dma_addr;
	int behavior;
};

enum gm_memcpy_kind {
	GM_MEMCPY_INIT,
	GM_MEMCPY_H2H,
	GM_MEMCPY_H2D,
	GM_MEMCPY_D2H,
	GM_MEMCPY_D2D,
	GM_MEMCPY_KIND_INVALID,
};

struct gm_memcpy_t {
	struct mm_struct *mm;
	struct gm_dev *dev;
	dma_addr_t src;
	dma_addr_t dest;

	size_t size;
	enum gm_memcpy_kind kind;
};

/**
 *
 * This struct defines a series of MMU functions registered by a peripheral
 * device that is to be invoked by GMEM.
 *
 * pmap is an opaque pointer that identifies a physical page table of a device.
 * A physical page table holds the physical mappings that can be interpreted by
 * the hardware MMU.
 */
struct gm_mmu {
	/*
	 * Each bit indicates a supported page size for page-based TLB.
	 * Currently we do not consider range TLBs.
	 */
	unsigned long pgsize_bitmap;

	/*
	 * cookie identifies the type of the MMU. If two gm_mmu shares the same cookie,
	 * then it means their page table formats are compatible.
	 * In that case, they can share the same void *pmap as the input arg.
	 */
	unsigned long cookie;

	/* Synchronize VMA in a peer OS to interact with the host OS */
	enum gm_ret (*peer_va_alloc_fixed)(struct gm_fault_t *gmf);
	enum gm_ret (*peer_va_free)(struct gm_fault_t *gmf);

	/* Create physical mappings on peer host.
	 * If copy is set, copy data [dma_addr, dma_addr + size] to peer host
	 */
	enum gm_ret (*peer_map)(struct gm_fault_t *gmf);
	/*
	 * Destroy physical mappings on peer host.
	 * If copy is set, copy data back to [dma_addr, dma_addr + size]
	 */
	enum gm_ret (*peer_unmap)(struct gm_fault_t *gmf);

	enum gm_ret (*import_phys_mem)(struct mm_struct *mm, int hnid, unsigned long page_cnt);

	/* Create or destroy a device's physical page table. */
	enum gm_ret (*pmap_create)(struct gm_dev *dev, void **pmap);
	enum gm_ret (*pmap_destroy)(void *pmap);

	/* Create or destroy a physical mapping of a created physical page table */
	enum gm_ret (*pmap_enter)(void *pmap, unsigned long va, unsigned long size,
			     unsigned long pa, unsigned long prot);
	enum gm_ret (*pmap_release)(void *pmap, unsigned long va, unsigned long size);

	/* Change the protection of a virtual page */
	enum gm_ret (*pmap_protect)(void *pmap, unsigned long va, unsigned long size,
						unsigned long new_prot);

	/* Invalidation functions of the MMU TLB */
	enum gm_ret (*tlb_invl)(void *pmap, unsigned long va, unsigned long size);
	enum gm_ret (*tlb_invl_coalesced)(void *pmap, struct list_head *mappings);

	// copy one area of memory from device to host or from host to device
	enum gm_ret (*peer_hmemcpy)(struct gm_memcpy_t *gmc);
};

/**
 * unsigned long defines a composable flag to describe the capabilities of a device.
 *
 * @GM_DEV_CAP_REPLAYABLE: Memory accesses can be replayed to recover page faults.
 * @GM_DEV_CAP_PEER: The device has its own VMA/PA management, controlled by another peer OS
 */
#define GM_DEV_CAP_REPLAYABLE	0x00000001
#define GM_DEV_CAP_PEER		0x00000010

#define gm_dev_is_peer(dev) (((dev)->capability & GM_DEV_CAP_PEER) != 0)

struct gm_context {
	struct gm_as *as;
	struct gm_dev *dev;
	void *pmap;
	/*
	 * consider a better container to maintain multiple ctx inside a device or multiple ctx
	 * inside a va space.
	 * A device may simultaneously have multiple contexts for time-sliced ctx switching
	 */
	struct list_head gm_dev_link;

	/* A va space may have multiple gm_context */
	struct list_head gm_as_link;
};
#define get_gm_context(head) (list_entry((head)->prev, struct gm_context, ctx_link))

struct gm_dev {
	int id;

	/* identifies the device capability
	 * For example, whether the device supports page faults or whether it has its
	 * own OS that manages the VA and PA resources.
	 */
	unsigned long capability;
	struct gm_mmu *mmu;
	void *dev_data;
	/*
	 * TODO: Use a better container of struct gm_context to support time-sliced context switch.
	 * A collection of device contexts. If the device does not support time-sliced context
	 * switch, then the size of the collection should never be greater than one.
	 * We need to think about what operators should the container be optimized for.
	 * A list, a radix-tree or what? What would gm_dev_activate require?
	 * Are there any accelerators that are really going to support time-sliced context switch?
	 */
	struct gm_context *current_ctx;

	struct list_head gm_ctx_list;

	/* Add tracking of registered device local physical memory. */
	nodemask_t registered_hnodes;
	struct device *dma_dev;

	struct gm_mapping *gm_mapping;
};

#define GM_MAPPING_CPU		0x10 /* Determines whether page is a pointer or a pfn number. */
#define GM_MAPPING_DEVICE	0x20
#define GM_MAPPING_NOMAP	0x40
#define GM_MAPPING_PINNED	0x80
#define GM_MAPPING_WILLNEED	0x100

#define GM_MAPPING_TYPE_MASK	(GM_MAPPING_CPU | GM_MAPPING_DEVICE | GM_MAPPING_NOMAP)

/* Records the status of a page-size physical page */
struct gm_mapping {
	unsigned int flag;

	union {
		struct page *page;		/* CPU node */
		struct gm_page *gm_page;	/* hetero-node */
	};

	struct gm_dev *dev;
	struct mutex lock;
};

static inline void gm_mapping_flags_set(struct gm_mapping *gm_mapping, int flags)
{
	if (flags & GM_MAPPING_TYPE_MASK)
		gm_mapping->flag &= ~GM_MAPPING_TYPE_MASK;

	gm_mapping->flag |= flags;
}

static inline void gm_mapping_flags_clear(struct gm_mapping *gm_mapping, int flags)
{
	gm_mapping->flag &= ~flags;
}

static inline bool gm_mapping_cpu(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_CPU);
}

static inline bool gm_mapping_device(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_DEVICE);
}

static inline bool gm_mapping_nomap(struct gm_mapping *gm_mapping)
{
	return !!(gm_mapping->flag & GM_MAPPING_NOMAP);
}

#define test_gm_mapping_mapped_on_node(i) { /* implement this */ }
#define set_gm_mapping_mapped_on_node(i) { /* implement this */ }
#define unset_gm_mapping_mapped_on_node(i) { /* implement this */ }

/* GMEM Device KPI */
extern enum gm_ret gm_dev_create(struct gm_mmu *mmu, void *dev_data, unsigned long cap,
				struct gm_dev **new_dev);
extern enum gm_ret gm_dev_switch(struct gm_dev *dev, struct gm_as *as);
extern enum gm_ret gm_dev_detach(struct gm_dev *dev, struct gm_as *as);
extern int gm_dev_register_hnode(struct gm_dev *dev);
enum gm_ret gm_dev_fault_locked(struct mm_struct *mm, unsigned long addr,
				struct gm_dev *dev, int behavior);
vm_fault_t gm_host_fault_locked(struct vm_fault *vmf, unsigned int order);

/* GMEM address space KPI */
extern enum gm_ret gm_as_create(unsigned long begin, unsigned long end, enum gm_as_alloc policy,
				unsigned long cache_quantum, struct gm_as **new_as);
extern enum gm_ret gm_as_destroy(struct gm_as *as);
extern enum gm_ret gm_as_attach(struct gm_as *as, struct gm_dev *dev, enum gm_mmu_mode mode,
				bool activate, struct gm_context **out_ctx);
extern unsigned long gm_as_alloc(struct gm_as *as, unsigned long hint, unsigned long size,
				unsigned long align, unsigned long no_cross, unsigned long max_va,
				struct gm_region **new_region);

extern int hmadvise_inner(int hnid, unsigned long start, size_t len_in, int behavior);
extern int hmemcpy(int hnid, unsigned long dest, unsigned long src, size_t size);

enum gmem_stats_item {
	NR_PAGE_MIGRATING_H2D,
	NR_PAGE_MIGRATING_D2H,
	NR_GMEM_STAT_ITEMS
};

extern void gmem_stats_counter(enum gmem_stats_item item, int val);
extern void gmem_stats_counter_show(void);

/* h-NUMA topology */
struct hnode {
	unsigned int id;
	struct gm_dev *dev;

	struct task_struct *swapd_task;

	struct list_head freelist;
	struct list_head activelist;
	spinlock_t freelist_lock;
	spinlock_t activelist_lock;
	atomic_t nr_free_pages;
	atomic_t nr_active_pages;

	unsigned long max_memsize;

	bool import_failed;
};

static inline void hnode_active_pages_inc(struct hnode *hnode)
{
	atomic_inc(&hnode->nr_active_pages);
}

static inline void hnode_active_pages_dec(struct hnode *hnode)
{
	atomic_dec(&hnode->nr_active_pages);
}

static inline void hnode_free_pages_inc(struct hnode *hnode)
{
	atomic_inc(&hnode->nr_free_pages);
}

static inline void hnode_free_pages_dec(struct hnode *hnode)
{
	atomic_dec(&hnode->nr_free_pages);
}

static inline int get_hnuma_id(struct gm_dev *gm_dev)
{
	return first_node(gm_dev->registered_hnodes);
}

void __init hnuma_init(void);
bool is_hnode(int nid);
unsigned int alloc_hnode_id(void);
void free_hnode_id(unsigned int nid);
struct hnode *get_hnode(unsigned int hnid);
struct gm_dev *get_gm_dev(unsigned int nid);
void hnode_init(struct hnode *hnode, unsigned int hnid, struct gm_dev *dev);
void hnode_deinit(unsigned int hnid, struct gm_dev *dev);

struct gm_page {
	struct list_head gm_page_list;

	unsigned long flags;
	unsigned long dev_pfn;
	unsigned long dev_dma_addr;
	unsigned int hnid;

	/*
	* The same functionality as rmap, we need know which process
	* maps to this gm_page with which virtual address.
	* */
	unsigned long va;
	struct mm_struct *mm;
	spinlock_t rmap_lock;

	unsigned int flag;
	atomic_t refcount;
};

#define GM_PAGE_EVICTING	0x1
#define GM_PAGE_PINNED		0x2

static inline void gm_page_flags_set(struct gm_page *gm_page, int flags)
{
	gm_page->flag |= flags;
}

static inline void gm_page_flags_clear(struct gm_page *gm_page, int flags)
{
	gm_page->flag &= ~flags;
}

static inline bool gm_page_evicting(struct gm_page *gm_page)
{
	return !!(gm_page->flag & GM_PAGE_EVICTING);
}

static inline bool gm_page_pinned(struct gm_page *gm_page)
{
	return !!(gm_page->flag & GM_PAGE_PINNED);
}

#define NUM_IMPORT_PAGES   16

int __init gm_page_cachep_init(void);
void gm_page_cachep_destroy(void);
struct gm_page *alloc_gm_page_struct(void);
void hnode_freelist_add(struct hnode *hnode, struct gm_page *gm_page);
void hnode_activelist_add(struct hnode *hnode, struct gm_page *gm_page);
void hnode_activelist_del(struct hnode *hnode, struct gm_page *gm_page);
void hnode_activelist_del_and_add(struct hnode *hnode, struct gm_page *gm_page);
void mark_gm_page_active(struct gm_page *gm_page);
void mark_gm_page_pinned(struct gm_page *gm_page);
void mark_gm_page_unpinned(struct gm_page *gm_page);
void gm_page_add_rmap(struct gm_page *gm_page, struct mm_struct *mm, unsigned long va);
void gm_page_remove_rmap(struct gm_page *gm_page);
int gm_add_pages(unsigned int hnid, struct list_head *pages);
void gm_free_page(struct gm_page *gm_page);
struct gm_page *gm_alloc_page(struct mm_struct *mm, struct hnode *hnode);

static inline void get_gm_page(struct gm_page *gm_page)
{
	atomic_inc(&gm_page->refcount);
}

static inline void put_gm_page(struct gm_page *gm_page)
{
	if (atomic_dec_and_test(&gm_page->refcount))
		gm_free_page(gm_page);
}

int hnode_init_sysfs(unsigned int hnid);
int __init gm_init_sysfs(void);
void gm_deinit_sysfs(void);

#define gmem_err(fmt, ...) \
	((void)pr_err("[gmem]" fmt "\n", ##__VA_ARGS__))

#endif /* _GMEM_H */
