/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_DMA_MAPPING_H
#define _ASM_X86_DMA_MAPPING_H

/*
 * IOMMU interface. See Documentation/DMA-API-HOWTO.txt and
 * Documentation/DMA-API.txt for documentation.
 */

#include <linux/scatterlist.h>
#include <linux/dma-debug.h>
#include <asm/io.h>
#include <asm/swiotlb.h>
#include <linux/dma-contiguous.h>

#ifdef CONFIG_ISA
# define ISA_DMA_BIT_MASK DMA_BIT_MASK(24)
#else
# define ISA_DMA_BIT_MASK DMA_BIT_MASK(32)
#endif

extern int iommu_merge;
extern struct device x86_dma_fallback_dev;
extern int panic_on_overflow;

extern const struct dma_map_ops *dma_ops;

static inline const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus)
{
	return dma_ops;
}

#if IS_BUILTIN(CONFIG_INTEL_IOMMU) && IS_BUILTIN(CONFIG_X86_64)
extern bool is_zhaoxin_kh40000(void);
phys_addr_t kh40000_iommu_iova_to_phys(struct device *dev, dma_addr_t paddr);
void kh40000_sync_single_dma_for_cpu(struct device *dev, dma_addr_t paddr,
				     enum dma_data_direction dir, bool is_iommu);
void *kh40000_dma_direct_alloc(struct device *dev, size_t size, dma_addr_t *dma_handle,
			       gfp_t gfp, unsigned long attrs);
void kh40000_set_direct_dma_ops(void);
void kh40000_set_swiotlb_dma_ops(void);
#else
static inline void kh40000_set_direct_dma_ops(void)
{

}

static inline void kh40000_set_swiotlb_dma_ops(void)
{

}
#endif

bool arch_dma_alloc_attrs(struct device **dev);
#define arch_dma_alloc_attrs arch_dma_alloc_attrs

#endif
