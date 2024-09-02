// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 */
#include <linux/crash_dump.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/iopoll.h>
#include <linux/pci.h>
#include <linux/hashtable.h>
#include <asm/kvm_tmi.h>

#include "arm-smmu-v3.h"
#include "arm-s-smmu-v3.h"
#include "../../dma-iommu.h"

struct cc_dev_config {
	u32	sid; /* BDF number of the device */
	u32	vmid;
	u32	root_bd; /* root bus and device number. Multiple sid can have the same root_bd. */
	bool secure;
	struct hlist_node node;
};

static bool g_smmu_id_map_init;

static DEFINE_HASHTABLE(g_cc_dev_htable, MAX_CC_DEV_NUM_ORDER);
static DECLARE_BITMAP(g_smmu_id_map, ARM_SMMU_MAX_IDS);

/* Traverse pcie topology to find the root <bus,device> number
 * return -1 if error
 * return -1 if not pci device
 */
int get_root_bd(struct device *dev)
{
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return -1;
	pdev = to_pci_dev(dev);
	if (pdev->bus == NULL)
		return -1;
	while (pdev->bus->parent != NULL)
		pdev = pdev->bus->self;

	return pci_dev_id(pdev) & 0xfff8;
}

/* if dev is a bridge, get all it's children.
 * if dev is a regular device, get itself.
 */
void get_child_devices_rec(struct pci_dev *dev, uint16_t *devs, int max_devs, int *ndev)
{
	struct pci_bus *bus = dev->subordinate;

	if (bus) { /* dev is a bridge */
		struct pci_dev *child;

		list_for_each_entry(child, &bus->devices, bus_list) {
			get_child_devices_rec(child, devs, max_devs, ndev);
		}
	} else { /* dev is a regular device */
		uint16_t bdf = pci_dev_id(dev);
		int i;
		/* check if bdf is already in devs */
		for (i = 0; i < *ndev; i++) {
			if (devs[i] == bdf)
				return;
		}
		/* check overflow */
		if (*ndev >= max_devs) {
			WARN_ON(1);
			return;
		}
		devs[*ndev] = bdf;
		*ndev = *ndev + 1;
	}
}

/* get all devices which share the same root_bd as dev
 * return 0 on failure
 */
int get_sibling_devices(struct device *dev, uint16_t *devs, int max_devs)
{
	int ndev = 0;
	struct pci_dev *pdev;

	if (!dev_is_pci(dev))
		return ndev;

	pdev = to_pci_dev(dev);
	if (pdev->bus == NULL)
		return ndev;

	while (pdev->bus->parent != NULL)
		pdev = pdev->bus->self;

	get_child_devices_rec(pdev, devs, max_devs, &ndev);
	return ndev;
}

void set_g_cc_dev(u32 sid, u32 vmid, u32 root_bd, bool secure)
{
	struct cc_dev_config *obj;

	hash_for_each_possible(g_cc_dev_htable, obj, node, sid) {
		if (obj->sid == sid) {
			obj->vmid = vmid;
			obj->root_bd = root_bd;
			obj->secure = secure;
			return;
		}
	}

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		WARN_ON(1);
		return;
	}

	obj->sid = sid;
	obj->vmid = vmid;
	obj->root_bd = root_bd;
	obj->secure = secure;

	hash_add(g_cc_dev_htable, &obj->node, sid);
}

void free_g_cc_dev_htable(void)
{
	int i;
	struct cc_dev_config *obj;
	struct hlist_node *tmp;

	hash_for_each_safe(g_cc_dev_htable, i, tmp, obj, node) {
		hash_del(&obj->node);
		kfree(obj);
	}
}

/* Has the root bus device number switched to secure? */
bool is_cc_root_bd(u32 root_bd)
{
	int bkt;
	struct cc_dev_config *obj;

	hash_for_each(g_cc_dev_htable, bkt, obj, node) {
		if (obj->root_bd == root_bd && obj->secure)
			return true;
	}

	return false;
}

static bool is_cc_vmid(u32 vmid)
{
	int bkt;
	struct cc_dev_config *obj;

	hash_for_each(g_cc_dev_htable, bkt, obj, node) {
		if (vmid > 0 && obj->vmid == vmid)
			return true;
	}

	return false;
}

bool is_cc_dev(u32 sid)
{
	struct cc_dev_config *obj;

	hash_for_each_possible(g_cc_dev_htable, obj, node, sid) {
		if (obj != NULL && obj->sid == sid)
			return obj->secure;
	}

	return false;
}
EXPORT_SYMBOL(is_cc_dev);

void s_smmu_cmdq_need_forward(u64 cmd0, u64 cmd1, u64 *forward)
{
	u64 opcode = FIELD_GET(CMDQ_0_OP, cmd0);

	switch (opcode) {
	case CMDQ_OP_TLBI_EL2_ALL:
	case CMDQ_OP_TLBI_NSNH_ALL:
		*forward = 1;
		break;
	case CMDQ_OP_PREFETCH_CFG:
	case CMDQ_OP_CFGI_CD:
	case CMDQ_OP_CFGI_STE:
	case CMDQ_OP_CFGI_CD_ALL:
		*forward = (uint64_t)is_cc_dev(FIELD_GET(CMDQ_CFGI_0_SID, cmd0));
		break;

	case CMDQ_OP_CFGI_ALL:
		*forward = 1;
		break;
	case CMDQ_OP_TLBI_NH_VA:
	case CMDQ_OP_TLBI_S2_IPA:
	case CMDQ_OP_TLBI_NH_ASID:
	case CMDQ_OP_TLBI_S12_VMALL:
		*forward = (uint64_t)is_cc_vmid(FIELD_GET(CMDQ_TLBI_0_VMID, cmd0));
		break;
	case CMDQ_OP_TLBI_EL2_VA:
	case CMDQ_OP_TLBI_EL2_ASID:
		*forward = 0;
		break;
	case CMDQ_OP_ATC_INV:
		*forward = (uint64_t)is_cc_dev(FIELD_GET(CMDQ_ATC_0_SID, cmd0));
		break;
	case CMDQ_OP_PRI_RESP:
		*forward = (uint64_t)is_cc_dev(FIELD_GET(CMDQ_PRI_0_SID, cmd0));
		break;
	case CMDQ_OP_RESUME:
		*forward = (uint64_t)is_cc_dev(FIELD_GET(CMDQ_RESUME_0_SID, cmd0));
		break;
	case CMDQ_OP_CMD_SYNC:
		*forward = 0;
		break;
	default:
		*forward = 0;
	}
}

void s_queue_write(struct arm_smmu_device *smmu, u64 *src, size_t n_dwords)
{
	u64 cmd0, cmd1;
	u64 forward = 0;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		if (n_dwords == 2) {
			cmd0 = cpu_to_le64(src[0]);
			cmd1 = cpu_to_le64(src[1]);
			s_smmu_cmdq_need_forward(cmd0, cmd1, &forward);

			/* need forward queue command to TMM */
			if (forward) {
				if (tmi_smmu_queue_write(cmd0, cmd1, smmu->id))
					pr_err("tmi_smmu_queue_write err!\n");
			}
		}
	}
}

void arm_s_smmu_cmdq_write_entries(struct arm_smmu_device *smmu, u64 *cmds, int n)
{
	int i;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		for (i = 0; i < n; ++i) {
			u64 *cmd = &cmds[i * CMDQ_ENT_DWORDS];

			s_queue_write(smmu, cmd, CMDQ_ENT_DWORDS);
		}
	}
}

int arm_s_smmu_init_one_queue(struct arm_smmu_device *smmu,
							  struct arm_smmu_queue *q,
							  size_t qsz, const char *name)
{
	if (smmu->id != ARM_SMMU_INVALID_ID) {
		struct tmi_smmu_queue_params *params_ptr = kzalloc(PAGE_SIZE, GFP_KERNEL_ACCOUNT);

		if (!params_ptr)
			return -ENOMEM;

		if (!strcmp(name, "cmdq")) {
			params_ptr->ns_src = q->base_dma;
			params_ptr->smmu_base_addr = smmu->ioaddr;
			params_ptr->size = qsz;
			params_ptr->smmu_id = smmu->id;
			params_ptr->type = TMI_SMMU_CMD_QUEUE;
			tmi_smmu_queue_create(__pa(params_ptr));
		}

		if (!strcmp(name, "evtq")) {
			params_ptr->ns_src = q->base_dma;
			params_ptr->smmu_base_addr = smmu->ioaddr;
			params_ptr->size = qsz;
			params_ptr->smmu_id = smmu->id;
			params_ptr->type = TMI_SMMU_EVT_QUEUE;
			tmi_smmu_queue_create(__pa(params_ptr));
		}

		kfree(params_ptr);
	}

	return 0;
}

int arm_smmu_write_s_reg_sync(struct arm_smmu_device *smmu, u32 val, u32 cmp_val,
				   unsigned int reg_off, unsigned int ack_off)
{
	u32 reg;

	if (tmi_smmu_write(smmu->ioaddr, reg_off, val, 32))
		return -ENXIO;

	return kvm_cvm_read_poll_timeout_atomic(tmi_smmu_read, reg, reg == cmp_val,
				       1, ARM_SMMU_POLL_TIMEOUT_US, false,
				       smmu->ioaddr, ack_off, 32);
}

int arm_smmu_update_s_gbpa(struct arm_smmu_device *smmu, u32 set, u32 clr)
{
	int ret;
	u32 reg;

	ret = kvm_cvm_read_poll_timeout_atomic(tmi_smmu_read, reg, !(reg & S_GBPA_UPDATE),
				       1, ARM_SMMU_POLL_TIMEOUT_US, false,
				       smmu->ioaddr, ARM_SMMU_S_GBPA, 32);
	if (ret)
		return ret;

	reg &= ~clr;
	reg |= set;

	ret = tmi_smmu_write(smmu->ioaddr, ARM_SMMU_S_GBPA, reg | S_GBPA_UPDATE, 32);
	if (ret)
		return ret;

	ret = kvm_cvm_read_poll_timeout_atomic(tmi_smmu_read, reg, !(reg & S_GBPA_UPDATE),
			1, ARM_SMMU_POLL_TIMEOUT_US, false,
			smmu->ioaddr, ARM_SMMU_S_GBPA, 32);
	if (ret)
		dev_err(smmu->dev, "S_GBPA not responding to update\n");
	return ret;
}

irqreturn_t arm_smmu_s_evtq_thread(int irq, void *dev)
{
	struct arm_smmu_device *smmu = dev;

	if (smmu->id != ARM_SMMU_INVALID_ID)
		tmi_handle_s_evtq(smmu->id);

	return IRQ_HANDLED;
}

irqreturn_t arm_smmu_s_gerror_handler(int irq, void *dev)
{
	u32 gerror, gerrorn, active;
	u64 ret;
	struct arm_smmu_device *smmu = dev;

	ret = tmi_smmu_read(smmu->ioaddr, ARM_SMMU_S_GERROR, 32);
	if (ret >> 32) {
		dev_err(smmu->dev, "Get ARM_SMMU_S_GERROR register failed\n");
		return IRQ_NONE;
	}
	gerror = (u32)ret;

	ret = tmi_smmu_read(smmu->ioaddr, ARM_SMMU_S_GERRORN, 32);
	if (ret >> 32) {
		dev_err(smmu->dev, "Get ARM_SMMU_S_GERRORN register failed\n");
		return IRQ_NONE;
	}
	gerrorn = (u32)ret;

	active = gerror ^ gerrorn;
	if (!(active & GERROR_ERR_MASK))
		return IRQ_NONE; /* No errors pending */

	dev_warn(smmu->dev,
		 "unexpected secure global error reported, this could be serious, active %x\n",
		 active);

	if (active & GERROR_SFM_ERR) {
		dev_err(smmu->dev, "device has entered Service Failure Mode!\n");
		arm_s_smmu_device_disable(smmu);
	}

	if (active & GERROR_MSI_GERROR_ABT_ERR)
		dev_warn(smmu->dev, "GERROR MSI write aborted\n");

	if (active & GERROR_MSI_PRIQ_ABT_ERR)
		dev_warn(smmu->dev, "PRIQ MSI write aborted\n");

	if (active & GERROR_MSI_EVTQ_ABT_ERR)
		dev_warn(smmu->dev, "EVTQ MSI write aborted\n");

	if (active & GERROR_MSI_CMDQ_ABT_ERR)
		dev_warn(smmu->dev, "CMDQ MSI write aborted\n");

	if (active & GERROR_PRIQ_ABT_ERR)
		dev_err(smmu->dev, "PRIQ write aborted -- events may have been lost\n");

	if (active & GERROR_EVTQ_ABT_ERR)
		dev_err(smmu->dev, "EVTQ write aborted -- events may have been lost\n");

	if (active & GERROR_CMDQ_ERR)
		dev_warn(smmu->dev, "CMDQ ERR\n");

	if (tmi_smmu_write(smmu->ioaddr, ARM_SMMU_S_GERRORN, gerror, 32)) {
		dev_err(smmu->dev, "SMMU write ARM_SMMU_S_GERRORN failed\n");
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

int arm_smmu_disable_s_irq(struct arm_smmu_device *smmu)
{
	int ret;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		ret = arm_smmu_write_s_reg_sync(smmu, 0, 0,
			ARM_SMMU_S_IRQ_CTRL, ARM_SMMU_S_IRQ_CTRLACK);
		if (ret) {
			dev_err(smmu->dev, "failed to disable secure irqs\n");
			return ret;
		}
	}

	return 0;
}

int arm_smmu_enable_s_irq(struct arm_smmu_device *smmu, u32 irqen_flags)
{
	int ret;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		ret = arm_smmu_write_s_reg_sync(smmu, irqen_flags,
			irqen_flags, ARM_SMMU_S_IRQ_CTRL, ARM_SMMU_S_IRQ_CTRLACK);
		if (ret) {
			dev_err(smmu->dev, "failed to enable irq for secure evtq\n");
			return ret;
		}
	}

	return 0;
}

void arm_s_smmu_setup_unique_irqs(struct arm_smmu_device *smmu)
{
	int irq, ret;

	irq = smmu->s_evtq_irq;
	if (irq && smmu->id != ARM_SMMU_INVALID_ID) {
		ret = devm_request_threaded_irq(smmu->dev, irq, NULL,
						arm_smmu_s_evtq_thread,
						IRQF_ONESHOT,
						"arm-smmu-v3-s_evtq", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable s_evtq irq\n");
	} else {
		dev_warn(smmu->dev, "no s_evtq irq - events will not be reported!\n");
	}

	irq = smmu->s_gerr_irq;
	if (irq && smmu->id != ARM_SMMU_INVALID_ID) {
		ret = devm_request_irq(smmu->dev, irq, arm_smmu_s_gerror_handler,
				       0, "arm-smmu-v3-s_gerror", smmu);
		if (ret < 0)
			dev_warn(smmu->dev, "failed to enable s_gerror irq\n");
	} else {
		dev_warn(smmu->dev, "no s_gerr irq - errors will not be reported!\n");
	}
}

void arm_smmu_write_s_msi_msg(struct arm_smmu_device *smmu, phys_addr_t *cfg,
							  struct msi_msg *msg, phys_addr_t doorbell)
{
	tmi_smmu_write((u64)smmu->ioaddr, cfg[0], doorbell, 64);
	tmi_smmu_write((u64)smmu->ioaddr, cfg[1], msg->data, 32);
	tmi_smmu_write((u64)smmu->ioaddr, cfg[2], ARM_SMMU_MEMATTR_DEVICE_nGnRE, 32);
}

void platform_get_s_irq_byname_optional(struct platform_device *pdev, struct arm_smmu_device *smmu)
{
	int irq;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		irq = platform_get_irq_byname_optional(pdev, "s_eventq");
		if (irq > 0)
			smmu->s_evtq_irq = irq;

		irq = platform_get_irq_byname_optional(pdev, "s_gerror");
		if (irq > 0)
			smmu->s_gerr_irq = irq;
	}
}

int arm_smmu_enable_secure(struct iommu_domain *domain)
{
	int ret = 0;
	struct arm_smmu_domain *smmu_domain = to_smmu_domain(domain);

	mutex_lock(&smmu_domain->init_mutex);
	if (smmu_domain->smmu)
		ret = -EPERM;
	else
		smmu_domain->secure = true;
	mutex_unlock(&smmu_domain->init_mutex);

	return ret;
}

u32 arm_smmu_tmi_dev_attach(struct arm_smmu_domain *arm_smmu_domain,
	struct kvm *kvm)
{
	int ret = -1;
	u64 cmd[CMDQ_ENT_DWORDS] = {0};
	unsigned long flags;
	int i, j;
	struct arm_smmu_master *master;
	struct virtcca_cvm *virtcca_cvm = (struct virtcca_cvm *)kvm->arch.virtcca_cvm;

	spin_lock_irqsave(&arm_smmu_domain->devices_lock, flags);
	list_for_each_entry(master, &arm_smmu_domain->devices, domain_head) {
		if (master && master->num_streams >= 0) {
			for (i = 0; i < master->num_streams; ++i) {
				u32 sid = master->streams[i].id;

				for (j = 0; j < i; j++)
					if (master->streams[j].id == sid)
						break;
				if (j < i)
					continue;
				ret = tmi_dev_attach(sid, virtcca_cvm->rd,
					arm_smmu_domain->smmu->id);
				if (ret) {
					kvm_err("dev protected failed!\n");
					ret = -ENXIO;
					goto out;
				}
				cmd[0] |= FIELD_PREP(CMDQ_0_OP, CMDQ_OP_CFGI_STE);
				cmd[0] |= FIELD_PREP(CMDQ_CFGI_0_SID, sid);
				cmd[1] |= FIELD_PREP(CMDQ_CFGI_1_LEAF, true);
				tmi_smmu_queue_write(cmd[0], cmd[1], arm_smmu_domain->smmu->id);
			}
		}
	}

out:
	spin_unlock_irqrestore(&arm_smmu_domain->devices_lock, flags);
	return ret;
}

int arm_smmu_secure_dev_ste_create(struct arm_smmu_device *smmu,
	struct arm_smmu_master *master, u32 sid)
{
	struct tmi_smmu_ste_params *params_ptr;
	struct arm_smmu_strtab_cfg *cfg = &smmu->strtab_cfg;
	struct arm_smmu_strtab_l1_desc *desc = &cfg->l1_desc[sid >> STRTAB_SPLIT];

	params_ptr = kzalloc(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!params_ptr)
		return -ENOMEM;

	/* Sync Level 2 STE to TMM */
	params_ptr->ns_src = desc->l2ptr_dma + ((sid & ((1 << STRTAB_SPLIT) - 1)) * STE_ENTRY_SIZE);
	params_ptr->sid = sid;
	params_ptr->smmu_id = smmu->id;

	if (tmi_smmu_ste_create(__pa(params_ptr)) != 0)
		dev_err(smmu->dev, "failed to create STE level 2");

	kfree(params_ptr);

	return 0;
}

int arm_smmu_secure_set_dev(struct arm_smmu_domain *smmu_domain, struct arm_smmu_master *master,
	struct device *dev)
{
	int i, j;
	u64 ret = 0;
	uint16_t root_bd = get_root_bd(dev);

	WARN_ON_ONCE(root_bd < 0);
	if (!is_cc_root_bd(root_bd)) {
		struct tmi_dev_delegate_params *params = kzalloc(
			sizeof(struct tmi_dev_delegate_params), GFP_KERNEL);

		params->root_bd = root_bd;
		params->num_dev = get_sibling_devices(dev, params->devs, MAX_DEV_PER_PORT);
		WARN_ON_ONCE(params->num_dev == 0);
		pr_info("Delegate %d devices as %02x:%02x to secure\n",
				params->num_dev, root_bd >> 8, (root_bd & 0xff) >> 3);
		ret = tmi_dev_delegate(__pa(params));
		if (!ret) {
			for (i = 0; i < params->num_dev; i++)
				set_g_cc_dev(params->devs[i], 0, root_bd, true);
		}
		kfree(params);
	}

	if (ret)
		return ret;

	for (i = 0; i < master->num_streams; ++i) {
		u32 sid = master->streams[i].id;

		for (j = 0; j < i; j++)
			if (master->streams[j].id == sid)
				break;
		if (j < i)
			continue;
		WARN_ON_ONCE(!is_cc_dev(sid));
		set_g_cc_dev(sid, smmu_domain->s2_cfg.vmid, root_bd, true);
	}

	return ret;
}

int arm_smmu_id_alloc(void)
{
	int idx;

	do {
		idx = find_first_zero_bit(g_smmu_id_map, ARM_SMMU_MAX_IDS);
		if (idx == ARM_SMMU_MAX_IDS)
			return -ENOSPC;
	} while (test_and_set_bit(idx, g_smmu_id_map));

	return idx;
}

void arm_smmu_id_free(int idx)
{
	if (idx != ARM_SMMU_INVALID_ID)
		clear_bit(idx, g_smmu_id_map);
}

void arm_smmu_map_init(struct arm_smmu_device *smmu, resource_size_t ioaddr)
{
	if (!g_smmu_id_map_init) {
		set_bit(0, g_smmu_id_map);
		g_smmu_id_map_init = true;
	}
	smmu->ioaddr = ioaddr;

	if (virtcca_is_available() && tmi_smmu_pcie_core_check(ioaddr))
		smmu->id = arm_smmu_id_alloc();
	else
		smmu->id = ARM_SMMU_INVALID_ID;

	hash_init(g_cc_dev_htable);
}

int arm_s_smmu_device_disable(struct arm_smmu_device *smmu)
{
	int ret = 0;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		ret = arm_smmu_write_s_reg_sync(smmu, 0, 0, ARM_SMMU_S_CR0, ARM_SMMU_S_CR0ACK);
		if (ret) {
			dev_err(smmu->dev, "failed to clear s_cr0\n");
			return ret;
		}
	}

	return ret;
}

int arm_s_smmu_device_reset(struct arm_smmu_device *smmu)
{
	int ret;
	u64 rv;
	u32 reg, enables;
	struct tmi_smmu_cfg_params *params_ptr;

	if (smmu->id == ARM_SMMU_INVALID_ID)
		return 0;

	rv = tmi_smmu_read(smmu->ioaddr, ARM_SMMU_S_CR0, 32);
	if (rv >> 32)
		return -ENXIO;

	ret = (int)rv;
	if (ret & S_CR0_SMMUEN) {
		dev_warn(smmu->dev, "Secure SMMU currently enabled! Resetting...\n");
		arm_smmu_update_s_gbpa(smmu, S_GBPA_ABORT, 0);
	}

	ret = arm_s_smmu_device_disable(smmu);
	if (ret)
		return ret;

	/* CR1 (table and queue memory attributes) */
	reg = FIELD_PREP(CR1_TABLE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_TABLE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_TABLE_IC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_SH, ARM_SMMU_SH_ISH) |
	      FIELD_PREP(CR1_QUEUE_OC, CR1_CACHE_WB) |
	      FIELD_PREP(CR1_QUEUE_IC, CR1_CACHE_WB);

	ret = tmi_smmu_write(smmu->ioaddr, ARM_SMMU_S_CR1, reg, 32);
	if (ret)
		return ret;

	/* CR2 (random crap) */
	reg = CR2_PTM | CR2_RECINVSID;

	if (smmu->features & ARM_SMMU_FEAT_E2H)
		reg |= CR2_E2H;

	ret = tmi_smmu_write(smmu->ioaddr, ARM_SMMU_S_CR2, reg, 32);
	if (ret)
		return ret;

	params_ptr = kzalloc(PAGE_SIZE, GFP_KERNEL_ACCOUNT);
	if (!params_ptr)
		return -ENOMEM;

	params_ptr->is_cmd_queue = 1;
	params_ptr->smmu_id = smmu->id;
	params_ptr->ioaddr = smmu->ioaddr;
	params_ptr->strtab_base_RA_bit =
		(smmu->strtab_cfg.strtab_base >> S_STRTAB_BASE_RA_SHIFT) & 0x1;
	params_ptr->q_base_RA_WA_bit =
		(smmu->cmdq.q.q_base >> S_CMDQ_BASE_RA_SHIFT) & 0x1;
	if (tmi_smmu_device_reset(__pa(params_ptr)) != 0) {
		dev_err(smmu->dev, "failed to set s cmd queue regs\n");
		return -ENXIO;
	}

	enables = CR0_CMDQEN;
	ret = arm_smmu_write_s_reg_sync(smmu, enables, enables, ARM_SMMU_S_CR0,
				      ARM_SMMU_S_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to enable secure command queue\n");
		return ret;
	}

	enables |= CR0_EVTQEN;

	/* Secure event queue */
	memset(params_ptr, 0, sizeof(struct tmi_smmu_ste_params));
	params_ptr->is_cmd_queue = 0;
	params_ptr->ioaddr = smmu->ioaddr;
	params_ptr->smmu_id = smmu->id;
	params_ptr->q_base_RA_WA_bit =
		 (smmu->evtq.q.q_base >> S_EVTQ_BASE_WA_SHIFT) & 0x1;
	params_ptr->strtab_base_RA_bit =
		(smmu->strtab_cfg.strtab_base >> S_STRTAB_BASE_RA_SHIFT) & 0x1;
	if (tmi_smmu_device_reset(__pa(params_ptr)) != 0) {
		dev_err(smmu->dev, "failed to set s event queue regs");
		return -ENXIO;
	}

	/* Enable secure eventq */
	ret = arm_smmu_write_s_reg_sync(smmu, enables, enables, ARM_SMMU_S_CR0,
					ARM_SMMU_S_CR0ACK);
	if (ret) {
		dev_err(smmu->dev, "failed to disable secure event queue\n");
		return ret;
	}

	ret = arm_smmu_write_s_reg_sync(smmu, SMMU_S_INIT_INV_ALL, 0,
		ARM_SMMU_S_INIT, ARM_SMMU_S_INIT);
	if (ret) {
		dev_err(smmu->dev, "failed to write S_INIT\n");
		return ret;
	}

	return 0;
}

int arm_s_smmu_device_enable(struct arm_smmu_device *smmu,
	u32 enables, bool bypass, bool disable_bypass)
{
	int ret = 0;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		/* Enable the SMMU interface, or ensure bypass */
		if (!bypass || disable_bypass) {
			enables |= CR0_SMMUEN;
		} else {
			ret = arm_smmu_update_s_gbpa(smmu, 0, S_GBPA_ABORT);
			if (ret)
				return ret;
		}
		/* Mask BIT1 and BIT4 which are RES0 in SMMU_S_CRO */
		ret = arm_smmu_write_s_reg_sync(smmu, enables & ~SMMU_S_CR0_RESERVED,
			enables & ~SMMU_S_CR0_RESERVED, ARM_SMMU_S_CR0, ARM_SMMU_S_CR0ACK);
		dev_info(smmu->dev, "SMMUv3: Secure smmu id:%lld init end!\n", smmu->id);
	}
	return ret;
}

int arm_smmu_s_idr1_support_secure(struct arm_smmu_device *smmu)
{
	u32 ret;
	u64 rv;

	if (smmu->id != ARM_SMMU_INVALID_ID) {
		rv = tmi_smmu_read(smmu->ioaddr, ARM_SMMU_S_IDR1, 32);
		if (rv >> 32) {
			dev_err(smmu->dev, "Get ARM_SMMU_S_IDR1 register failed!\n");
			return -ENXIO;
		}
		ret = (u32)rv;
		if (!(ret & S_IDR1_SECURE_IMPL)) {
			dev_err(smmu->dev, "SMMUv3 does not implement secure state!\n");
			return -ENXIO;
		}

		if (!(ret & S_IDR1_SEL2)) {
			dev_err(smmu->dev, "SMMUv3: Secure stage2 translation not supported!\n");
			return -ENXIO;
		}
		dev_info(smmu->dev, "SMMUv3: Secure smmu id:%lld start init!\n", smmu->id);
	}

	return 0;
}
