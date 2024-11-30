// SPDX-License-Identifier: BSD-3-Clause
/* Copyright(c) 2019-2023 Broadcom
 * All rights reserved.
 */

#include <linux/types.h>
#include <linux/vmalloc.h>
#include "ulp_linux.h"
#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_vfr.h"
#include "bnxt_tf_ulp.h"
#include "bnxt_tf_ulp_tfc.h"
#include "bnxt_ulp_flow.h"
#include "bnxt_tf_common.h"
#include "tf_core.h"
#include "tfc.h"
#include "tf_ext_flow_handle.h"

#include "ulp_template_db_enum.h"
#include "ulp_template_struct.h"
#include "ulp_mark_mgr.h"
#include "ulp_fc_mgr.h"
#include "ulp_flow_db.h"
#include "ulp_mapper.h"
#include "ulp_matcher.h"
#include "ulp_port_db.h"

#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
bool
bnxt_ulp_cntxt_shared_tbl_scope_enabled(struct bnxt_ulp_context *ulp_ctx)
{
	u32 flags = 0;
	int rc;

	rc = bnxt_ulp_cntxt_ptr2_ulp_flags_get(ulp_ctx, &flags);
	if (rc)
		return false;
	return !!(flags & BNXT_ULP_SHARED_TBL_SCOPE_ENABLED);
}

int
bnxt_ulp_cntxt_tfcp_set(struct bnxt_ulp_context *ulp, struct tfc *tfcp)
{
	enum bnxt_ulp_tfo_type tfo_type = BNXT_ULP_TFO_TYPE_TFC;

	if (!ulp)
		return -EINVAL;

	/* If NULL, this is invalidating an entry */
	if (!tfcp)
		tfo_type = BNXT_ULP_TFO_TYPE_INVALID;
	ulp->tfo_type = tfo_type;
	ulp->tfcp = tfcp;

	return 0;
}

struct tfc *
bnxt_ulp_cntxt_tfcp_get(struct bnxt_ulp_context *ulp)
{
	return (struct tfc *)ulp->tfcp;
}

u32
bnxt_ulp_cntxt_tbl_scope_max_pools_get(struct bnxt_ulp_context *ulp_ctx)
{
	/* Max pools can be 1 or greater, always return workable value */
	if (ulp_ctx &&
	    ulp_ctx->cfg_data &&
	    ulp_ctx->cfg_data->max_pools)
		return ulp_ctx->cfg_data->max_pools;
	return 1;
}

int
bnxt_ulp_cntxt_tbl_scope_max_pools_set(struct bnxt_ulp_context *ulp_ctx,
				       u32 max)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	/* make sure that max is at least 1 */
	if (!max)
		max = 1;

	ulp_ctx->cfg_data->max_pools = max;
	return 0;
}

enum tfc_tbl_scope_bucket_factor
bnxt_ulp_cntxt_em_mulitplier_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return TFC_TBL_SCOPE_BUCKET_FACTOR_1;

	return ulp_ctx->cfg_data->em_multiplier;
}

int
bnxt_ulp_cntxt_em_mulitplier_set(struct bnxt_ulp_context *ulp_ctx,
				 enum tfc_tbl_scope_bucket_factor factor)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;
	ulp_ctx->cfg_data->em_multiplier = factor;
	return 0;
}

u32
bnxt_ulp_cntxt_num_rx_flows_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->num_rx_flows;
}

int
bnxt_ulp_cntxt_num_rx_flows_set(struct bnxt_ulp_context *ulp_ctx, u32 num)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;
	ulp_ctx->cfg_data->num_rx_flows = num;
	return 0;
}

u32
bnxt_ulp_cntxt_num_tx_flows_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->num_tx_flows;
}

int
bnxt_ulp_cntxt_num_tx_flows_set(struct bnxt_ulp_context *ulp_ctx, u32 num)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;
	ulp_ctx->cfg_data->num_tx_flows = num;
	return 0;
}

u16
bnxt_ulp_cntxt_em_rx_key_max_sz_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->em_rx_key_max_sz;
}

int
bnxt_ulp_cntxt_em_rx_key_max_sz_set(struct bnxt_ulp_context *ulp_ctx,
				    u16 max)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->em_rx_key_max_sz = max;
	return 0;
}

u16
bnxt_ulp_cntxt_em_tx_key_max_sz_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->em_tx_key_max_sz;
}

int
bnxt_ulp_cntxt_em_tx_key_max_sz_set(struct bnxt_ulp_context *ulp_ctx,
				    u16 max)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->em_tx_key_max_sz = max;
	return 0;
}

u16
bnxt_ulp_cntxt_act_rec_rx_max_sz_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->act_rx_max_sz;
}

int
bnxt_ulp_cntxt_act_rec_rx_max_sz_set(struct bnxt_ulp_context *ulp_ctx,
				     int16_t max)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->act_rx_max_sz = max;
	return 0;
}

u16
bnxt_ulp_cntxt_act_rec_tx_max_sz_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return 0;
	return ulp_ctx->cfg_data->act_tx_max_sz;
}

int
bnxt_ulp_cntxt_act_rec_tx_max_sz_set(struct bnxt_ulp_context *ulp_ctx,
				     int16_t max)
{
	if (!ulp_ctx || !ulp_ctx->cfg_data)
		return -EINVAL;

	ulp_ctx->cfg_data->act_tx_max_sz = max;
	return 0;
}

u32
bnxt_ulp_cntxt_page_sz_get(struct bnxt_ulp_context *ulp_ctx)
{
	if (!ulp_ctx)
		return 0;

	return ulp_ctx->cfg_data->page_sz;
}

int
bnxt_ulp_cntxt_page_sz_set(struct bnxt_ulp_context *ulp_ctx,
			   u32 page_sz)
{
	if (!ulp_ctx)
		return -EINVAL;
	ulp_ctx->cfg_data->page_sz = page_sz;
	return 0;
}

static int
ulp_tfc_dparms_init(struct bnxt *bp,
		    struct bnxt_ulp_context *ulp_ctx,
		    u32 dev_id)
{
	u32 num_flows = 0, num_rx_flows = 0, num_tx_flows = 0;
	struct bnxt_ulp_device_params *dparms;

	/* The max_num_kflows were set, so move to external */
	if (bnxt_ulp_cntxt_mem_type_set(ulp_ctx, BNXT_ULP_FLOW_MEM_TYPE_EXT))
		return -EINVAL;

	dparms = bnxt_ulp_device_params_get(dev_id);
	if (!dparms) {
		netdev_dbg(bp->dev, "Failed to get device parms\n");
		return -EINVAL;
	}

	if (bp->max_num_kflows) {
		num_flows = bp->max_num_kflows * 1024;
		dparms->ext_flow_db_num_entries = bp->max_num_kflows * 1024;
	} else {
		num_rx_flows = bnxt_ulp_cntxt_num_rx_flows_get(ulp_ctx);
		num_tx_flows = bnxt_ulp_cntxt_num_tx_flows_get(ulp_ctx);
		num_flows = num_rx_flows + num_tx_flows;
	}

	dparms->ext_flow_db_num_entries = num_flows;

	/* GFID =  2 * num_flows */
	dparms->mark_db_gfid_entries = dparms->ext_flow_db_num_entries * 2;
	netdev_dbg(bp->dev, "Set the number of flows = %llu\n",
		   dparms->ext_flow_db_num_entries);

	return 0;
}

static void
ulp_tfc_tbl_scope_deinit(struct bnxt *bp)
{
	u16 fid = 0, fid_cnt = 0;
	struct tfc *tfcp;
	u8 tsid = 0;
	int rc;

	tfcp = bnxt_ulp_cntxt_tfcp_get(bp->ulp_ctx);
	if (!tfcp)
		return;

	rc = bnxt_ulp_cntxt_tsid_get(bp->ulp_ctx, &tsid);
	if (rc)
		return;

	rc = bnxt_ulp_cntxt_fid_get(bp->ulp_ctx, &fid);
	if (rc)
		return;

	rc = tfc_tbl_scope_cpm_free(tfcp, tsid);
	if (rc)
		netdev_dbg(bp->dev, "Failed Freeing CPM TSID:%d FID:%d\n",
			   tsid, fid);
	else
		netdev_dbg(bp->dev, "Freed CPM TSID:%d FID:%d\n", tsid, fid);

	rc = tfc_tbl_scope_mem_free(tfcp, fid, tsid);
	if (rc)
		netdev_dbg(bp->dev, "Failed freeing tscope mem TSID:%d FID:%d\n",
			   tsid, fid);
	else
		netdev_dbg(bp->dev, "Freed tscope mem TSID:%d FID:%d\n",
			   tsid, fid);

	rc = tfc_tbl_scope_fid_rem(tfcp, fid, tsid, &fid_cnt);
	if (rc)
		netdev_dbg(bp->dev, "Failed removing FID from TSID:%d FID:%d\n",
			   tsid, fid);
	else
		netdev_dbg(bp->dev, "Removed FID from TSID:%d FID:%d\n",
			   tsid, fid);
}

static int
ulp_tfc_tbl_scope_init(struct bnxt *bp)
{
	struct tfc_tbl_scope_mem_alloc_parms mem_parms;
	struct tfc_tbl_scope_size_query_parms qparms =  { 0 };
	u8 max_lkup_sz[CFA_DIR_MAX], max_act_sz[CFA_DIR_MAX];
	struct tfc_tbl_scope_cpm_alloc_parms cparms;
	u16 fid, max_pools;
	bool first = true, shared = false;
	u8 tsid = 0;
	struct tfc *tfcp;
	int rc = 0;

	tfcp = bnxt_ulp_cntxt_tfcp_get(bp->ulp_ctx);
	if (!tfcp)
		return -EINVAL;

	fid = bp->pf.fw_fid;

	max_pools = bnxt_ulp_cntxt_tbl_scope_max_pools_get(bp->ulp_ctx);
	max_lkup_sz[CFA_DIR_RX] =
		bnxt_ulp_cntxt_em_rx_key_max_sz_get(bp->ulp_ctx);
	max_lkup_sz[CFA_DIR_TX] =
		bnxt_ulp_cntxt_em_tx_key_max_sz_get(bp->ulp_ctx);
	max_act_sz[CFA_DIR_RX] =
		bnxt_ulp_cntxt_act_rec_rx_max_sz_get(bp->ulp_ctx);
	max_act_sz[CFA_DIR_TX] =
		bnxt_ulp_cntxt_act_rec_tx_max_sz_get(bp->ulp_ctx);

	shared = bnxt_ulp_cntxt_shared_tbl_scope_enabled(bp->ulp_ctx);
	/* Calculate the sizes for setting up memory */
	qparms.shared = shared;
	qparms.max_pools = max_pools;
	qparms.factor = bnxt_ulp_cntxt_em_mulitplier_get(bp->ulp_ctx);
	qparms.flow_cnt[CFA_DIR_RX] =
		bnxt_ulp_cntxt_num_rx_flows_get(bp->ulp_ctx);
	qparms.flow_cnt[CFA_DIR_TX] =
		bnxt_ulp_cntxt_num_tx_flows_get(bp->ulp_ctx);
	qparms.key_sz_in_bytes[CFA_DIR_RX] = max_lkup_sz[CFA_DIR_RX];
	qparms.key_sz_in_bytes[CFA_DIR_TX] = max_lkup_sz[CFA_DIR_TX];
	qparms.act_rec_sz_in_bytes[CFA_DIR_RX] = max_act_sz[CFA_DIR_RX];
	qparms.act_rec_sz_in_bytes[CFA_DIR_TX] = max_act_sz[CFA_DIR_TX];
	rc = tfc_tbl_scope_size_query(tfcp, &qparms);
	if (rc)
		return rc;

	rc = tfc_tbl_scope_id_alloc(tfcp, shared, CFA_APP_TYPE_TF, &tsid, &first);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to allocate tscope\n");
		return rc;
	}
	netdev_dbg(bp->dev, "Allocated tscope TSID:%d\n", tsid);

	rc = bnxt_ulp_cntxt_tsid_set(bp->ulp_ctx, tsid);
	if (rc)
		return rc;

	/* If we are shared and not the first table scope creator
	 */
	if (shared && !first) {
		bool configured;
		#define ULP_SHARED_TSID_WAIT_TIMEOUT 5000
		#define ULP_SHARED_TSID_WAIT_TIME 50
		int32_t timeout = ULP_SHARED_TSID_WAIT_TIMEOUT;

		do {
			udelay(ULP_SHARED_TSID_WAIT_TIMEOUT);
			rc = tfc_tbl_scope_config_state_get(tfcp, tsid, &configured);
			if (rc) {
				netdev_dbg(bp->dev,
					   "Failed get tsid(%d) config state\n",
					   rc);
				return rc;
			}
			timeout -= ULP_SHARED_TSID_WAIT_TIME;
			netdev_dbg(bp->dev,
				   "Waiting %d ms for shared tsid(%d)\n",
				   timeout, tsid);
		} while (!configured && timeout > 0);
		if (timeout <= 0) {
			netdev_dbg(bp->dev, "Timed out on shared tsid(%d)\n",
				   tsid);
			return -ETIMEDOUT;
		}
	}
	mem_parms.first = first;
	mem_parms.static_bucket_cnt_exp[CFA_DIR_RX] =
		qparms.static_bucket_cnt_exp[CFA_DIR_RX];
	mem_parms.static_bucket_cnt_exp[CFA_DIR_TX] =
		qparms.static_bucket_cnt_exp[CFA_DIR_TX];
	mem_parms.lkup_rec_cnt[CFA_DIR_RX] = qparms.lkup_rec_cnt[CFA_DIR_RX];
	mem_parms.lkup_rec_cnt[CFA_DIR_TX] = qparms.lkup_rec_cnt[CFA_DIR_TX];
	mem_parms.act_rec_cnt[CFA_DIR_RX] = qparms.act_rec_cnt[CFA_DIR_RX];
	mem_parms.act_rec_cnt[CFA_DIR_TX] = qparms.act_rec_cnt[CFA_DIR_TX];
	mem_parms.pbl_page_sz_in_bytes =
		bnxt_ulp_cntxt_page_sz_get(bp->ulp_ctx);
	mem_parms.max_pools = max_pools;

	mem_parms.lkup_pool_sz_exp[CFA_DIR_RX] =
		qparms.lkup_pool_sz_exp[CFA_DIR_RX];
	mem_parms.lkup_pool_sz_exp[CFA_DIR_TX] =
		qparms.lkup_pool_sz_exp[CFA_DIR_TX];

	mem_parms.act_pool_sz_exp[CFA_DIR_RX] =
		qparms.act_pool_sz_exp[CFA_DIR_RX];
	mem_parms.act_pool_sz_exp[CFA_DIR_TX] =
		qparms.act_pool_sz_exp[CFA_DIR_TX];
	mem_parms.local = true;
	rc = tfc_tbl_scope_mem_alloc(tfcp, fid, tsid, &mem_parms);
	if (rc) {
		netdev_dbg(bp->dev,
			   "Failed to allocate tscope mem TSID:%d on FID:%d\n", tsid, fid);
		return rc;
	}
	netdev_dbg(bp->dev, "Allocated tscope mem TSID:%d on FID:%d\n", tsid, fid);

	/* The max contiguous is in 32 Bytes records, so convert Bytes to 32
	 * Byte records.
	 */
	cparms.lkup_max_contig_rec[CFA_DIR_RX] = (max_lkup_sz[CFA_DIR_RX] + 31) / 32;
	cparms.lkup_max_contig_rec[CFA_DIR_TX] = (max_lkup_sz[CFA_DIR_TX] + 31) / 32;
	cparms.act_max_contig_rec[CFA_DIR_RX] = (max_act_sz[CFA_DIR_RX] + 31) / 32;
	cparms.act_max_contig_rec[CFA_DIR_TX] = (max_act_sz[CFA_DIR_TX] + 31) / 32;
	cparms.max_pools = max_pools;

	rc = tfc_tbl_scope_cpm_alloc(tfcp, tsid, &cparms);
	if (rc)
		netdev_dbg(bp->dev, "Failed to allocate CPM TSID:%d FID:%d\n", tsid, fid);
	else
		netdev_dbg(bp->dev, "Allocated CPM TSID:%d FID:%d\n", tsid, fid);

	return rc;
}

static int
ulp_tfc_cntxt_app_caps_init(struct bnxt *bp, u8 app_id, u32 dev_id)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct bnxt_ulp_app_capabilities_info *info;
	bool found = false;
	u32 num = 0, rc;
	u16 i;

	if (ULP_APP_DEV_UNSUPPORTED_ENABLED(ulp_ctx->cfg_data->ulp_flags)) {
		netdev_dbg(bp->dev, "APP ID %d, Device ID: 0x%x not supported.\n",
			   app_id, dev_id);
		return -EINVAL;
	}

	info = bnxt_ulp_app_cap_list_get(&num);
	if (!info || !num) {
		netdev_dbg(bp->dev, "Failed to get app capabilities.\n");
		return -EINVAL;
	}

	for (i = 0; i < num && !found; i++) {
		if (info[i].app_id != app_id || info[i].device_id != dev_id)
			continue;
		found = true;
		if (info[i].flags & BNXT_ULP_APP_CAP_SHARED_EN)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_SHARED_SESSION_ENABLED;
		if (info[i].flags & BNXT_ULP_APP_CAP_HOT_UPGRADE_EN)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_HIGH_AVAIL_ENABLED;
		if (info[i].flags & BNXT_ULP_APP_CAP_UNICAST_ONLY)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_APP_UNICAST_ONLY;
		if (info[i].flags & BNXT_ULP_APP_CAP_IP_TOS_PROTO_SUPPORT)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_APP_TOS_PROTO_SUPPORT;
		if (info[i].flags & BNXT_ULP_APP_CAP_BC_MC_SUPPORT)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_APP_BC_MC_SUPPORT;
		if (info[i].flags & BNXT_ULP_APP_CAP_SOCKET_DIRECT) {
			/* Enable socket direction only if MR is enabled in fw*/
			if (BNXT_MR(bp)) {
				ulp_ctx->cfg_data->ulp_flags |=
					BNXT_ULP_APP_SOCKET_DIRECT;
				netdev_dbg(bp->dev,
					   "Socket Direct feature is enabled\n");
			}
		}
		if (info[i].flags & BNXT_ULP_APP_CAP_DSCP_REMAP)
			ulp_ctx->cfg_data->ulp_flags |=
				BNXT_ULP_APP_DSCP_REMAP_ENABLED;

		rc = bnxt_ulp_cntxt_tbl_scope_max_pools_set(ulp_ctx,
							    info[i].max_pools);
		if (rc)
			return rc;
		rc = bnxt_ulp_cntxt_em_mulitplier_set(ulp_ctx,
						      info[i].em_multiplier);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_num_rx_flows_set(ulp_ctx,
						     info[i].num_rx_flows);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_num_tx_flows_set(ulp_ctx,
						     info[i].num_tx_flows);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_em_rx_key_max_sz_set(ulp_ctx,
							 info[i].em_rx_key_max_sz);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_em_tx_key_max_sz_set(ulp_ctx,
							 info[i].em_tx_key_max_sz);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_act_rec_rx_max_sz_set(ulp_ctx,
							  info[i].act_rx_max_sz);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_act_rec_tx_max_sz_set(ulp_ctx,
							  info[i].act_tx_max_sz);
		if (rc)
			return rc;

		rc = bnxt_ulp_cntxt_page_sz_set(ulp_ctx,
						info[i].pbl_page_sz_in_bytes);
		if (rc)
			return rc;
		bnxt_ulp_num_key_recipes_set(ulp_ctx,
					     info[i].num_key_recipes_per_dir);
	}
	if (!found) {
		netdev_dbg(bp->dev, "APP ID %d, Device ID: 0x%x not supported.\n",
			   app_id, dev_id);
		ulp_ctx->cfg_data->ulp_flags |= BNXT_ULP_APP_DEV_UNSUPPORTED;
		return -EINVAL;
	}

	return 0;
}

/* The function to free and deinit the ulp context data. */
static int
ulp_tfc_ctx_deinit(struct bnxt *bp,
		   struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	/* Free the contents */
	if (session->cfg_data) {
		vfree(session->cfg_data);
		ulp_ctx->cfg_data = NULL;
		session->cfg_data = NULL;
	}
	return 0;
}

/* The function to allocate and initialize the ulp context data. */
static int
ulp_tfc_ctx_init(struct bnxt *bp,
		 struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context	*ulp_ctx = bp->ulp_ctx;
	struct bnxt_ulp_data *ulp_data;
	enum bnxt_ulp_device_id devid;
	int rc = 0;

	/* Initialize the context entries list */
	bnxt_ulp_cntxt_list_init();

	/* Add the context to the context entries list */
	rc = bnxt_ulp_cntxt_list_add(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add the context list entry\n");
		return -ENOMEM;
	}

	/* Allocate memory to hold ulp context data. */
	ulp_data = vzalloc(sizeof(*ulp_data));
	if (!ulp_data)
		return -ENOMEM;

	/* Increment the ulp context data reference count usage. */
	ulp_ctx->cfg_data = ulp_data;
	session->cfg_data = ulp_data;
	ulp_data->ref_cnt++;
	ulp_data->ulp_flags |= BNXT_ULP_VF_REP_ENABLED;

	rc = bnxt_ulp_devid_get(bp, &devid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to determine device for ULP init.\n");
		goto error_deinit;
	}

	rc = bnxt_ulp_cntxt_dev_id_set(ulp_ctx, devid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to set device for ULP init.\n");
		goto error_deinit;
	}

	rc = bnxt_ulp_cntxt_app_id_set(ulp_ctx, bp->app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to set app_id for ULP init.\n");
		goto error_deinit;
	}
	netdev_dbg(bp->dev, "Ulp initialized with app id %d\n", bp->app_id);

	rc = ulp_tfc_dparms_init(bp, ulp_ctx, devid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to init dparms for app(%x)/dev(%x)\n",
			   bp->app_id, devid);
		goto error_deinit;
	}

	rc = ulp_tfc_cntxt_app_caps_init(bp, bp->app_id, devid);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to set caps for app(%x)/dev(%x)\n",
			   bp->app_id, devid);
		goto error_deinit;
	}

	return rc;

error_deinit:
	session->session_opened[BNXT_ULP_SESSION_TYPE_DEFAULT] = 1;
	(void)ulp_tfc_ctx_deinit(bp, session);
	return rc;
}

static int
ulp_tfc_ctx_attach(struct bnxt *bp,
		   struct bnxt_ulp_session_state *session)
{
	u32 flags, dev_id = BNXT_ULP_DEVICE_ID_LAST;
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct tfc *tfcp = bp->tfp;
	u16 fid_cnt = 0;
	int rc = 0;
	u8 app_id;

	rc = bnxt_ulp_cntxt_tfcp_set(bp->ulp_ctx, tfcp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add tfcp to ulp ctxt\n");
		return rc;
	}

	/* Increment the ulp context data reference count usage. */
	ulp_ctx->cfg_data = session->cfg_data;
	ulp_ctx->cfg_data->ref_cnt++;

	rc = tfc_session_fid_add(tfcp, bp->pf.fw_fid,
				 session->session_id, &fid_cnt);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add FID:%d to SID:%d.\n",
			   bp->pf.fw_fid, session->session_id);
		return rc;
	}
	netdev_dbg(bp->dev, "SID:%d added FID:%d\n", session->session_id, bp->pf.fw_fid);

	rc = bnxt_ulp_cntxt_sid_set(bp->ulp_ctx, session->session_id);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add fid to session.\n");
		return rc;
	}

	/* Add the context to the context entries list */
	rc = bnxt_ulp_cntxt_list_add(bp->ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add the context list entry\n");
		return -EINVAL;
	}

	/*
	 * The supported flag will be set during the init. Use it now to
	 * know if we should go through the attach.
	 */
	rc = bnxt_ulp_cntxt_app_id_get(bp->ulp_ctx, &app_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get the app id from ulp.\n");
		return -EINVAL;
	}

	rc = bnxt_ulp_cntxt_dev_id_get(bp->ulp_ctx, &dev_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable do get the dev_id.\n");
		return -EINVAL;
	}

	flags = ulp_ctx->cfg_data->ulp_flags;
	if (ULP_APP_DEV_UNSUPPORTED_ENABLED(flags)) {
		netdev_dbg(bp->dev, "APP ID %d, Device ID: 0x%x not supported.\n",
			   app_id, dev_id);
		return -EINVAL;
	}

	rc = ulp_tfc_tbl_scope_init(bp);

	return rc;
}

static void
ulp_tfc_ctx_detach(struct bnxt *bp,
		   struct bnxt_ulp_session_state *session)
{
	struct tfc *tfcp = bp->tfp;
	u16 fid_cnt = 0;
	int rc;

	ulp_tfc_tbl_scope_deinit(bp);

	rc = tfc_session_fid_rem(tfcp, bp->pf.fw_fid, &fid_cnt);
	if (rc)
		netdev_dbg(bp->dev, "Failed to remove FID:%d from SID:%d\n",
			   bp->pf.fw_fid, session->session_id);
	else
		netdev_dbg(bp->dev, "SID:%d removed FID:%d CNT:%d\n",
			   session->session_id, bp->pf.fw_fid, fid_cnt);
	bnxt_ulp_cntxt_sid_reset(bp->ulp_ctx);
}

/*
 * When a port is deinit'ed by dpdk. This function is called
 * and this function clears the ULP context and rest of the
 * infrastructure associated with it.
 */
static void
ulp_tfc_deinit(struct bnxt *bp,
	       struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	struct tfc *tfcp = bp->tfp;
	u16 fid_cnt = 0;
	int rc;

	if (!ulp_ctx ||
	    !ulp_ctx->cfg_data ||
	    !tfcp)
		return;

	/* cleanup the flow database */
	ulp_flow_db_deinit(ulp_ctx);

	/* Delete the Mark database */
	ulp_mark_db_deinit(ulp_ctx);

	/* cleanup the ulp mapper */
	ulp_mapper_deinit(ulp_ctx);

	/* cleanup the ulp matcher */
	ulp_matcher_deinit(ulp_ctx);

	/* Delete the Flow Counter Manager */
	ulp_fc_mgr_deinit(ulp_ctx);

	/* Delete the Port database */
	ulp_port_db_deinit(ulp_ctx);

	/* free the flow db lock */
	mutex_destroy(&ulp_ctx->cfg_data->flow_db_lock);

	ulp_tfc_tbl_scope_deinit(bp);

	rc = tfc_session_fid_rem(tfcp, bp->pf.fw_fid, &fid_cnt);
	if (rc)
		netdev_dbg(bp->dev, "Failed to remove FID:%d from SID:%d\n",
			   bp->pf.fw_fid, session->session_id);
	else
		netdev_dbg(bp->dev, "SID:%d removed FID:%d CNT:%d\n",
			   session->session_id, bp->pf.fw_fid, fid_cnt);
	bnxt_ulp_cntxt_sid_reset(ulp_ctx);

	/* Delete the ulp context and tf session and free the ulp context */
	ulp_tfc_ctx_deinit(bp, session);

	netdev_dbg(bp->dev, "ulp ctx has been deinitialized\n");
}

/*
 * When a port is initialized by dpdk. This functions is called
 * and this function initializes the ULP context and rest of the
 * infrastructure associated with it.
 */
static int
ulp_tfc_init(struct bnxt *bp,
	     struct bnxt_ulp_session_state *session)
{
	struct bnxt_ulp_context *ulp_ctx = bp->ulp_ctx;
	u32 ulp_dev_id = BNXT_ULP_DEVICE_ID_LAST;
	struct tfc *tfcp = bp->tfp;
	u16 sid;
	int rc;

	if (!bp->tfp)
		return -ENODEV;

	rc = bnxt_ulp_cntxt_tfcp_set(ulp_ctx, bp->tfp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to add tfcp to ulp cntxt\n");
		return -EINVAL;
	}

	/* First time, so allocate a session and save it. */
	rc = tfc_session_id_alloc(tfcp, bp->pf.fw_fid, &sid);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to allocate a session id\n");
		return -EINVAL;
	}
	netdev_dbg(bp->dev, "SID:%d allocated with FID:%d\n", sid, bp->pf.fw_fid);

	session->session_id = sid;
	rc = bnxt_ulp_cntxt_sid_set(ulp_ctx, sid);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to sid to ulp cntxt\n");
		return -EINVAL;
	}

	/* Allocate and Initialize the ulp context. */
	rc = ulp_tfc_ctx_init(bp, session);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the ulp context\n");
		goto jump_to_error;
	}

	rc = ulp_tfc_tbl_scope_init(bp);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the ulp context\n");
		goto jump_to_error;
	}

	mutex_init(&ulp_ctx->cfg_data->flow_db_lock);

	/* Initialize ulp dparms with values devargs passed */
	rc = bnxt_ulp_cntxt_dev_id_get(ulp_ctx, &ulp_dev_id);
	if (rc) {
		netdev_dbg(bp->dev, "Unable to get device id from ulp.\n");
		return rc;
	}

	rc = ulp_tfc_dparms_init(bp, ulp_ctx, ulp_dev_id);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize the dparms\n");
		goto jump_to_error;
	}

	/* create the port database */
	rc = ulp_port_db_init(ulp_ctx, bp->port_count);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the port database\n");
		goto jump_to_error;
	}

	/* Create the Mark database. */
	rc = ulp_mark_db_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the mark database\n");
		goto jump_to_error;
	}

	/* Create the flow database. */
	rc = ulp_flow_db_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to create the flow database\n");
		goto jump_to_error;
	}

	rc = ulp_matcher_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp matcher\n");
		goto jump_to_error;
	}

	rc = ulp_mapper_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp mapper\n");
		goto jump_to_error;
	}

	rc = ulp_fc_mgr_init(ulp_ctx);
	if (rc) {
		netdev_dbg(bp->dev, "Failed to initialize ulp flow counter mgr\n");
		goto jump_to_error;
	}

	rc = bnxt_flow_meter_init(bp);
	if (rc) {
		if (rc != -EOPNOTSUPP) {
			netdev_err(bp->dev, "Failed to config meter\n");
			goto jump_to_error;
		}
		rc = 0;
	}

	netdev_dbg(bp->dev, "ulp ctx has been initialized\n");
	return rc;

jump_to_error:
	ulp_ctx->ops->ulp_deinit(bp, session);
	return rc;
}

const struct bnxt_ulp_core_ops bnxt_ulp_tfc_core_ops = {
	.ulp_ctx_attach = ulp_tfc_ctx_attach,
	.ulp_ctx_detach = ulp_tfc_ctx_detach,
	.ulp_deinit =  ulp_tfc_deinit,
	.ulp_init =  ulp_tfc_init,
};
#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */
