// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016-2018 Broadcom Limited
 * Copyright (c) 2018-2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <linux/jhash.h>
#ifdef HAVE_TC_SETUP_TYPE
#include <net/pkt_cls.h>
#endif

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "bnxt_sriov.h"
#include "bnxt_vfr.h"
#include "bnxt_devlink.h"
#include "bnxt_tc.h"
#include "bnxt_ulp_flow.h"

/* Synchronize TF ULP port operations.
 * TBD: Revisit this global lock and consider making this a per-adapter lock.
 */
DEFINE_MUTEX(tf_port_lock);

#if defined(CONFIG_VF_REPS) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
/* This function initializes Truflow feature which enables host based
 * flow offloads.
 */
int bnxt_tf_port_init(struct bnxt *bp)
{
	int rc;

	mutex_lock(&tf_port_lock);
	rc = bnxt_ulp_port_init(bp);
	if (rc)
		netdev_err(bp->dev, "Failed to initialize Truflow feature\n");
	mutex_unlock(&tf_port_lock);
	return rc;
}

void bnxt_tf_port_deinit(struct bnxt *bp)
{
	mutex_lock(&tf_port_lock);
	bnxt_ulp_port_deinit(bp);
	mutex_unlock(&tf_port_lock);
}

void bnxt_custom_tf_port_init(struct bnxt *bp)
{
#ifdef CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD
	if (bnxt_tc_is_switchdev_mode(bp))
		return;
	if (BNXT_PF(bp) && BNXT_TRUFLOW_EN(bp))
		bnxt_tf_port_init(bp);
#endif
	return;
}

void bnxt_custom_tf_port_deinit(struct bnxt *bp)
{
#ifdef CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD
	if (bnxt_tc_is_switchdev_mode(bp))
		return;
	if (BNXT_PF(bp) && BNXT_TRUFLOW_EN(bp))
		bnxt_tf_port_deinit(bp);
#endif
	return;
}
#endif

#ifdef CONFIG_VF_REPS

#define CFA_HANDLE_INVALID		0xffff
#define VF_IDX_INVALID			0xffff

static int hwrm_cfa_vfr_alloc(struct bnxt *bp, u16 vf_idx,
			      u32 *tx_cfa_action, u16 *rx_cfa_code)
{
	struct hwrm_cfa_vfr_alloc_output *resp;
	struct hwrm_cfa_vfr_alloc_input *req;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_CFA_VFR_ALLOC);
	if (!rc) {
		req->vf_id = cpu_to_le16(vf_idx);
		sprintf(req->vfr_name, "vfr%d", vf_idx);

		resp = hwrm_req_hold(bp, req);
		rc = hwrm_req_send(bp, req);
		if (!rc) {
			*tx_cfa_action = le16_to_cpu(resp->tx_cfa_action);
			*rx_cfa_code = le16_to_cpu(resp->rx_cfa_code);
			netdev_dbg(bp->dev, "tx_cfa_action=0x%x, rx_cfa_code=0x%x",
				   *tx_cfa_action, *rx_cfa_code);
		}
		hwrm_req_drop(bp, req);
	}
	if (rc)
		netdev_info(bp->dev, "%s error rc=%d\n", __func__, rc);
	return rc;
}

static int hwrm_cfa_vfr_free(struct bnxt *bp, u16 vf_idx)
{
	struct hwrm_cfa_vfr_free_input *req;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_CFA_VFR_FREE);
	if (!rc) {
		sprintf(req->vfr_name, "vfr%d", vf_idx);
		rc = hwrm_req_send(bp, req);
	}
	if (rc)
		netdev_info(bp->dev, "%s error rc=%d\n", __func__, rc);
	return rc;
}

int bnxt_hwrm_vfr_qcfg(struct bnxt *bp, struct bnxt_vf_rep *vf_rep,
		       u16 *max_mtu)
{
	struct hwrm_func_qcfg_output *resp;
	struct hwrm_func_qcfg_input *req;
	struct bnxt_vf_info *vf;
	u16 mtu;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_FUNC_QCFG);
	if (rc)
		return rc;

	rcu_read_lock();
	vf = rcu_dereference(bp->pf.vf);
	if (!vf) {
		rcu_read_unlock();
		return -EINVAL;
	}
	req->fid = vf[vf_rep->vf_idx].fw_fid;
	rcu_read_unlock();
	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (!rc) {
		mtu = le16_to_cpu(resp->max_mtu_configured);
		if (!mtu)
			*max_mtu = BNXT_MAX_MTU;
		else
			*max_mtu = mtu;
	}
	hwrm_req_drop(bp, req);

	return rc;
}

static int bnxt_vf_rep_open(struct net_device *dev)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);
	struct bnxt *bp = vf_rep->bp;

	/* Enable link and TX only if the parent PF is open. */
	if (netif_running(bp->dev)) {
		netif_carrier_on(dev);
		netif_tx_start_all_queues(dev);
	}
	return 0;
}

static int bnxt_vf_rep_close(struct net_device *dev)
{
	netif_carrier_off(dev);
	netif_tx_disable(dev);

	return 0;
}

static netdev_tx_t bnxt_vf_rep_xmit(struct sk_buff *skb,
				    struct net_device *dev)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);
	int rc, len = skb->len;

	skb_dst_drop(skb);
	dst_hold((struct dst_entry *)vf_rep->dst);
	skb_dst_set(skb, (struct dst_entry *)vf_rep->dst);
	skb->dev = vf_rep->dst->u.port_info.lower_dev;

	rc = dev_queue_xmit(skb);
	if (!rc) {
		vf_rep->tx_stats.packets++;
		vf_rep->tx_stats.bytes += len;
	}
	return rc;
}

static void
bnxt_vf_rep_get_stats64(struct net_device *dev,
			struct rtnl_link_stats64 *stats)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);

	if (!vf_rep || !vf_rep->bp)
		return;

	stats->rx_packets = vf_rep->rx_stats.packets;
	stats->rx_bytes = vf_rep->rx_stats.bytes;
	stats->tx_packets = vf_rep->tx_stats.packets;
	stats->tx_bytes = vf_rep->tx_stats.bytes;
}

#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
#ifdef HAVE_TC_SETUP_TYPE
#ifdef HAVE_TC_SETUP_BLOCK
static LIST_HEAD(bnxt_vf_block_cb_list);

static int bnxt_vf_rep_setup_tc_block_cb(enum tc_setup_type type,
					 void *type_data,
					 void *cb_priv)
{
	struct bnxt_vf_rep *vf_rep = cb_priv;
	struct bnxt *bp = vf_rep->bp;
	u16 vf_fid;

	vf_fid = bnxt_vf_target_id(&bp->pf, vf_rep->vf_idx);
	if (vf_fid == INVALID_HW_RING_ID)
		return -EINVAL;

	if (!bnxt_tc_flower_enabled(vf_rep->bp))
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
#ifdef HAVE_TC_CB_EGDEV
		return bnxt_tc_setup_flower(bp, vf_fid, type_data,
					    BNXT_TC_DEV_INGRESS);
#else
		return bnxt_tc_setup_flower(bp, vf_fid, type_data);
#endif

#if defined(HAVE_TC_MATCHALL_FLOW_RULE) && defined(HAVE_FLOW_ACTION_POLICE)
	case TC_SETUP_CLSMATCHALL:
		return bnxt_tc_setup_matchall(bp, vf_fid, type_data);
#endif
	default:
		return -EOPNOTSUPP;
	}
}

#endif /* HAVE_TC_SETUP_BLOCK */

static int bnxt_vf_rep_setup_tc(struct net_device *dev, enum tc_setup_type type,
				void *type_data)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);

	switch (type) {
#ifdef HAVE_TC_SETUP_BLOCK
	case TC_SETUP_BLOCK:
		return flow_block_cb_setup_simple(type_data,
						  &bnxt_vf_block_cb_list,
						  bnxt_vf_rep_setup_tc_block_cb,						  vf_rep, vf_rep, true);
#else
	case TC_SETUP_CLSFLOWER: {
		struct bnxt *bp = vf_rep->bp;
		u16 vf_fid;

		vf_fid = bnxt_vf_target_id(&bp->pf, vf_rep->vf_idx);
		if (vf_fid == INVALID_HW_RING_ID)
			return -EINVAL;
#ifdef HAVE_TC_CB_EGDEV
		return bnxt_tc_setup_flower(bp, vf_fid, type_data,
					    BNXT_TC_DEV_INGRESS);
#else
		return bnxt_tc_setup_flower(bp, vf_fid, type_data);
#endif
	}
#endif
	default:
		return -EOPNOTSUPP;
	}
}

#else /* HAVE_TC_SETUP_TYPE */

#ifdef HAVE_CHAIN_INDEX
static int bnxt_vf_rep_setup_tc(struct net_device *dev, u32 handle,
				u32 chain_index, __be16 proto,
				struct tc_to_netdev *ntc)
#else
static int bnxt_vf_rep_setup_tc(struct net_device *dev, u32 handle,
				__be16 proto, struct tc_to_netdev *ntc)
#endif
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);
	u16 vf_fid;

	vf_fid = bnxt_vf_target_id(&vf_rep->bp->pf, vf_rep->vf_idx);
	if (vf_fid == INVALID_HW_RING_ID)
		return -EINVAL;

	if (!bnxt_tc_flower_enabled(vf_rep->bp))
		return -EOPNOTSUPP;

	switch (ntc->type) {
	case TC_SETUP_CLSFLOWER:
#ifdef HAVE_TC_CB_EGDEV
		return bnxt_tc_setup_flower(vf_rep->bp,
					    vf_fid,
					    ntc->cls_flower,
					    BNXT_TC_DEV_INGRESS);
#else
		return bnxt_tc_setup_flower(vf_rep->bp,
					    vf_fid,
					    ntc->cls_flower);
#endif
	default:
		return -EOPNOTSUPP;
	}
}
#endif /* HAVE_TC_SETUP_TYPE */

#ifdef HAVE_TC_CB_EGDEV
static int bnxt_vf_rep_tc_cb_egdev(enum tc_setup_type type, void *type_data,
				   void *cb_priv)
{
	struct bnxt_vf_rep *vf_rep = cb_priv;
	struct bnxt *bp = vf_rep->bp;
	u16 vf_fid;

	vf_fid = bnxt_vf_target_id(&bp->pf, vf_rep->vf_idx);
	if (vf_fid == INVALID_HW_RING_ID)
		return -EINVAL;

	if (!bnxt_tc_flower_enabled(vf_rep->bp))
		return -EOPNOTSUPP;

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return bnxt_tc_setup_flower(bp, vf_fid, type_data,
					    BNXT_TC_DEV_EGRESS);
	default:
		return -EOPNOTSUPP;
	}
}
#else

#ifdef HAVE_TC_SETUP_TYPE
static int bnxt_vf_rep_tc_cb_egdev(enum tc_setup_type type, void *type_data,
				   void *cb_priv)
#else
static int bnxt_vf_rep_tc_cb_egdev(int type, void *type_data, void *cb_priv)
#endif /* HAVE_TC_SETUP_TYPE */
{
	return 0;
}
#endif /* HAVE_TC_CB_EGDEV */

#define bnxt_cb_egdev		((void *)bnxt_vf_rep_tc_cb_egdev)

#endif /* CONFIG_BNXT_FLOWER_OFFLOAD */

struct net_device *bnxt_get_vf_rep(struct bnxt *bp, u16 cfa_code)
{
	u16 vf_idx;

	if (cfa_code && bp->cfa_code_map && BNXT_PF(bp)) {
		vf_idx = bp->cfa_code_map[cfa_code];
		if (vf_idx != VF_IDX_INVALID)
			return bp->vf_reps[vf_idx]->dev;
	}
	return NULL;
}

struct net_device *bnxt_tf_get_vf_rep(struct bnxt *bp,
				      struct rx_cmp_ext *rxcmp1,
				      struct bnxt_tpa_info *tpa_info)
{
	u32 mark_id = 0;
	u16 vf_idx;
	int rc;

	if (bp->cfa_code_map && BNXT_PF(bp)) {
		rc = bnxt_ulp_get_mark_from_cfacode(bp, rxcmp1, tpa_info,
						    &mark_id);
		if (rc)
			return NULL;
		/* mark_id is endpoint vf's fw fid */
		vf_idx = bp->cfa_code_map[mark_id];
		if (vf_idx != VF_IDX_INVALID)
			return bp->vf_reps[vf_idx]->dev;
	}
	return NULL;
}

void bnxt_vf_rep_rx(struct bnxt *bp, struct sk_buff *skb)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(skb->dev);

	vf_rep->rx_stats.bytes += skb->len;
	vf_rep->rx_stats.packets++;

	netif_receive_skb(skb);
}

static int bnxt_vf_rep_get_phys_port_name(struct net_device *dev, char *buf,
					  size_t len)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);
	int rc;

	if (!vf_rep || !vf_rep->bp || !vf_rep->bp->pdev)
		return -EINVAL;

	rc = snprintf(buf, len, "pf%dvf%d", vf_rep->bp->pf.fw_fid - 1,
		      vf_rep->vf_idx);
	if (rc >= len)
		return -EOPNOTSUPP;

	return 0;
}

static void bnxt_vf_rep_get_drvinfo(struct net_device *dev,
				    struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, DRV_MODULE_NAME, sizeof(info->driver));
	strlcpy(info->version, DRV_MODULE_VERSION, sizeof(info->version));
}

#ifdef HAVE_NDO_GET_PORT_PARENT_ID
static int bnxt_vf_rep_get_port_parent_id(struct net_device *dev,
					  struct netdev_phys_item_id *ppid)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);

	/* as only PORT_PARENT_ID is supported currently use common code
	 * between PF and VF-rep for now.
	 */
	return bnxt_get_port_parent_id(vf_rep->bp->dev, ppid);
}

#else

static int bnxt_vf_rep_port_attr_get(struct net_device *dev,
				     struct switchdev_attr *attr)
{
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);

	/* as only PORT_PARENT_ID is supported currently use common code
	 * between PF and VF-rep for now.
	 */
	return bnxt_port_attr_get(vf_rep->bp, attr);
}

static const struct switchdev_ops bnxt_vf_rep_switchdev_ops = {
	.switchdev_port_attr_get	= bnxt_vf_rep_port_attr_get
};
#endif

static const char *const bnxt_vf_rep_stats_str[] = {
	"vport_rx_packets",
	"vport_rx_bytes",
	"vport_tx_packets",
	"vport_tx_bytes",
	"vport_rx_errors",
	"vport_rx_discards",
	"vport_tx_discards",
	"vport_rx_tpa_pkt",
	"vport_rx_tpa_bytes",
	"vport_rx_tpa_errors"
};

#define	BNXT_VF_REP_NUM_COUNTERS	ARRAY_SIZE(bnxt_vf_rep_stats_str)
static int bnxt_get_vf_rep_sset_count(struct net_device *dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS:
		return BNXT_VF_REP_NUM_COUNTERS;
	default:
		return -EOPNOTSUPP;
	}
}

static void bnxt_get_vf_rep_strings(struct net_device *dev, u32 stringset, u8 *buf)
{
	u32 i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < BNXT_VF_REP_NUM_COUNTERS; i++) {
			sprintf(buf, "%s", bnxt_vf_rep_stats_str[i]);
			buf += ETH_GSTRING_LEN;
		}
		break;
	default:
		netdev_err(dev, "%s invalid request %x\n", __func__, stringset);
		break;
	}
}

static void bnxt_get_vf_rep_ethtool_stats(struct net_device *dev,
					  struct ethtool_stats *stats, u64 *buf)
{
	int buf_size = BNXT_VF_REP_NUM_COUNTERS * sizeof(u64);
	struct bnxt_vf_rep *vf_rep = netdev_priv(dev);
	struct bnxt_vf_info *vf;
	u64 *sw;

	if (!vf_rep || !vf_rep->bp)
		return;

	memset(buf, 0, buf_size);

	rcu_read_lock();
	vf = rcu_dereference(vf_rep->bp->pf.vf);
	if (!vf) {
		rcu_read_unlock();
		return;
	}
	sw = vf[vf_rep->vf_idx].stats.sw_stats;

	buf[0] = bnxt_add_ring_rx_pkts(sw);
	buf[1] = bnxt_add_ring_rx_bytes(sw);
	buf[2] = bnxt_add_ring_tx_pkts(sw);
	buf[3] = bnxt_add_ring_tx_bytes(sw);
	buf[4] = BNXT_GET_RING_STATS64(sw, rx_error_pkts);
	buf[5] = BNXT_GET_RING_STATS64(sw, rx_discard_pkts);
	buf[6] = BNXT_GET_RING_STATS64(sw, tx_error_pkts) +
			BNXT_GET_RING_STATS64(sw, tx_discard_pkts);
	buf[7] = BNXT_GET_RING_STATS64(sw, tpa_pkts);
	buf[8] = BNXT_GET_RING_STATS64(sw, tpa_bytes);
	buf[9] = BNXT_GET_RING_STATS64(sw, tpa_aborts);
	rcu_read_unlock();
}

static const struct ethtool_ops bnxt_vf_rep_ethtool_ops = {
	.get_drvinfo		= bnxt_vf_rep_get_drvinfo,
	.get_ethtool_stats      = bnxt_get_vf_rep_ethtool_stats,
	.get_strings            = bnxt_get_vf_rep_strings,
	.get_sset_count         = bnxt_get_vf_rep_sset_count,
};

static const struct net_device_ops bnxt_vf_rep_netdev_ops = {
#ifdef HAVE_NDO_SETUP_TC_RH
	.ndo_size               = sizeof(const struct net_device_ops),
#endif
	.ndo_open		= bnxt_vf_rep_open,
	.ndo_stop		= bnxt_vf_rep_close,
	.ndo_start_xmit		= bnxt_vf_rep_xmit,
	.ndo_get_stats64	= bnxt_vf_rep_get_stats64,
#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
#ifdef HAVE_NDO_SETUP_TC_RH
	.extended.ndo_setup_tc_rh	= bnxt_vf_rep_setup_tc,
#else
	.ndo_setup_tc		= bnxt_vf_rep_setup_tc,
#endif
#endif
#ifdef HAVE_NDO_GET_PORT_PARENT_ID
	.ndo_get_port_parent_id	= bnxt_vf_rep_get_port_parent_id,
#endif
#ifdef HAVE_EXT_GET_PHYS_PORT_NAME
	.extended.ndo_get_phys_port_name = bnxt_vf_rep_get_phys_port_name
#else
	.ndo_get_phys_port_name = bnxt_vf_rep_get_phys_port_name
#endif
};

bool bnxt_dev_is_vf_rep(struct net_device *dev)
{
	return dev->netdev_ops == &bnxt_vf_rep_netdev_ops;
}

int bnxt_hwrm_cfa_pair_exists(struct bnxt *bp, void *vfr)
{
	struct hwrm_cfa_pair_info_output *resp;
	struct hwrm_cfa_pair_info_input *req;
	struct bnxt_vf_rep *rep_bp = vfr;
	int rc;

	if (!(BNXT_PF(bp) || BNXT_VF_IS_TRUSTED(bp))) {
		netdev_dbg(bp->dev,
			   "Not a PF or trusted VF. Command not supported\n");
		return -EOPNOTSUPP;
	}

	rc = hwrm_req_init(bp, req, HWRM_CFA_PAIR_INFO);
	if (rc)
		return rc;

	rc = snprintf(req->pair_name, sizeof(req->pair_name), "%svfr%d",
		      dev_name(rep_bp->bp->dev->dev.parent), rep_bp->vf_idx);

	if (rc >= sizeof(req->pair_name) || rc < 0)
		return -EINVAL;

	req->flags = cpu_to_le32(CFA_PAIR_INFO_REQ_FLAGS_LOOKUP_TYPE);

	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	/* CFA_PAIR_EXISTS command will succeed even though there is no
	 * CFA_PAIR, the proper check to see if CFA_PAIR exists or not
	 * is to look at the resp->pair_name.
	 */
	if (!rc && !strlen(resp->pair_name))
		rc = -EINVAL;
	hwrm_req_drop(bp, req);

	return rc;
}

int bnxt_hwrm_cfa_pair_free(struct bnxt *bp, void *vfr)
{
	struct hwrm_cfa_pair_free_input *req;
	struct bnxt_vf_rep *rep_bp = vfr;
	int rc;

	if (!(BNXT_PF(bp) || BNXT_VF_IS_TRUSTED(bp))) {
		netdev_dbg(bp->dev,
			   "Not a PF or trusted VF. Command not supported\n");
		return 0;
	}

	rc = hwrm_req_init(bp, req, HWRM_CFA_PAIR_FREE);
	if (rc)
		return rc;

	rc = snprintf(req->pair_name, sizeof(req->pair_name), "%svfr%d",
		      dev_name(rep_bp->bp->dev->dev.parent), rep_bp->vf_idx);

	if (rc >= sizeof(req->pair_name) || rc < 0)
		return -EINVAL;

	req->pair_mode = cpu_to_le16(CFA_PAIR_FREE_REQ_PAIR_MODE_REP2FN_TRUFLOW);
	req->pf_b_id = rep_bp->bp->pf.fw_fid - 1;
	req->vf_id =  cpu_to_le16(rep_bp->vf_idx);

	rc = hwrm_req_send(bp, req);
	if (rc)
		return rc;

	netdev_dbg(bp->dev, "VFR %d freed\n", rep_bp->vf_idx);
	return 0;
}

static void __bnxt_tf_free_one_vf_rep(struct bnxt *bp,
				      struct bnxt_vf_rep *vf_rep)
{
	bnxt_ulp_free_vf_rep(bp, vf_rep);
}

/* Called when the parent PF interface is closed:
 * As the mode transition from SWITCHDEV to LEGACY
 * happens under the rtnl_lock() this routine is safe
 * under the rtnl_lock()
 */
void bnxt_vf_reps_close(struct bnxt *bp)
{
	struct bnxt_vf_rep *vf_rep;
	u16 num_vfs, i;

	if (!bnxt_tc_is_switchdev_mode(bp))
		return;

	num_vfs = pci_num_vf(bp->pdev);
	for (i = 0; i < num_vfs; i++) {
		vf_rep = bp->vf_reps[i];
		if (netif_running(vf_rep->dev))
			bnxt_vf_rep_close(vf_rep->dev);
	}
}

/* Called when the parent PF interface is opened (re-opened):
 * As the mode transition from SWITCHDEV to LEGACY
 * happen under the rtnl_lock() this routine is safe
 * under the rtnl_lock()
 */
void bnxt_vf_reps_open(struct bnxt *bp)
{
	int i;

	if (!bnxt_tc_is_switchdev_mode(bp))
		return;

	for (i = 0; i < pci_num_vf(bp->pdev); i++) {
		/* Open the VF-Rep only if it is allocated in the FW */
		if (bp->vf_reps[i]->tx_cfa_action != CFA_HANDLE_INVALID)
			bnxt_vf_rep_open(bp->vf_reps[i]->dev);
	}
}

static void __bnxt_free_one_vf_rep(struct bnxt *bp, struct bnxt_vf_rep *vf_rep)
{
	if (!vf_rep)
		return;

	if (vf_rep->dst) {
		dst_release((struct dst_entry *)vf_rep->dst);
		vf_rep->dst = NULL;
	}
	if (vf_rep->tx_cfa_action != CFA_HANDLE_INVALID) {
		if (BNXT_TRUFLOW_EN(bp))
			__bnxt_tf_free_one_vf_rep(bp, vf_rep);
		else
			hwrm_cfa_vfr_free(bp, vf_rep->vf_idx);
		vf_rep->tx_cfa_action = CFA_HANDLE_INVALID;
	}
}

static void __bnxt_vf_reps_destroy(struct bnxt *bp)
{
	u16 num_vfs = pci_num_vf(bp->pdev);
	struct bnxt_vf_rep *vf_rep;
	int i;

	for (i = 0; i < num_vfs; i++) {
		vf_rep = bp->vf_reps[i];
		if (vf_rep) {
			__bnxt_free_one_vf_rep(bp, vf_rep);
			if (vf_rep->dev) {
				/* if register_netdev failed, then netdev_ops
				 * would have been set to NULL
				 */
				if (vf_rep->dev->netdev_ops) {
#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
					bnxt_unreg_egdev(vf_rep->dev,
							 bnxt_cb_egdev,
							 (void *)vf_rep);
#endif
					unregister_netdev(vf_rep->dev);
				}
				free_netdev(vf_rep->dev);
			}
			bp->vf_reps[i] = NULL;
		}
	}

	kfree(bp->vf_reps);
	bp->vf_reps = NULL;
}

void bnxt_vf_reps_destroy(struct bnxt *bp)
{
	bool closed = false;

	if (!bnxt_tc_is_switchdev_mode(bp))
		return;

	if (!bp->vf_reps)
		return;

	/* Ensure that parent PF's and VF-reps' RX/TX has been quiesced
	 * before proceeding with VF-rep cleanup.
	 */
	rtnl_lock();
	if (netif_running(bp->dev)) {
		bnxt_close_nic(bp, false, false);
		closed = true;
	}
	/* un-publish cfa_code_map so that RX path can't see it anymore */
	kfree(bp->cfa_code_map);
	bp->cfa_code_map = NULL;
	bp->eswitch_mode = DEVLINK_ESWITCH_MODE_LEGACY;

	if (closed)
		bnxt_open_nic(bp, false, false);
	rtnl_unlock();

	/* Need to call vf_reps_destroy() outside of rntl_lock
	 * as unregister_netdev takes rtnl_lock
	 */
	__bnxt_vf_reps_destroy(bp);

	if (BNXT_TRUFLOW_EN(bp))
		bnxt_tf_port_deinit(bp);
}

/* Free the VF-Reps in firmware, during firmware hot-reset processing.
 * Note that the VF-Rep netdevs are still active (not unregistered) during
 * this process. As the mode transition from SWITCHDEV to LEGACY happens
 * under the rtnl_lock() this routine is safe under the rtnl_lock().
 */
void bnxt_vf_reps_free(struct bnxt *bp)
{
	u16 num_vfs = pci_num_vf(bp->pdev);
	int i;

	if (!bnxt_tc_is_switchdev_mode(bp))
		return;

	for (i = 0; i < num_vfs; i++)
		__bnxt_free_one_vf_rep(bp, bp->vf_reps[i]);
}

int bnxt_hwrm_cfa_pair_alloc(struct bnxt *bp, void *vfr)
{
	struct hwrm_cfa_pair_alloc_input *req;
	struct bnxt_vf_rep *rep_bp = vfr;
	int rc;

	if (!(BNXT_PF(bp) || BNXT_VF_IS_TRUSTED(bp))) {
		netdev_dbg(bp->dev,
			   "Not a PF or trusted VF. Command not supported\n");
		return -EINVAL;
	}

	rc = hwrm_req_init(bp, req, HWRM_CFA_PAIR_ALLOC);
	if (rc)
		return rc;

	req->pair_mode = cpu_to_le16(CFA_PAIR_ALLOC_REQ_PAIR_MODE_REP2FN_TRUFLOW);
	rc = snprintf(req->pair_name, sizeof(req->pair_name), "%svfr%d",
		      dev_name(rep_bp->bp->dev->dev.parent), rep_bp->vf_idx);

	if (rc >= sizeof(req->pair_name) || rc < 0)
		return -EINVAL;

	req->pf_b_id = rep_bp->bp->pf.fw_fid - 1;
	req->vf_b_id =  cpu_to_le16(rep_bp->vf_idx);
	req->vf_a_id = cpu_to_le16(rep_bp->bp->pf.fw_fid);
	req->host_b_id = 1; /* TBD - Confirm if this is OK */

	rc = hwrm_req_send(bp, req);
	if (rc)
		return rc;

	netdev_dbg(bp->dev, "VFR %d allocated\n", rep_bp->vf_idx);
	return rc;
}

static int bnxt_alloc_tf_vf_rep(struct bnxt *bp, struct bnxt_vf_rep *vf_rep,
				u16 *cfa_code_map)
{
	struct bnxt_vf_info *vf;
	int rc;

	rc = bnxt_ulp_alloc_vf_rep(bp, vf_rep);
	if (rc)
		return rc;

	rcu_read_lock();
	vf = rcu_dereference(bp->pf.vf);
	if (vf)
		cfa_code_map[vf[vf_rep->vf_idx].fw_fid] = vf_rep->vf_idx;
	rcu_read_unlock();

	return 0;
}

static int bnxt_alloc_vf_rep(struct bnxt *bp, struct bnxt_vf_rep *vf_rep,
			     u16 *cfa_code_map)
{
	int rc;

	if (!BNXT_TRUFLOW_EN(bp)) {
		/* get cfa handles from FW */
		if (hwrm_cfa_vfr_alloc(bp, vf_rep->vf_idx, &vf_rep->tx_cfa_action,
				       &vf_rep->rx_cfa_code))
			return -ENOLINK;
		cfa_code_map[vf_rep->rx_cfa_code] = vf_rep->vf_idx;
	} else {
		rc = bnxt_alloc_tf_vf_rep(bp, vf_rep, cfa_code_map);
		if (rc)
			return rc;
	}

	vf_rep->dst = metadata_dst_alloc(0, METADATA_HW_PORT_MUX, GFP_KERNEL);
	if (!vf_rep->dst)
		return -ENOMEM;

	/* only cfa_action is needed to mux a packet while TXing */
	vf_rep->dst->u.port_info.port_id = vf_rep->tx_cfa_action;
	vf_rep->dst->u.port_info.lower_dev = bp->dev;

	return 0;
}

/* Allocate the VF-Reps in firmware, during firmware hot-reset processing.
 * Note that the VF-Rep netdevs are still active (not unregistered) during
 * this process. As the mode transition from SWITCHDEV to LEGACY happens
 * under the rtnl_lock() this routine is safe under the rtnl_lock().
 */
int bnxt_vf_reps_alloc(struct bnxt *bp)
{
	u16 *cfa_code_map = bp->cfa_code_map, num_vfs = pci_num_vf(bp->pdev);
	struct bnxt_vf_rep *vf_rep;
	int rc, i;

	if (!bnxt_tc_is_switchdev_mode(bp))
		return -EINVAL;

	if (!cfa_code_map)
		return -EINVAL;

	for (i = 0; i < MAX_CFA_CODE; i++)
		cfa_code_map[i] = VF_IDX_INVALID;

	for (i = 0; i < num_vfs; i++) {
		vf_rep = bp->vf_reps[i];
		vf_rep->vf_idx = i;

		rc = bnxt_alloc_vf_rep(bp, vf_rep, cfa_code_map);
		if (rc)
			goto err;
	}

	return 0;

err:
	netdev_info(bp->dev, "%s error=%d\n", __func__, rc);
	bnxt_vf_reps_free(bp);
	return rc;
}

/* Use the OUI of the PF's perm addr and report the same mac addr
 * for the same VF-rep each time
 */
static void bnxt_vf_rep_eth_addr_gen(u8 *src_mac, u16 vf_idx, u8 *mac)
{
	u32 addr;

	ether_addr_copy(mac, src_mac);

	addr = jhash(src_mac, ETH_ALEN, 0) + vf_idx;
	mac[3] = (u8)(addr & 0xFF);
	mac[4] = (u8)((addr >> 8) & 0xFF);
	mac[5] = (u8)((addr >> 16) & 0xFF);
}

static void bnxt_vf_rep_netdev_init(struct bnxt *bp, struct bnxt_vf_rep *vf_rep,
				    struct net_device *dev)
{
	struct net_device *pf_dev = bp->dev;
	u16 max_mtu;

	dev->netdev_ops = &bnxt_vf_rep_netdev_ops;
	dev->ethtool_ops = &bnxt_vf_rep_ethtool_ops;
#ifndef HAVE_NDO_GET_PORT_PARENT_ID
	SWITCHDEV_SET_OPS(dev, &bnxt_vf_rep_switchdev_ops);
#endif
	/* Just inherit all the featues of the parent PF as the VF-R
	 * uses the RX/TX rings of the parent PF
	 */
	dev->hw_features = pf_dev->hw_features;
	dev->gso_partial_features = pf_dev->gso_partial_features;
	dev->vlan_features = pf_dev->vlan_features;
	dev->hw_enc_features = pf_dev->hw_enc_features;
	dev->features |= pf_dev->features;
	bnxt_vf_rep_eth_addr_gen(bp->pf.mac_addr, vf_rep->vf_idx,
				 dev->perm_addr);
	eth_hw_addr_set(dev, dev->perm_addr);
	/* Set VF-Rep's max-mtu to the corresponding VF's max-mtu */
	if (!bnxt_hwrm_vfr_qcfg(bp, vf_rep, &max_mtu))
#ifdef HAVE_NET_DEVICE_EXT
		dev->extended->max_mtu = max_mtu;
	dev->extended->min_mtu = ETH_ZLEN;
#else
		dev->max_mtu = max_mtu;
	dev->min_mtu = ETH_ZLEN;
#endif
}

static int bnxt_vf_reps_create(struct bnxt *bp)
{
	u16 *cfa_code_map = NULL, num_vfs = pci_num_vf(bp->pdev);
	struct bnxt_vf_rep *vf_rep;
	struct net_device *dev;
	int rc, i;

	if (!(bp->flags & BNXT_FLAG_DSN_VALID))
		return -ENODEV;

	bp->vf_reps = kcalloc(num_vfs, sizeof(vf_rep), GFP_KERNEL);
	if (!bp->vf_reps)
		return -ENOMEM;

	/* storage for cfa_code to vf-idx mapping */
	cfa_code_map = kmalloc_array(MAX_CFA_CODE, sizeof(*bp->cfa_code_map),
				     GFP_KERNEL);
	if (!cfa_code_map) {
		rc = -ENOMEM;
		goto err;
	}
	for (i = 0; i < MAX_CFA_CODE; i++)
		cfa_code_map[i] = VF_IDX_INVALID;

	if (BNXT_TRUFLOW_EN(bp)) {
		rc = bnxt_tf_port_init(bp);
		if (rc)
			goto tf_init_err;
	}

	for (i = 0; i < num_vfs; i++) {
		dev = alloc_etherdev(sizeof(*vf_rep));
		if (!dev) {
			rc = -ENOMEM;
			goto err;
		}

		vf_rep = netdev_priv(dev);
		bp->vf_reps[i] = vf_rep;
		vf_rep->dev = dev;
		vf_rep->bp = bp;
		vf_rep->vf_idx = i;
		vf_rep->tx_cfa_action = CFA_HANDLE_INVALID;

		if (BNXT_TRUFLOW_EN(bp))
			bnxt_vf_rep_netdev_init(bp, vf_rep, dev);

		rc = bnxt_alloc_vf_rep(bp, vf_rep, cfa_code_map);
		if (rc) {
			if (BNXT_TRUFLOW_EN(bp))
				vf_rep->dev->netdev_ops = NULL;
			goto err;
		}

		if (!BNXT_TRUFLOW_EN(bp))
			bnxt_vf_rep_netdev_init(bp, vf_rep, dev);

		rc = register_netdev(dev);
		if (rc) {
			/* no need for unregister_netdev in cleanup */
			dev->netdev_ops = NULL;
			goto err;
		}
#ifdef CONFIG_BNXT_FLOWER_OFFLOAD
		bnxt_reg_egdev(vf_rep->dev, bnxt_cb_egdev, (void *)vf_rep,
			       vf_rep->vf_idx);
#endif
	}

	/* publish cfa_code_map only after all VF-reps have been initialized */
	bp->cfa_code_map = cfa_code_map;
	bp->eswitch_mode = DEVLINK_ESWITCH_MODE_SWITCHDEV;
	netif_keep_dst(bp->dev);
	return 0;

tf_init_err:
	if (BNXT_TRUFLOW_EN(bp))
		mutex_unlock(&tf_port_lock);
err:
	netdev_err(bp->dev, "Failed to initialize SWITCHDEV mode\n");
	kfree(cfa_code_map);
	__bnxt_vf_reps_destroy(bp);
	return rc;
}

/* Devlink related routines */
int bnxt_dl_eswitch_mode_get(struct devlink *devlink, u16 *mode)
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);

	*mode = bp->eswitch_mode;
	return 0;
}

#ifdef HAVE_ESWITCH_MODE_SET_EXTACK
int bnxt_dl_eswitch_mode_set(struct devlink *devlink, u16 mode,
			     struct netlink_ext_ack *extack)
#else
int bnxt_dl_eswitch_mode_set(struct devlink *devlink, u16 mode)
#endif
{
	struct bnxt *bp = bnxt_get_bp_from_dl(devlink);
	int rc = 0;

	/* If we are going to enable switchdev mode,
	 * allocate memory for VF stats before that.
	 */
	if (mode == DEVLINK_ESWITCH_MODE_SWITCHDEV) {
		rtnl_lock();
		if (!netif_running(bp->dev)) {
			netdev_info(bp->dev,
				    "Bring up the interface before setting switchdev mode\n");
			rtnl_unlock();
			return -EINVAL;
		}
		if (bp->sriov_cfg) {
			netdev_info(bp->dev,
				    "SRIOV is being configured, cannot set switchdev mode\n");
			rtnl_unlock();
			return -EBUSY;
		}
		rtnl_unlock();
		bnxt_alloc_vf_stats_mem(bp);
	}

	mutex_lock(&bp->vf_rep_lock);
	if (bp->eswitch_mode == mode) {
		netdev_info(bp->dev, "already in %s eswitch mode\n",
			    mode == DEVLINK_ESWITCH_MODE_LEGACY ?
			    "legacy" : "switchdev");
		rc = -EINVAL;
		goto done;
	}

	switch (mode) {
	case DEVLINK_ESWITCH_MODE_LEGACY:
		bnxt_vf_reps_destroy(bp);
		break;

	case DEVLINK_ESWITCH_MODE_SWITCHDEV:
		if (bp->hwrm_spec_code < 0x10803) {
			netdev_warn(bp->dev, "FW does not support SRIOV E-Switch SWITCHDEV mode\n");
			rc = -ENOTSUPP;
			goto done;
		}

		if (pci_num_vf(bp->pdev) == 0) {
			netdev_info(bp->dev, "Enable VFs before setting switchdev mode\n");
			rc = -EPERM;
			goto done;
		}
		rc = bnxt_vf_reps_create(bp);
		break;

	default:
		rc = -EINVAL;
		goto done;
	}
done:
	mutex_unlock(&bp->vf_rep_lock);
	/* If we switched back to legacy mode, free VF stats memory. */
	if (bp->eswitch_mode == DEVLINK_ESWITCH_MODE_LEGACY)
		bnxt_free_vf_stats_mem(bp);
	return rc;
}

#endif /* CONFIG_VF_REPS */

#if defined(CONFIG_VF_REPS) || defined(CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD)
int bnxt_hwrm_get_dflt_vnic_svif(struct bnxt *bp, u16 fid,
				 u16 *vnic_id, u16 *svif)
{
	struct hwrm_func_qcfg_output *resp;
	struct hwrm_func_qcfg_input *req;
	u16 svif_info;
	int rc;

	rc = hwrm_req_init(bp, req, HWRM_FUNC_QCFG);
	if (rc)
		return rc;
	req->fid = cpu_to_le16(fid);

	resp = hwrm_req_hold(bp, req);
	rc = hwrm_req_send(bp, req);
	if (rc)
		goto err;

	svif_info = le16_to_cpu(resp->svif_info);
	if (svif && (svif_info & FUNC_QCFG_RESP_SVIF_INFO_SVIF_VALID)) {
		*svif = svif_info & FUNC_QCFG_RESP_SVIF_INFO_SVIF_MASK;
		/* When the VF corresponding to the VFR is down at the time of
		 * VFR conduit creation, the VFR rule will be programmed with
		 * invalid vnic id because FW will return default vnic id as
		 * INVALID when queried through FUNC_QCFG. As a result, when
		 * the VF is brought up, VF won't receive packets because
		 * INVALID vnic id is already programmed.
		 *
		 * Hence, use svif value as vnic id during VFR conduit creation
		 * as both svif and default vnic id values are same and will
		 * never change.
		 */
		if (vnic_id)
			*vnic_id = *svif;
	}

	netdev_dbg(bp->dev, "FID %d SVIF %d VNIC ID %d\n", req->fid, *svif, *vnic_id);
err:
	hwrm_req_drop(bp, req);
	return rc;
}
#endif /* CONFIG_VF_REPS || CONFIG_BNXT_CUSTOM_FLOWER_OFFLOAD */
