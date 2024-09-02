/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 */
#ifndef _ARM_S_SMMU_V3_H
#define _ARM_S_SMMU_V3_H

#include <linux/platform_device.h>

#define MAX_CC_DEV_NUM_ORDER 8
#define STE_ENTRY_SIZE			0x40

#define ARM_SMMU_MAX_IDS		(1 << 5)
#define ARM_SMMU_INVALID_ID		ARM_SMMU_MAX_IDS

/* Secure MMIO registers */
#define ARM_SMMU_S_IDR0			0x8000
#define S_IDR0_STALL_MODEL		GENMASK(25, 24)
#define S_IDR0_ECMDQ			(1 << 31)
#define S_IDR0_MSI			    (1 << 13)

#define ARM_SMMU_S_IDR1			0x8004
#define S_IDR1_SECURE_IMPL	    (1 << 31)
#define S_IDR1_SEL2	            (1 << 29)
#define S_IDR1_SIDSIZE			GENMASK(5, 0)

#define ARM_SMMU_S_IDR3			0x800c
#define S_IDR3_SAMS			    (1 << 6)

#define ARM_SMMU_S_CR0		    0x8020
#define S_CR0_SIF			    (1 << 9)
#define S_CR0_NSSTALLD			(1 << 5)
#define S_CR0_CMDQEN			(1 << 3)
#define S_CR0_EVTQEN			(1 << 2)
#define S_CR0_SMMUEN			(1 << 0)

#define ARM_SMMU_S_CR0ACK		0x8024

#define ARM_SMMU_S_CR1			0x8028
#define S_CR1_TABLE_SH			GENMASK(11, 10)
#define S_CR1_TABLE_OC			GENMASK(9, 8)
#define S_CR1_TABLE_IC			GENMASK(7, 6)
#define S_CR1_QUEUE_SH			GENMASK(5, 4)
#define S_CR1_QUEUE_OC			GENMASK(3, 2)
#define S_CR1_QUEUE_IC			GENMASK(1, 0)

/* S_CR1 cacheability fields don't quite follow the usual TCR-style encoding */
#define S_CR1_CACHE_NC			0
#define S_CR1_CACHE_WB			1
#define S_CR1_CACHE_WT			2

#define ARM_SMMU_S_CR2			0x802c
#define S_CR2_PTM				(1 << 2)
#define S_CR2_RECINVSID			(1 << 1)
#define S_CR2_E2H				(1 << 0)

#define ARM_SMMU_S_INIT			U(0x803c)
/* SMMU_S_INIT register fields */
#define SMMU_S_INIT_INV_ALL		(1UL << 0)

#define ARM_SMMU_S_GBPA			0x8044
#define S_GBPA_UPDATE			(1 << 31)
#define S_GBPA_ABORT			(1 << 20)

#define ARM_SMMU_S_IRQ_CTRL		0x8050
#define S_IRQ_CTRL_EVTQ_IRQEN		(1 << 2)
#define S_IRQ_CTRL_GERROR_IRQEN		(1 << 0)

#define ARM_SMMU_S_IRQ_CTRLACK		0x8054

#define ARM_SMMU_S_GERROR			0x8060
#define S_GERROR_SFM_ERR			(1 << 8)
#define S_GERROR_MSI_GERROR_ABT_ERR	(1 << 7)
#define S_GERROR_MSI_EVTQ_ABT_ERR		(1 << 5)
#define S_GERROR_MSI_CMDQ_ABT_ERR		(1 << 4)
#define S_GERROR_EVTQ_ABT_ERR		(1 << 2)
#define S_GERROR_CMDQ_ERR			(1 << 0)

#define ARM_SMMU_S_GERRORN		    0x8064

#define ARM_SMMU_S_GERROR_IRQ_CFG0	0x8068
#define ARM_SMMU_S_GERROR_IRQ_CFG1	0x8070
#define ARM_SMMU_S_GERROR_IRQ_CFG2	0x8074

#define ARM_SMMU_S_STRTAB_BASE		0x8080
#define S_STRTAB_BASE_RA_SHIFT		62
#define S_STRTAB_BASE_RA			(1UL << S_STRTAB_BASE_RA_SHIFT)
#define S_STRTAB_BASE_ADDR_MASK		GENMASK_ULL(51, 6)

#define ARM_SMMU_S_STRTAB_BASE_CFG	0x8088
#define S_STRTAB_BASE_CFG_FMT		GENMASK(17, 16)
#define S_STRTAB_BASE_CFG_SPLIT		GENMASK(10, 6)
#define S_STRTAB_BASE_CFG_LOG2SIZE	GENMASK(5, 0)

#define ARM_SMMU_S_CMDQ_BASE		0x8090
#define ARM_SMMU_S_CMDQ_PROD		0x8098
#define ARM_SMMU_S_CMDQ_CONS		0x809c
#define S_CMDQ_BASE_ADDR_MASK		GENMASK_ULL(51, 5)
#define S_CMDQ_BASE_RA_SHIFT		62

#define ARM_SMMU_S_EVTQ_BASE		0x80a0
#define ARM_SMMU_S_EVTQ_PROD		0x80a8
#define ARM_SMMU_S_EVTQ_CONS		0x80ac
#define ARM_SMMU_S_EVTQ_IRQ_CFG0	0x80b0
#define ARM_SMMU_S_EVTQ_IRQ_CFG1	0x80b8
#define ARM_SMMU_S_EVTQ_IRQ_CFG2	0x80bc
#define S_EVTQ_BASE_ADDR_MASK		GENMASK_ULL(51, 5)
#define S_EVTQ_BASE_WA_SHIFT		62

/*
 * BIT1 is PRIQEN, BIT4 is ATSCHK in SMMU_CRO
 * BIT1 and BIT4 are RES0 in SMMU_S_CRO
 */
#define SMMU_S_CR0_RESERVED 0xFFFFFC12

int get_root_bd(struct device *dev);
void get_child_devices_rec(struct pci_dev *dev, uint16_t *devs, int max_devs, int *ndev);
int get_sibling_devices(struct device *dev, uint16_t *devs, int max_devs);
void set_g_cc_dev(u32 sid, u32 vmid, u32 root_bd, bool secure);
void free_g_cc_dev_htable(void);
/* Has the root bus device number switched to secure? */
bool is_cc_root_bd(u32 root_bd);
bool is_cc_dev(u32 sid);

void s_smmu_cmdq_need_forward(u64 cmd0, u64 cmd1, u64 *forward);
void s_queue_write(struct arm_smmu_device *smmu, u64 *src, size_t n_dwords);
void arm_s_smmu_cmdq_write_entries(struct arm_smmu_device *smmu, u64 *cmds, int n);
int arm_s_smmu_init_one_queue(struct arm_smmu_device *smmu,
							  struct arm_smmu_queue *q,
							  size_t qsz, const char *name);
int arm_smmu_write_s_reg_sync(struct arm_smmu_device *smmu, u32 val, u32 cmp_val,
				   unsigned int reg_off, unsigned int ack_off);
int arm_smmu_update_s_gbpa(struct arm_smmu_device *smmu, u32 set, u32 clr);

irqreturn_t arm_smmu_s_evtq_thread(int irq, void *dev);
irqreturn_t arm_smmu_s_gerror_handler(int irq, void *dev);
int arm_smmu_disable_s_irq(struct arm_smmu_device *smmu);
int arm_smmu_enable_s_irq(struct arm_smmu_device *smmu, u32 irqen_flags);
void arm_s_smmu_setup_unique_irqs(struct arm_smmu_device *smmu);
void arm_smmu_write_s_msi_msg(struct arm_smmu_device *smmu, phys_addr_t *cfg,
	struct msi_msg *msg, phys_addr_t doorbell);
void platform_get_s_irq_byname_optional(struct platform_device *pdev, struct arm_smmu_device *smmu);
int arm_smmu_enable_secure(struct iommu_domain *domain);
u32 arm_smmu_tmi_dev_attach(struct arm_smmu_domain *arm_smmu_domain,
	struct kvm *kvm);
int arm_smmu_secure_dev_ste_create(struct arm_smmu_device *smmu,
	struct arm_smmu_master *master, u32 sid);
int arm_smmu_secure_set_dev(struct arm_smmu_domain *smmu_domain, struct arm_smmu_master *master,
	 struct device *dev);

int arm_smmu_id_alloc(void);
void arm_smmu_id_free(int idx);
void arm_smmu_map_init(struct arm_smmu_device *smmu, resource_size_t ioaddr);
int arm_s_smmu_device_disable(struct arm_smmu_device *smmu);
int arm_s_smmu_device_reset(struct arm_smmu_device *smmu);
int arm_s_smmu_device_enable(struct arm_smmu_device *smmu,
	u32 enables, bool bypass, bool disable_bypass);
int arm_smmu_s_idr1_support_secure(struct arm_smmu_device *smmu);

#endif /* _ARM_S_SMMU_V3_H */
