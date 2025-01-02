/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 Huawei Technologies Co., Ltd */

#ifndef HINIC_HMM_H__
#define HINIC_HMM_H__

/* has no mpt entry */
#define HMM_MPT_EN_SW 1	/* has mpt, state INVALID */
#define HMM_MPT_EN_HW 2	/* has mpt, state FREE or VALID */
#define HMM_MPT_DISABLED 0 /* has no mpt entry */
#define HMM_MPT_FIX_BUG_LKEY 0

#include "hinic3_cqm.h"
#include "hinic3_hwdev.h"
#include "hmm_comp.h"
#include "hmm_mr.h"

/* Prototype	: hmm_reg_user_mr_update
 * Description  : Update the MR entry in the MPT and MTT tables
 * Input: struct hinic3_hwdev *hwdev - Device structure pointer
 *		hmm_mr *mr -
 *		MR structure, containing the completed user-mode memory physical address
 *		acquisition umem
 *		u32 pdn -
 *      PD number, if the feature of the unsupported pd is directly filled with 0.
 *		u64 length -
 *      The length of the user-mode address that needs to be registered
 *		u64 virt_addr -
 *		The first virtual address of the IOV virtual address that needs to be registered
 *		int hmm_access -
 *		Fill in the value of enum rdma_ib_access, describing the access permission
 *		u32 service_type -
 *      The value of enum hinic3_service_type, describing the service type
 * Output	   : None
 */
int hmm_reg_user_mr_update(struct hinic3_hwdev *hwdev, struct hmm_mr *mr, u32 pdn, u64 length,
	u64 virt_addr, int access, u32 service_type, u16 channel);

/* Prototype	: hmm_reg_user_mr_update
 * Description  : Remove the MR entry from the MPT and MTT tables
 * Input: struct hinic3_hwdev *hwdev - Device structure pointer
 *		rdma_mr *mr - MR structure
 *		u32 service_type - enum hinic3_service_type value, describes the service type
 * Output	   : None
 */
int hmm_dereg_mr_update(struct hinic3_hwdev *hwdev, struct rdma_mr *mr,
	u32 service_type, u16 channel);

#ifndef ROCE_SERVICE
/* Prototype	: hmm_reg_user_mr
 * Description  : Register an MR for the user
 * Input: struct hinic3_hwdev *hwdev - Device structure pointer
 *		u64 start - The starting address of the memory region
 *		u32 pdn - PD number
 *		u64 length - The length of the memory region
 *		u64 virt_addr - The virtual address of the IOV
 *		int hmm_access -
 *		The access permission, filled in with the value of enum rdma_ib_access
 *		u32 service_type -
 *		The service type, filled in with the value of enum hinic3_service_type
 * Output: struct hmm_mr * - Returns a pointer to the newly registered MR structure
 */
struct hmm_mr *hmm_reg_user_mr(struct hinic3_hwdev *hwdev, u64 start, u32 pdn, u64 length,
	u64 virt_addr, int hmm_access, u32 service_type, u16 channel);

/* Prototype	: hmm_dereg_mr
 * Description  : Deregister DMA_MR, user_MR, or FRMR
 * Input: struct hmm_mr *mr - MR structure
 *	u32 service_type - enum hinic3_service_type value, describes the service type
 * Output	  : None
 */
int hmm_dereg_mr(struct hmm_mr *mr, u32 service_type, u16 channel);
#endif

int hmm_rdma_write_mtt(void *hwdev, struct rdma_mtt *mtt, u32 start_index, u32 npages,
	u64 *page_list, u32 service_type);

int hmm_rdma_mtt_alloc(void *hwdev, u32 npages, u32 page_shift,
	struct rdma_mtt *mtt, u32 service_type);

void hmm_rdma_mtt_free(void *hwdev, struct rdma_mtt *mtt, u32 service_type);

int hmm_init_mtt_table(struct hmm_comp_priv *comp_priv);

void hmm_cleanup_mtt_table(struct hmm_comp_priv *comp_priv);

#endif /* HINIC_RDMA_H__ */
