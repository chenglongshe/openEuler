/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023-2023 Broadcom
 * All rights reserved.
 */

#ifndef _BNXT_TF_TC_SHIM_H_
#define _BNXT_TF_TC_SHIM_H_

#ifdef CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD
#include "ulp_linux.h"
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_tf_common.h"
#include "ulp_mapper.h"

/* Internal Tunnel type, */
enum bnxt_global_register_tunnel_type {
	BNXT_GLOBAL_REGISTER_TUNNEL_UNUSED = 0,
	BNXT_GLOBAL_REGISTER_TUNNEL_VXLAN,
	BNXT_GLOBAL_REGISTER_TUNNEL_ECPRI,
	BNXT_GLOBAL_REGISTER_TUNNEL_VXLAN_GPE,
	BNXT_GLOBAL_REGISTER_TUNNEL_VXLAN_GPE_V6,
	BNXT_GLOBAL_REGISTER_TUNNEL_MAX
};

#define BNXT_VNIC_MAX_QUEUE_SIZE        256
#define BNXT_VNIC_MAX_QUEUE_SZ_IN_8BITS (BNXT_VNIC_MAX_QUEUE_SIZE / 8)
#define BNXT_VNIC_MAX_QUEUE_SZ_IN_64BITS (BNXT_VNIC_MAX_QUEUE_SIZE / 64)

int32_t bnxt_pmd_queue_action_create(struct bnxt_ulp_mapper_parms *parms,
				     uint16_t *vnic_idx, uint16_t *vnic_id);
int32_t bnxt_pmd_queue_action_delete(struct tf *tfp, uint16_t vnic_idx);
int32_t bnxt_pmd_bd_act_set(struct bnxt *bp, uint16_t port_id, uint32_t act);
#endif

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD)
int bnxt_ulp_tf_v6_subnet_add(struct bnxt *bp,
			      u8 *byte_key, u8 *byte_mask,
			      u8 *byte_data, u16 *subnet_hndl);

int bnxt_ulp_tf_v6_subnet_del(struct bnxt *bp, u16 subnet_hndl);
#endif /* #if defined(CONFIG_BNXT_FLOWER_OFFLOAD) */

#endif
