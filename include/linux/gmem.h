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

extern int hmadvise_inner(int hnid, unsigned long start, size_t len_in, int behavior);
extern int hmemcpy(int hnid, unsigned long dest, unsigned long src, size_t size);

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

struct gm_page *alloc_gm_page_struct(void);

#define gmem_err(fmt, ...) \
	((void)pr_err("[gmem]" fmt "\n", ##__VA_ARGS__))

#endif /* _GMEM_H */
