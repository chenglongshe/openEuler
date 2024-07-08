// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "ulp_generic_flow_offload.h"
#include "ulp_udcc.h"
#include "bnxt_udcc.h"
#include "bnxt_debugfs.h"

#if defined(CONFIG_BNXT_FLOWER_OFFLOAD)

int bnxt_alloc_udcc_info(struct bnxt *bp)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;
	struct hwrm_udcc_qcaps_output *resp;
	struct hwrm_func_qcaps_input *req;
	int rc;

	if (BNXT_VF(bp) || !BNXT_UDCC_CAP(bp))
		return 0;

	if (udcc)
		return 0;

	rc = hwrm_req_init(bp, req, HWRM_UDCC_QCAPS);
	if (rc)
		return rc;

	req->fid = cpu_to_le16(0xffff);
	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (rc)
		goto exit;

	udcc = kzalloc(sizeof(*udcc), GFP_KERNEL);
	if (!udcc)
		goto exit;

	udcc->max_sessions = le16_to_cpu(resp->max_sessions);

	spin_lock_init(&udcc->session_db_lock);

	bp->udcc_info = udcc;
	netdev_dbg(bp->dev, "%s(): udcc_info initialized!\n", __func__);
exit:
	hwrm_req_drop(bp, req);
	return rc;
}

int bnxt_hwrm_udcc_session_query(struct bnxt *bp, u32 session_id,
				 struct hwrm_udcc_session_query_output *resp_out)
{
	struct hwrm_udcc_session_query_input *req;
	struct hwrm_udcc_session_query_output *resp;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_UDCC_SESSION_QUERY);
	if (rc)
		return rc;

	req->session_id = cpu_to_le16(session_id);

	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (rc)
		goto udcc_query_exit;

	memcpy(resp_out, resp, sizeof(struct hwrm_udcc_session_query_output));

udcc_query_exit:
	hwrm_req_drop(bp, req);
	return rc;
}

static int bnxt_hwrm_udcc_session_qcfg(struct bnxt *bp, struct bnxt_udcc_session_entry *entry)
{
	struct hwrm_udcc_session_qcfg_output *resp;
	struct hwrm_udcc_session_qcfg_input *req;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_UDCC_SESSION_QCFG);
	if (rc)
		return rc;

	req->session_id = cpu_to_le16(entry->session_id);

	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (rc)
		goto udcc_qcfg_exit;

	ether_addr_copy(entry->dest_mac, resp->dest_mac);
	ether_addr_copy(entry->src_mac, resp->src_mac);
	memcpy(entry->dst_ip.s6_addr32, resp->dest_ip, sizeof(resp->dest_ip));
	entry->dest_qp_num = le32_to_cpu(resp->dest_qp_num);
	entry->src_qp_num = le32_to_cpu(resp->src_qp_num);

udcc_qcfg_exit:
	hwrm_req_drop(bp, req);
	return rc;
}

static int bnxt_hwrm_udcc_session_cfg(struct bnxt *bp, struct bnxt_udcc_session_entry *entry)
{
	struct hwrm_udcc_session_cfg_input *req;
	int rc = 0;

	rc = hwrm_req_init(bp, req, HWRM_UDCC_SESSION_CFG);
	if (rc)
		return rc;

	req->session_id = cpu_to_le16(entry->session_id);
	req->enables = cpu_to_le32(UDCC_SESSION_CFG_REQ_ENABLES_SESSION_STATE |
				   UDCC_SESSION_CFG_REQ_ENABLES_DEST_MAC |
				   UDCC_SESSION_CFG_REQ_ENABLES_SRC_MAC |
				   UDCC_SESSION_CFG_REQ_ENABLES_TX_STATS_RECORD |
				   UDCC_SESSION_CFG_REQ_ENABLES_RX_STATS_RECORD);
	ether_addr_copy(req->dest_mac, entry->dst_mac_mod);
	ether_addr_copy(req->src_mac, entry->src_mac_mod);
	req->tx_stats_record = cpu_to_le32((u32)entry->tx_counter_hndl);
	req->rx_stats_record = cpu_to_le32((u32)entry->rx_counter_hndl);
	req->session_state = UDCC_SESSION_CFG_REQ_SESSION_STATE_ENABLED;

	return hwrm_req_send(bp, req);
}

int bnxt_tf_ulp_flow_create(struct bnxt *bp, struct bnxt_udcc_session_entry *entry)
{
	struct bnxt_ulp_gen_bth_hdr bth_spec = { 0 }, bth_mask = { 0 };
	struct bnxt_ulp_gen_ipv6_hdr v6_spec = { 0 }, v6_mask = { 0 };
	struct bnxt_ulp_gen_l2_hdr_parms l2_parms = { 0 };
	struct bnxt_ulp_gen_l3_hdr_parms l3_parms = { 0 };
	struct bnxt_ulp_gen_l4_hdr_parms l4_parms = { 0 };
	struct bnxt_ulp_gen_action_parms actions = { 0 };
	struct bnxt_ulp_gen_flow_parms parms = { 0 };
	int rc;

	/* These would normally be preset and passed to the upper layer */
	u8 l4_proto = IPPROTO_UDP;
	u8 l4_proto_mask = 0xff;
	u16 op_code = cpu_to_be16(0x81); /* RoCE CNP */
	u16 op_code_mask = cpu_to_be16(0xffff);
	u8 bnxt_ulp_gen_l3_ipv6_addr_em_mask[] = { 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff
	};

	/* Pack the L2 Data - Don't fill l2_spec for now as per TF team */
	l2_parms.type = BNXT_ULP_GEN_L2_L2_HDR;

	/* Pack the L3 Data */
	v6_spec.proto6 = &l4_proto;
	v6_mask.proto6 = &l4_proto_mask;
	v6_spec.dip6 = NULL;
	v6_mask.dip6 = NULL;
	v6_spec.sip6 = entry->dst_ip.s6_addr;
	v6_mask.sip6 = bnxt_ulp_gen_l3_ipv6_addr_em_mask;

	l3_parms.type = BNXT_ULP_GEN_L3_IPV6;
	l3_parms.v6_spec = &v6_spec;
	l3_parms.v6_mask = &v6_mask;

	/* Pack the L4 Data */
	bth_spec.op_code = &op_code;
	bth_mask.op_code = &op_code_mask;

	l4_parms.type = BNXT_ULP_GEN_L4_BTH;
	l4_parms.bth_spec = &bth_spec;
	l4_parms.bth_mask = &bth_mask;

	actions.enables = BNXT_ULP_GEN_ACTION_ENABLES_REDIRECT |
		BNXT_ULP_GEN_ACTION_ENABLES_COUNT;
	actions.dst_fid = bp->pf.fw_fid;

	parms.dir = BNXT_ULP_GEN_RX;
	parms.flow_id = &entry->rx_flow_id;
	parms.counter_hndl = &entry->rx_counter_hndl;
	parms.l2 = &l2_parms;
	parms.l3 = &l3_parms;
	parms.l4 = &l4_parms;
	parms.actions = &actions;

	rc = bnxt_ulp_gen_flow_create(bp, bp->pf.fw_fid, &parms);

	netdev_dbg(bp->dev, "ADD: Session ID = %d Ingress Flow ID = %d, Counter = 0x%llx\n",
		   entry->session_id,
		   entry->rx_flow_id,
		   entry->rx_counter_hndl);

	parms.dir = BNXT_ULP_GEN_TX;
	parms.flow_id = &entry->tx_flow_id;
	parms.counter_hndl = &entry->tx_counter_hndl;

	v6_spec.sip6 = NULL;
	v6_mask.sip6 = NULL;
	v6_spec.dip6 = entry->dst_ip.s6_addr;
	v6_mask.dip6 = bnxt_ulp_gen_l3_ipv6_addr_em_mask;
	bth_spec.op_code = NULL;
	bth_mask.op_code = NULL;

	actions.enables = BNXT_ULP_GEN_ACTION_ENABLES_REDIRECT |
		BNXT_ULP_GEN_ACTION_ENABLES_COUNT |
		BNXT_ULP_GEN_ACTION_ENABLES_SET_SMAC |
		BNXT_ULP_GEN_ACTION_ENABLES_SET_DMAC;

	actions.dst_fid = bp->pf.fw_fid;
	memcpy(actions.dmac, entry->dst_mac_mod, ETH_ALEN);
	memcpy(actions.smac, entry->src_mac_mod, ETH_ALEN);

	rc = bnxt_ulp_gen_flow_create(bp, bp->pf.fw_fid, &parms);

	netdev_dbg(bp->dev, "ADD: Session ID = %d Egress Flow ID = %d, Counter = 0x%llx\n",
		   entry->session_id,
		   entry->tx_flow_id,
		   entry->tx_counter_hndl);
	return rc;
}

/* Insert a new session entry into the database */
static int bnxt_udcc_create_session(struct bnxt *bp, u32 session_id)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;
	struct bnxt_udcc_session_entry *entry;
	int rc;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->session_id = session_id;

	/* ====================================================================
	 * 1.Issue HWRM_UDCC_SESSION_QCFG to get the session details
	 *
	 * 2.Use the returned DIP to invoke TF API to get flow_ids/counter_hndls
	 *  for Tx/Tx
	 * a) Use the DIP to query the smac/dmac - TF API
	 * b) Add a Tx flow using DIP, action_param - modify dmac/smac,count
	 * c) Add a Rx flow using DIP as SIP, match: CNP, action: count
	 *
	 * 3. Issue HWRM_UDCC_SESSION_CFG to update the FW
	 */
	rc = bnxt_hwrm_udcc_session_qcfg(bp, entry);
	if (rc)
		goto create_sess_exit;

	rc = bnxt_ulp_udcc_v6_subnet_check(bp, bp->pf.fw_fid, &entry->dst_ip,
					   entry->dst_mac_mod,
					   entry->src_mac_mod);
	if (rc)
		goto create_sess_exit;

	rc = bnxt_tf_ulp_flow_create(bp, entry);
	if (rc)
		goto create_sess_exit;

	rc = bnxt_hwrm_udcc_session_cfg(bp, entry);
	if (rc)
		goto create_sess_exit;

	spin_lock(&udcc->session_db_lock);
	udcc->session_db[session_id] = entry;
	spin_unlock(&udcc->session_db_lock);

	bnxt_debugfs_create_udcc_session(bp, session_id);

	return 0;
create_sess_exit:
	kfree(entry);
	return rc;
}

/* Lookup a session entry by DIP */
struct bnxt_udcc_session_entry *bnxt_udcc_session_db_lookup(struct bnxt *bp, u32 *dip)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;
	struct bnxt_udcc_session_entry *entry;
	int i;

	spin_lock(&udcc->session_db_lock);
	for (i = 0; i < BNXT_UDCC_MAX_SESSIONS; i++) {
		entry = udcc->session_db[i];

		if (entry && !memcmp(entry->dst_ip.s6_addr, dip, 16)) {
			spin_unlock(&udcc->session_db_lock);
			return entry;
		}
	}
	spin_unlock(&udcc->session_db_lock);

	return NULL;
}

static int bnxt_udcc_delete_session(struct bnxt *bp, u32 session_id)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;
	struct bnxt_udcc_session_entry *entry;
	int rc;

	spin_lock(&udcc->session_db_lock);
	if (!udcc->session_db[session_id]) {
		spin_unlock(&udcc->session_db_lock);
		return -ENOENT;
	}

	entry = udcc->session_db[session_id];
	/*
	 * Delete the TF flows for Rx/Tx
	 */
	rc = bnxt_ulp_gen_flow_destroy(bp, bp->pf.fw_fid,
				       entry->rx_flow_id);
	if (rc)
		netdev_dbg(bp->dev, "DEL: Ingress Flow ID = %d Failed rc %d\n",
			   entry->rx_flow_id, rc);

	netdev_dbg(bp->dev, "DEL: Ingress Session ID = %d Flow ID = %d\n",
		   entry->session_id, entry->rx_flow_id);

	rc = bnxt_ulp_gen_flow_destroy(bp, bp->pf.fw_fid,
				       entry->tx_flow_id);
	if (rc)
		netdev_dbg(bp->dev, "DEL: Egress Flow ID = %d Failed rc %d\n",
			   entry->tx_flow_id, rc);
	netdev_dbg(bp->dev, "DEL: Egress Session ID = %d Flow ID = %d\n",
		   entry->session_id, entry->tx_flow_id);

	bnxt_debugfs_delete_udcc_session(bp, session_id);

	kfree(entry);
	udcc->session_db[session_id] = NULL;
	spin_unlock(&udcc->session_db_lock);

	return rc;
}

void bnxt_udcc_session_db_cleanup(struct bnxt *bp)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;
	int i;

	if (!udcc)
		return;

	for (i = 0; i < BNXT_UDCC_MAX_SESSIONS; i++)
		bnxt_udcc_delete_session(bp, i);
}

void bnxt_udcc_task(struct work_struct *work)
{
	struct bnxt_udcc_work *udcc_work =
			container_of(work, struct bnxt_udcc_work, work);
	struct bnxt *bp;

	bp = udcc_work->bp;

	switch (udcc_work->session_opcode) {
	case BNXT_UDCC_SESSION_CREATE:
		bnxt_udcc_create_session(bp, udcc_work->session_id);
		break;

	case BNXT_UDCC_SESSION_DELETE:
		bnxt_udcc_delete_session(bp, udcc_work->session_id);
		break;
	default:
		netdev_warn(bp->dev, "Invalid UDCC session opcode session_id: %d\n",
			    udcc_work->session_id);
	}
}

void bnxt_free_udcc_info(struct bnxt *bp)
{
	struct bnxt_udcc_info *udcc = bp->udcc_info;

	if (!udcc)
		return;

	bnxt_udcc_session_db_cleanup(bp);

	kfree(udcc);
	bp->udcc_info = NULL;

	netdev_info(bp->dev, "%s(): udcc_info freed up!\n", __func__);
}

#else /* if defined(CONFIG_BNXT_FLOWER_OFFLOAD) */

void bnxt_free_udcc_info(struct bnxt *bp)
{
}

int bnxt_alloc_udcc_info(struct bnxt *bp)
{
	return 0;
}

void bnxt_udcc_task(struct work_struct *work)
{
}

void bnxt_udcc_session_db_cleanup(struct bnxt *bp)
{
}

#endif /* if defined(CONFIG_BNXT_FLOWER_OFFLOAD) */
