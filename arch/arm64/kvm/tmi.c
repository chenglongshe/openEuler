// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 */
#include <linux/arm-smccc.h>
#include <asm/kvm_tmi.h>
#include <asm/memory.h>

u64 tmi_version(void)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_VERSION_REQ, &res);
	return res.a1;
}

u64 tmi_data_create(u64 numa_set, u64 rd, u64 map_addr, u64 src, u64 level)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_DATA_CREATE, numa_set, rd, map_addr, src, level, &res);
	return res.a1;
}

u64 tmi_data_destroy(u64 rd, u64 map_addr, u64 level)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_DATA_DESTROY, rd, map_addr, level, &res);
	return res.a1;
}

u64 tmi_cvm_activate(u64 rd)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_CVM_ACTIVATE, rd, &res);
	return res.a1;
}

u64 tmi_cvm_create(u64 params_ptr, u64 numa_set)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_CVM_CREATE, params_ptr, numa_set, &res);
	return res.a1;
}

u64 tmi_cvm_destroy(u64 rd)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_CVM_DESTROY, rd, &res);
	return res.a1;
}

u64 tmi_tec_create(u64 numa_set, u64 rd, u64 mpidr, u64 params_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TEC_CREATE, numa_set, rd, mpidr, params_ptr, &res);
	return res.a1;
}

u64 tmi_tec_destroy(u64 tec)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TEC_DESTROY, tec, &res);
	return res.a1;
}

u64 tmi_tec_enter(u64 tec, u64 run_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TEC_ENTER, tec, run_ptr, &res);
	return res.a1;
}

u64 tmi_ttt_create(u64 numa_set, u64 rd, u64 map_addr, u64 level)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TTT_CREATE, numa_set, rd, map_addr, level, &res);
	return res.a1;
}

u64 tmi_psci_complete(u64 calling_tec, u64 target_tec)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_PSCI_COMPLETE, calling_tec, target_tec, &res);
	return res.a1;
}

u64 tmi_features(u64 index)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_FEATURES, index, &res);
	return res.a1;
}

u64 tmi_mem_info_show(u64 mem_info_addr)
{
	struct arm_smccc_res res;
	u64 pa_addr = __pa(mem_info_addr);

	arm_smccc_1_1_smc(TMI_TMM_MEM_INFO_SHOW, pa_addr, &res);
	return res.a1;
}
EXPORT_SYMBOL_GPL(tmi_mem_info_show);

u64 tmi_ttt_map_range(u64 rd, u64 map_addr, u64 size, u64 cur_node, u64 target_node)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TTT_MAP_RANGE, rd, map_addr, size, cur_node, target_node, &res);
	return res.a1;
}

u64 tmi_ttt_unmap_range(u64 rd, u64 map_addr, u64 size, u64 node_id)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_TTT_UNMAP_RANGE, rd, map_addr, size, node_id, &res);
	return res.a1;
}

u64 tmi_tmm_inf_test(u64 x1, u64 x2, u64 x3, u64 x4, u64 x5)
{
	struct arm_smccc_res res;
	u64 vttbr_el2_pa = __pa(x2);
	u64 cvm_params_pa = __pa(x3);
	u64 tec_params_pa = __pa(x4);

	arm_smccc_1_1_smc(TMI_TMM_INF_TEST, x1, vttbr_el2_pa, cvm_params_pa, tec_params_pa, x5, &res);
	return res.a1;
}
EXPORT_SYMBOL_GPL(tmi_tmm_inf_test);

u64 tmi_smmu_queue_create(u64 params_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_QUEUE_CREATE, params_ptr, &res);
	return res.a1;
}
EXPORT_SYMBOL_GPL(tmi_smmu_queue_create);


u64 tmi_smmu_queue_write(uint64_t cmd0, uint64_t cmd1, u64 smmu_id)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_QUEUE_WRITE, cmd0, cmd1, smmu_id, &res);
	return res.a1;
}
EXPORT_SYMBOL_GPL(tmi_smmu_queue_write);

u64 tmi_smmu_ste_create(u64 params_ptr)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_STE_CREATE, params_ptr, &res);
	return res.a1;
}
EXPORT_SYMBOL_GPL(tmi_smmu_ste_create);

u64 tmi_mmio_map(u64 rd, u64 map_addr, u64 level, u64 ttte)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_MMIO_MAP, rd, map_addr, level, ttte, &res);
	return res.a1;
}

u64 tmi_mmio_unmap(u64 rd, u64 map_addr, u64 level)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_MMIO_UNMAP, rd, map_addr, level, &res);
	return res.a1;
}

u64 tmi_mmio_write(u64 addr, u64 val, u64 bits, u64 dev_num)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_MMIO_WRITE, addr, val, bits, dev_num, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_mmio_write);

u64 tmi_mmio_read(u64 addr, u64 bits, u64 dev_num)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_MMIO_READ, addr, bits, dev_num, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_mmio_read);

u64 tmi_dev_delegate(u64 params)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_DEV_DELEGATE, params, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_dev_delegate);

u64 tmi_dev_attach(u64 vdev, u64 rd, u64 smmu_id)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_DEV_ATTACH, vdev, rd, smmu_id, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_dev_attach);

u64 tmi_handle_s_evtq(u64 smmu_id)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_HANDLE_S_EVTQ, smmu_id, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_handle_s_evtq);

u64 tmi_smmu_device_reset(u64 params)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_DEVICE_RESET, params, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_smmu_device_reset);

u64 tmi_smmu_pcie_core_check(u64 smmu_base)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_PCIE_CORE_CHECK, smmu_base, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_smmu_pcie_core_check);

u64 tmi_smmu_write(u64 smmu_base, u64 reg_offset, u64 val, u64 bits)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_WRITE, smmu_base, reg_offset, val, bits, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_smmu_write);

u64 tmi_smmu_read(u64 smmu_base, u64 reg_offset, u64 bits)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(TMI_TMM_SMMU_READ, smmu_base, reg_offset, bits, &res);
	return res.a1;
}
EXPORT_SYMBOL(tmi_smmu_read);

