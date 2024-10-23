/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KVM_VFIO_H
#define __KVM_VFIO_H

#ifdef CONFIG_HISI_VIRTCCA_HOST
#include <linux/kvm_host.h>
#include <asm/kvm_tmi.h>
#include "../drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3.h"
#include "../drivers/iommu/arm/arm-smmu-v3/arm-s-smmu-v3.h"
#endif

#ifdef CONFIG_KVM_VFIO
int kvm_vfio_ops_init(void);
void kvm_vfio_ops_exit(void);
#else
static inline int kvm_vfio_ops_init(void)
{
	return 0;
}
static inline void kvm_vfio_ops_exit(void)
{
}
#endif

#ifdef CONFIG_HISI_VIRTCCA_HOST
struct kvm *arm_smmu_get_kvm(struct arm_smmu_domain *domain);
int kvm_get_arm_smmu_domain(struct kvm *kvm, struct list_head *smmu_domain_group_list);
#endif
#endif
