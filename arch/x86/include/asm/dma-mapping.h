/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_DMA_MAPPING_H
#define _ASM_X86_DMA_MAPPING_H

/*
 * IOMMU interface. See Documentation/core-api/dma-api-howto.rst and
 * Documentation/core-api/dma-api.rst for documentation.
 */

#include <linux/scatterlist.h>
#include <asm/io.h>
#include <asm/swiotlb.h>

extern int iommu_merge;
extern int panic_on_overflow;

extern const struct dma_map_ops *dma_ops;

static inline const struct dma_map_ops *get_arch_dma_ops(struct bus_type *bus)
{
	return dma_ops;
}

#if IS_BUILTIN(CONFIG_INTEL_IOMMU)

bool is_zhaoxin_kh40000(void);

phys_addr_t kh40000_iommu_iova_to_phys(struct device *dev, dma_addr_t paddr);
void kh40000_sync_single_dma_for_cpu(struct device *dev, dma_addr_t paddr,
				     enum dma_data_direction dir, bool is_iommu);
struct page *kh40000_alloc_coherent(int node, gfp_t gfp, size_t size);

#endif

#endif
