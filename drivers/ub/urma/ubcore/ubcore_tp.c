// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022-2025. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * Description: ubcore tp implementation
 * Author: Yan Fangfang
 * Create: 2022-08-25
 * Note:
 * History: 2022-08-25: Create file
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/version.h>
#include "ubcore_log.h"
#include "ubcore_netlink.h"
#include "ubcore_priv.h"
#include <ub/urma/ubcore_uapi.h>
#include "ubcore_tp_table.h"
#include "ubcore_msg.h"
#include "ubcore_vtp.h"
#include <ub/urma/ubcore_jetty.h>
#include "ubcore_uvs.h"
#include "ubcore_tp.h"

#define UB_PROTOCOL_HEAD_BYTES 313
#define UB_MTU_BITS_BASE_SHIFT 7
/* to guarantee all bitmaps filled as 1 */
#define UBCORE_TP_ATTR_MASK 0xFFFFFFFF
#define UBCORE_MAX_TP_EXT_LEN 2048

static inline uint32_t get_udrv_in_len(struct ubcore_udata *udata)
{
	return ((udata == NULL || udata->udrv_data == NULL) ?
			      0 :
			      udata->udrv_data->in_len);
}

static inline int get_udrv_in_data(uint8_t *dst, uint32_t dst_len,
				   struct ubcore_udata *udata)
{
	if (get_udrv_in_len(udata) == 0)
		return 0;

	if (udata->uctx != NULL) {
		if (dst_len < udata->udrv_data->in_len)
			return -1;
		return (int)copy_from_user(
			dst,
			(void __user *)(uintptr_t)udata->udrv_data->in_addr,
			udata->udrv_data->in_len);
	} else {
		(void)memcpy(dst, (void *)udata->udrv_data->in_addr,
			     udata->udrv_data->in_len);
		return 0;
	}
}

static inline int ubcore_mtu_enum_to_int(enum ubcore_mtu mtu)
{
	return 1 << ((int)mtu + UB_MTU_BITS_BASE_SHIFT);
}

enum ubcore_mtu ubcore_get_mtu(int mtu)
{
	int tmp_mtu = mtu - UB_PROTOCOL_HEAD_BYTES;

	if (mtu < 0)
		return 0;

	if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_8192))
		return UBCORE_MTU_8192;
	if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_4096))
		return UBCORE_MTU_4096;
	else if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_2048))
		return UBCORE_MTU_2048;
	else if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_1024))
		return UBCORE_MTU_1024;
	else if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_512))
		return UBCORE_MTU_512;
	else if (tmp_mtu >= ubcore_mtu_enum_to_int(UBCORE_MTU_256))
		return UBCORE_MTU_256;
	else
		return 0;
}
EXPORT_SYMBOL(ubcore_get_mtu);

static int ubcore_set_tp_peer_ext(struct ubcore_tp_attr *attr,
				  uint64_t ext_addr, uint32_t ext_len)
{
	void *peer_ext = NULL;
	int ret;

	/* ext is unused */
	if (ext_len == 0 || ext_addr == 0)
		return 0;

	if (ext_len > UBCORE_MAX_TP_EXT_LEN)
		return -EINVAL;

	/* copy resp ext from req or response */
	peer_ext = kzalloc(ext_len, GFP_KERNEL);
	if (peer_ext == NULL)
		return -ENOMEM;

	ret = (int)copy_from_user(peer_ext, (void __user *)(uintptr_t)ext_addr,
				  ext_len);
	if (ret != 0) {
		kfree(peer_ext);
		return -1;
	}
	attr->peer_ext.addr = (uint64_t)peer_ext;
	attr->peer_ext.len = ext_len;
	return 0;
}

static inline void ubcore_unset_tp_peer_ext(struct ubcore_tp_attr *attr)
{
	if (attr->peer_ext.addr != 0) {
		kfree((void *)attr->peer_ext.addr);
		attr->peer_ext.addr = 0;
		attr->peer_ext.len = 0;
	}
}

static void ubcore_get_ta_data_from_ta(const struct ubcore_ta *ta,
				       enum ubcore_transport_type trans_type,
				       struct ubcore_ta_data *ta_data)
{
	struct ubcore_jetty *jetty;
	struct ubcore_jfs *jfs;

	ta_data->ta_type = ta->type;
	switch (ta->type) {
	case UBCORE_TA_JFS_TJFR:
		jfs = ta->jfs;
		if (jfs->jfs_cfg.eid_index >= jfs->ub_dev->eid_table.eid_cnt ||
		    IS_ERR_OR_NULL(jfs->ub_dev->eid_table.eid_entries))
			return;
		ta_data->jetty_id.eid =
			jfs->ub_dev->eid_table
				.eid_entries[jfs->jfs_cfg.eid_index]
				.eid;
		ta_data->jetty_id.id = jfs->jfs_id.id;
		ta_data->tjetty_id = ta->tjetty_id;
		break;
	case UBCORE_TA_JETTY_TJETTY:
		jetty = ta->jetty;
		if (jetty->jetty_cfg.eid_index >=
			    jetty->ub_dev->eid_table.eid_cnt ||
		    IS_ERR_OR_NULL(jetty->ub_dev->eid_table.eid_entries))
			return;
		ta_data->jetty_id.eid =
			jetty->ub_dev->eid_table
				.eid_entries[jetty->jetty_cfg.eid_index]
				.eid;
		ta_data->jetty_id.id = jetty->jetty_id.id;
		ta_data->tjetty_id = ta->tjetty_id;
		break;
	case UBCORE_TA_NONE:
	case UBCORE_TA_VIRT:
	default:
		return;
	}
	ta_data->trans_type = trans_type;
}

static void ubcore_tp_kref_release(struct kref *ref_cnt)
{
	struct ubcore_tp *tp = container_of(ref_cnt, struct ubcore_tp, ref_cnt);

	complete(&tp->comp);
}

void ubcore_tp_kref_put(struct ubcore_tp *tp)
{
	(void)kref_put(&tp->ref_cnt, ubcore_tp_kref_release);
}

void ubcore_tp_get(void *obj)
{
	struct ubcore_tp *tp = obj;

	kref_get(&tp->ref_cnt);
}

int ubcore_destroy_tp(struct ubcore_tp *tp)
{
	if (tp == NULL)
		return -EINVAL;
	if (!ubcore_have_tp_ops(tp->ub_dev)) {
		ubcore_log_err("TP ops is NULL");
		return -EINVAL;
	}
	if (tp->peer_ext.len > 0 && tp->peer_ext.addr != 0)
		kfree((void *)tp->peer_ext.addr);

	mutex_destroy(&tp->lock);
	return tp->ub_dev->ops->destroy_tp(tp);
}
EXPORT_SYMBOL(ubcore_destroy_tp);

static void ubcore_set_tp_flag(union ubcore_tp_flag *flag,
			       struct ubcore_tp_cfg *cfg,
			       struct ubcore_device *dev)
{
	flag->bs.target = cfg->flag.bs.target;
}

void ubcore_set_tp_init_cfg(struct ubcore_tp *tp, struct ubcore_tp_cfg *cfg)
{
	ubcore_set_tp_flag(&tp->flag, cfg, tp->ub_dev);
	if (tp->ub_dev->transport_type == UBCORE_TRANSPORT_UB &&
	     tp->trans_mode == UBCORE_TP_RC) {
		tp->local_jetty = cfg->local_jetty;
		tp->peer_jetty = cfg->peer_jetty;
	} else {
		tp->local_eid = cfg->local_eid;
		tp->peer_eid = cfg->peer_eid;
	}

	tp->trans_mode = cfg->trans_mode;
	tp->tx_psn = 0;
	tp->retry_num = cfg->retry_num;
	tp->ack_timeout = cfg->ack_timeout;
	tp->dscp = cfg->dscp;
	tp->oor_cnt = cfg->oor_cnt;
}

struct ubcore_tp *ubcore_create_tp(struct ubcore_device *dev,
				   struct ubcore_tp_cfg *cfg,
				   struct ubcore_udata *udata)
{
	struct ubcore_tp *tp = NULL;

	if (!ubcore_have_tp_ops(dev)) {
		ubcore_log_err("Invalid parameter");
		return ERR_PTR(-EINVAL);
	}

	tp = dev->ops->create_tp(dev, cfg, udata);
	if (IS_ERR_OR_NULL(tp)) {
		ubcore_log_err("Failed to create tp towards remote eid %pI6c",
			       &cfg->peer_eid);
		if (tp == NULL)
			return ERR_PTR(-ENOEXEC);
		return tp;
	}
	/* The driver may return the old tp pointer */
	if (tp->state != UBCORE_TP_STATE_RESET)
		return tp;

	tp->ub_dev = dev;
	ubcore_set_tp_init_cfg(tp, cfg);
	tp->state = UBCORE_TP_STATE_RESET;
	ubcore_log_info_rl("tp state:(set to RESET) with tpn %u", tp->tpn);
	tp->priv = NULL;
	atomic_set(&tp->use_cnt, 0);
	mutex_init(&tp->lock);

	return tp;
}

int ubcore_fill_netaddr_macvlan(struct ubcore_net_addr *netaddr,
				struct net_device *netdev,
				enum ubcore_net_addr_type type)
{
	netaddr->type = type;

	if (!netdev)
		return -EINVAL;

	/* UB does not have a mac address
	 * to prevent the duplication of the mac address from hanging
	 */
	if (netdev->type == UBCORE_NETDEV_UB_TYPE) {
		ubcore_log_err("Pure ub does not support uboe mac\n");
		return -1;
	}
	(void)memcpy(netaddr->mac, netdev->dev_addr, netdev->addr_len);
	if (is_vlan_dev(netdev))
		netaddr->vlan = vlan_dev_vlan_id(netdev);
	else
		netaddr->vlan = 0;

	return 0;
}

/* check if current tp state can be truned into new tp state */
int ubcore_modify_tp_state_check(struct ubcore_tp *tp,
				 enum ubcore_tp_state new_state)
{
	int ret = 0;

	switch (tp->state) {
	case UBCORE_TP_STATE_RESET:
		if (new_state != UBCORE_TP_STATE_RTR)
			ret = -1;
		break;
	case UBCORE_TP_STATE_RTR:
		if (new_state != UBCORE_TP_STATE_ERR &&
		    new_state != UBCORE_TP_STATE_RTS)
			ret = -1;
		break;
	case UBCORE_TP_STATE_RTS:
		if (new_state != UBCORE_TP_STATE_ERR &&
		    new_state != UBCORE_TP_STATE_SUSPENDED)
			ret = -1;
		break;
	case UBCORE_TP_STATE_SUSPENDED:
		if (new_state != UBCORE_TP_STATE_RTS &&
		    new_state != UBCORE_TP_STATE_ERR)
			ret = -1;
		break;
	case UBCORE_TP_STATE_ERR:
		/* ERR -> ERR is allowed */
		if (new_state != UBCORE_TP_STATE_ERR &&
		    new_state != UBCORE_TP_STATE_RESET)
			ret = -1;
		break;
	default:
		ret = -1;
		break;
	}

	if (ret != 0) {
		ubcore_log_err(
			"Failed to check modify_tp state: tpn = %u; old_state %u -> new_state %u",
			tp->tpn, (uint32_t)tp->state, (uint32_t)new_state);
	} else {
		ubcore_log_debug(
			"modify_tp state check: tpn = %u; old_state %u -> new_state %u",
			tp->tpn, (uint32_t)tp->state, (uint32_t)new_state);
	}

	return ret;
}

int ubcore_modify_tp_state(struct ubcore_device *dev, struct ubcore_tp *tp,
			   enum ubcore_tp_state new_state,
			   struct ubcore_tp_attr *attr,
			   union ubcore_tp_attr_mask mask)
{
	enum ubcore_tp_state old_state = tp->state;

	mutex_lock(&tp->lock);
	old_state = tp->state;
	if (ubcore_modify_tp_state_check(tp, new_state) != 0) {
		mutex_unlock(&tp->lock);
		return -1;
	}

	if (tp->state == UBCORE_TP_STATE_ERR &&
	    new_state == UBCORE_TP_STATE_ERR) {
		ubcore_log_info("tp is already in ERR state and tpn = %u",
				tp->tpn);
		mutex_unlock(&tp->lock);
		return 0;
	}

	if (dev == NULL || dev->ops == NULL ||
	    dev->ops->modify_tp(tp, attr, mask) != 0) {
		/* tp->peer_ext.addr will be freed when called ubcore_destroy_tp */
		ubcore_log_err(
			"Failed to modify tp to %u from state %u and tpn = %u",
			(uint32_t)new_state, (uint32_t)tp->state, tp->tpn);
		mutex_unlock(&tp->lock);
		return -1;
	}
	tp->state = new_state;
	ubcore_log_info("change tp state:(%u to %u) with tpn %u, peer_tpn %u",
			(uint32_t)old_state, (uint32_t)new_state, tp->tpn,
			tp->peer_tpn);
	mutex_unlock(&tp->lock);
	return 0;
}

static int ubcore_modify_tp_to_rts(const struct ubcore_device *dev,
				   struct ubcore_tp *tp)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	mask.value = 0;
	mask.bs.state = 1;
	attr.state = UBCORE_TP_STATE_RTS;
	attr.tx_psn = 0;

	if (dev->ops->modify_tp(tp, &attr, mask) != 0) {
		/* tp->peer_ext.addr will be freed when called ubcore_destroy_tp */
		ubcore_log_err("Failed to modify tp");
		return -1;
	}
	tp->state = UBCORE_TP_STATE_RTS;
	ubcore_log_debug("tp state:(RTR to RTS) with tpn %u, peer_tpn %u",
			 tp->tpn, tp->peer_tpn);
	return 0;
}

#define ubcore_mod_tp_attr_with_mask(tp, attr, field, mask) \
	(tp->field = mask.bs.field ? attr->field : tp->field)

#define ubcore_log_tp_attr(tp, attr, field, mask)                        \
	do {                                                             \
		if (mask.bs.field) {                                     \
			ubcore_log_info("dev: %s, tp:%u, modify " #field \
					" to: %u ",                      \
					tp->ub_dev->dev_name, tp->tpn,   \
					(uint32_t)tp->field);            \
		}                                                        \
	} while (0)

void ubcore_modify_tp_attr(struct ubcore_tp *tp, struct ubcore_tp_attr *attr,
			   union ubcore_tp_attr_mask mask)
{
	if (mask.bs.flag) {
		tp->flag.bs.oor_en = attr->flag.bs.oor_en;
		tp->flag.bs.sr_en = attr->flag.bs.sr_en;
		tp->flag.bs.cc_en = attr->flag.bs.cc_en;
		tp->flag.bs.cc_alg = attr->flag.bs.cc_alg;
		tp->flag.bs.spray_en = attr->flag.bs.spray_en;
		tp->flag.bs.clan = attr->flag.bs.clan;
	}

	ubcore_mod_tp_attr_with_mask(tp, attr, peer_tpn, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, state, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, tx_psn, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, rx_psn, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, mtu, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, cc_pattern_idx, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, peer_ext, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, oos_cnt, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, local_net_addr_idx, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, peer_net_addr, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, data_udp_start, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, ack_udp_start, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, udp_range, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, hop_limit, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, port_id, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, flow_label, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, mn, mask);
	ubcore_mod_tp_attr_with_mask(tp, attr, peer_trans_type, mask);

	// log for attr change
	ubcore_log_tp_attr(tp, attr, data_udp_start, mask);
}

static int ubcore_set_target_peer(struct ubcore_tp *tp,
				  struct ubcore_tp_attr *attr,
				  union ubcore_tp_attr_mask *mask,
				  struct ubcore_tp_attr *tp_attr,
				  struct ubcore_udata udata)
{
	mask->value = UBCORE_TP_ATTR_MASK;

	memset(attr, 0, sizeof(*attr));
	(void)memcpy(attr, tp_attr, sizeof(struct ubcore_tp_attr));
	attr->tx_psn = tp_attr->rx_psn;
	attr->state = UBCORE_TP_STATE_RTR;

	if (tp->peer_ext.addr != 0)
		return 0;

	return ubcore_set_tp_peer_ext(attr, udata.udrv_data->in_addr,
				      udata.udrv_data->in_len);
}

static void ubcore_set_jetty_for_tp_param(struct ubcore_ta *ta,
					  enum ubcore_transport_mode trans_mode,
					  struct ubcore_vtp_param *vtp_param)
{
	struct ubcore_jetty *jetty;
	struct ubcore_jfs *jfs;

	(void)memset(vtp_param, 0, sizeof(struct ubcore_vtp_param));
	if (ta == NULL)
		return;

	switch (ta->type) {
	case UBCORE_TA_JFS_TJFR:
		jfs = ta->jfs;
		if (jfs->jfs_cfg.eid_index >= jfs->ub_dev->eid_table.eid_cnt ||
		    IS_ERR_OR_NULL(jfs->ub_dev->eid_table.eid_entries))
			return;
		vtp_param->local_eid =
			jfs->ub_dev->eid_table
				.eid_entries[jfs->jfs_cfg.eid_index]
				.eid;
		vtp_param->local_jetty = jfs->jfs_id.id;
		vtp_param->eid_index = jfs->jfs_cfg.eid_index;
		break;
	case UBCORE_TA_JETTY_TJETTY:
		jetty = ta->jetty;
		if (jetty->jetty_cfg.eid_index >=
			    jetty->ub_dev->eid_table.eid_cnt ||
		    IS_ERR_OR_NULL(jetty->ub_dev->eid_table.eid_entries))
			return;
		vtp_param->local_eid =
			jetty->ub_dev->eid_table
				.eid_entries[jetty->jetty_cfg.eid_index]
				.eid;
		vtp_param->local_jetty = jetty->jetty_id.id;
		vtp_param->eid_index = jetty->jetty_cfg.eid_index;
		break;
	case UBCORE_TA_NONE:
	case UBCORE_TA_VIRT:
	default:
		return;
	}
	vtp_param->trans_mode = trans_mode;
	vtp_param->peer_eid = ta->tjetty_id.eid;
	vtp_param->peer_jetty = ta->tjetty_id.id;
	vtp_param->ta = *ta;
}

int ubcore_modify_tp(struct ubcore_device *dev, struct ubcore_tp_node *tp_node,
		     struct ubcore_tp_attr *tp_attr, struct ubcore_udata udata)
{
	struct ubcore_tp *tp = tp_node->tp;
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;
	int ret = 0;

	mutex_lock(&tp_node->lock);

	switch (tp->state) {
	case UBCORE_TP_STATE_RTS:
		ubcore_log_info_rl(
			"Reuse tp state:(RTS) with tpn %u, peer_tpn %u",
			tp->tpn, tp->peer_tpn);
		break;
	case UBCORE_TP_STATE_RESET:
		/* Modify target tp to RTR */
		if (ubcore_set_target_peer(tp, &attr, &mask, tp_attr, udata) !=
		    0) {
			ubcore_log_err("Failed to set target peer");
			ret = -1;
			break;
		}
		if (dev->ops->modify_tp(tp, &attr, mask) != 0) {
			ubcore_unset_tp_peer_ext(&attr);
			ubcore_log_err("Failed to modify tp");
			ret = -1;
			break;
		}
		ubcore_modify_tp_attr(tp, &attr, mask);
		ubcore_log_info_rl(
			"tp state:(RESET to RTR) with tpn %u, peer_tpn %u",
			tp->tpn, tp->peer_tpn);
		break;
	case UBCORE_TP_STATE_RTR:
		ret = ubcore_modify_tp_to_rts(dev, tp);
		ubcore_log_info_rl(
			"tp state:(RTR to RTS) with tpn %u, peer_tpn %u",
			tp->tpn, tp->peer_tpn);
		break;
	case UBCORE_TP_STATE_SUSPENDED:
		ubcore_log_info_rl("tp state: TP_STATE_SUSPENDED\n");
		fallthrough;

	case UBCORE_TP_STATE_ERR:
		ubcore_log_info_rl("tp state: TP_STATE_ERR\n");
		fallthrough;

	default:
		ret = -1;
		break;
	}

	mutex_unlock(&tp_node->lock);
	return ret;
}

static int ubcore_parse_ta(struct ubcore_device *dev,
			   struct ubcore_ta_data *ta_data,
			   struct ubcore_tp_meta *meta)
{
	struct ubcore_jetty *jetty;
	struct ubcore_jfs *jfs;

	switch (ta_data->ta_type) {
	case UBCORE_TA_JFS_TJFR:
		jfs = ubcore_find_get_jfs(dev, ta_data->tjetty_id.id);
		if (jfs != NULL) {
			meta->ht = ubcore_get_tptable(jfs->tptable);
			ubcore_put_jfs(jfs);
		}
		break;
	case UBCORE_TA_JETTY_TJETTY:
		/* todonext: add kref to jetty, as it may be destroyed any time */
		jetty = ubcore_find_get_jetty(dev, ta_data->tjetty_id.id);
		if (jetty != NULL) {
			if (jetty->jetty_cfg.trans_mode == UBCORE_TP_RC &&
			    jetty->remote_jetty != NULL &&
			    memcmp(&jetty->remote_jetty->cfg.id,
				   &ta_data->jetty_id,
				   sizeof(struct ubcore_jetty_id))) {
				ubcore_log_err(
					"the same jetty is binded with another remote jetty.\n");
				ubcore_put_jetty(jetty);
				return -1;
			}
			meta->ht = ubcore_get_tptable(jetty->tptable);
			ubcore_put_jetty(jetty);
		}
		break;
	case UBCORE_TA_NONE:
	case UBCORE_TA_VIRT:
	default:
		return -1;
	}
	ubcore_init_tp_key_jetty_id(&meta->key, &ta_data->jetty_id);

	/* jetty and jfs should be indexed consecutively */
	meta->hash = ubcore_get_jetty_hash(&ta_data->jetty_id);
	return 0;
}

static int ubcore_init_create_tp_req(struct ubcore_device *dev,
				     struct ubcore_vtp_param *tp_param,
				     struct ubcore_tp *tp,
				     struct ubcore_udata *udata,
				     struct ubcore_create_vtp_req *data)
{
	data->trans_mode = tp_param->trans_mode;
	data->local_eid = tp_param->local_eid;
	data->peer_eid = tp_param->peer_eid;
	data->eid_index = tp_param->eid_index;
	data->local_jetty = tp_param->local_jetty;
	data->peer_jetty = tp_param->peer_jetty;
	(void)strscpy(data->dev_name, dev->dev_name, UBCORE_MAX_DEV_NAME - 1);
	data->virtualization = dev->attr.virtualization;

	ubcore_get_ta_data_from_ta(&tp_param->ta, dev->transport_type,
				   &data->ta_data);
	data->udrv_in_len = get_udrv_in_len(udata);
	data->ext_len = tp->tp_ext.len;

	if (get_udrv_in_data(data->udrv_ext, get_udrv_in_len(udata), udata) !=
	    0) {
		ubcore_log_err("Failed to get udrv data");
		return -1;
	}
	if (tp->tp_ext.len > 0)
		(void)memcpy(data->udrv_ext + get_udrv_in_len(udata),
			     (void *)tp->tp_ext.addr, tp->tp_ext.len);

	return 0;
}

static int ubcore_send_create_tp_req(struct ubcore_device *dev,
				     struct ubcore_vtp_param *tp_param,
				     struct ubcore_tp *tp,
				     struct ubcore_udata *udata)
{
	struct ubcore_create_vtp_req *data;
	struct ubcore_req *req_msg;
	uint32_t payload_len;
	uint32_t udata_len;
	uint32_t tp_len;

	/* dev has been unregistered and the message channel has been down */
	if (ubcore_check_dev_is_exist(dev->dev_name) == false)
		return -ENONET;

	tp_len = tp->tp_ext.len;
	udata_len = get_udrv_in_len(udata);
	if (tp_len + udata_len > UBCORE_MAX_UDRV_EXT_LEN ||
	    tp_len + udata_len < udata_len) {
		ubcore_log_err("Invalid len tp_len:%u, udata_len:%u", tp_len,
			       udata_len);
		return -EINVAL;
	}

	payload_len = (uint32_t)sizeof(struct ubcore_create_vtp_req);
	req_msg =
		kcalloc(1, sizeof(struct ubcore_req) + payload_len, GFP_KERNEL);
	if (req_msg == NULL)
		return -ENOMEM;

	req_msg->opcode = UBCORE_MSG_CREATE_VTP;
	req_msg->len = payload_len;
	data = (struct ubcore_create_vtp_req *)req_msg->data;
	if (ubcore_init_create_tp_req(dev, tp_param, tp, udata, data) != 0) {
		kfree(req_msg);
		return -ENOEXEC;
	}
	kfree(req_msg);
	return 0;
}

static int ubcore_send_del_tp_req(struct ubcore_device *dev,
				  struct ubcore_vtp_param *tp_param)
{
	struct ubcore_create_vtp_req *data;
	struct ubcore_req *req_msg;

	/* dev has been unregistered and the message channel has been down */
	if (ubcore_check_dev_is_exist(dev->dev_name) == false)
		return -ENONET;

	req_msg = kcalloc(1,
			  sizeof(struct ubcore_req) +
				  sizeof(struct ubcore_create_vtp_req),
			  GFP_KERNEL);
	if (req_msg == NULL)
		return -ENOMEM;

	req_msg->opcode = UBCORE_MSG_DESTROY_VTP;
	req_msg->len = sizeof(struct ubcore_create_vtp_req);
	data = (struct ubcore_create_vtp_req *)req_msg->data;
	data->trans_mode = tp_param->trans_mode;
	data->local_eid = tp_param->local_eid;
	data->peer_eid = tp_param->peer_eid;
	data->eid_index = tp_param->eid_index;
	data->local_jetty = tp_param->local_jetty;
	data->peer_jetty = tp_param->peer_jetty;
	(void)strscpy(data->dev_name, dev->dev_name, UBCORE_MAX_DEV_NAME - 1);
	data->virtualization = dev->attr.virtualization;
	/* for alpha start */
	ubcore_get_ta_data_from_ta(&tp_param->ta, dev->transport_type,
				   &data->ta_data);
	kfree(req_msg);
	return 0;
}

static struct ubcore_nlmsg *
ubcore_get_query_tp_req(struct ubcore_device *dev,
			enum ubcore_transport_mode trans_mode)
{
	uint32_t payload_len = sizeof(struct ubcore_nl_query_tp_req);
	struct ubcore_nl_query_tp_req *query;
	struct ubcore_nlmsg *req;

	req = kzalloc(sizeof(struct ubcore_nlmsg) + payload_len, GFP_KERNEL);
	if (req == NULL)
		return NULL;

	req->transport_type = dev->transport_type;
	req->msg_type = UBCORE_CMD_QUERY_TP_REQ;
	req->payload_len = payload_len;
	query = (struct ubcore_nl_query_tp_req *)req->payload;
	query->trans_mode = trans_mode;
	(void)memcpy(query->dev_name, dev->dev_name, UBCORE_MAX_DEV_NAME);
	query->ue_idx = dev->attr.ue_idx;
	return req;
}

static int ubcore_query_tp(struct ubcore_device *dev,
			   enum ubcore_transport_mode trans_mode,
			   struct ubcore_nl_query_tp_resp *query_tp_resp)
{
	struct ubcore_nlmsg *req_msg, *resp_msg;
	struct ubcore_uvs_instance *uvs = NULL;
	struct ubcore_nl_query_tp_resp *resp;
	int ret = 0;

	req_msg = ubcore_get_query_tp_req(dev, trans_mode);
	if (req_msg == NULL) {
		ubcore_log_err("Failed to get query tp req");
		return -1;
	}

	uvs = ubcore_find_get_uvs_by_ue(dev, dev->attr.ue_idx);
	if (uvs == NULL) {
		ubcore_log_err("Failed to find uvs for ue %u",
			       dev->attr.ue_idx);
		kfree(req_msg);
		return -1;
	}

	resp_msg = ubcore_nl_send_wait(dev, req_msg, uvs);
	if (resp_msg == NULL) {
		ubcore_log_err("Failed to wait query response");
		ubcore_uvs_kref_put(uvs);
		kfree(req_msg);
		return -1;
	}

	resp = (struct ubcore_nl_query_tp_resp *)(void *)resp_msg->payload;
	if (resp_msg->msg_type != UBCORE_CMD_QUERY_TP_RESP || resp == NULL ||
	    resp->ret != UBCORE_NL_RESP_SUCCESS) {
		ret = -1;
		ubcore_log_err(
			"Query tp request is rejected with type %d ret %d",
			resp_msg->msg_type, (resp == NULL ? 1 : resp->ret));
	} else {
		(void)memcpy(query_tp_resp, resp,
			     sizeof(struct ubcore_nl_query_tp_resp));
	}

	ubcore_uvs_kref_put(uvs);
	kfree(resp_msg);
	kfree(req_msg);
	return ret;
}

static void
ubcore_set_initiator_tp_cfg(struct ubcore_tp_cfg *cfg,
			    struct ubcore_vtp_param *tp_param,
			    struct ubcore_nl_query_tp_resp *query_tp_resp)
{
	cfg->flag.bs.target = 0;
	cfg->local_jetty.eid = tp_param->local_eid;
	cfg->local_jetty.id = tp_param->local_jetty;
	cfg->peer_jetty.eid = tp_param->peer_eid;
	cfg->peer_jetty.id = tp_param->peer_jetty;
	cfg->trans_mode = tp_param->trans_mode;
	cfg->retry_factor = query_tp_resp->retry_factor;
	cfg->retry_num = query_tp_resp->retry_num;
	cfg->ack_timeout = query_tp_resp->ack_timeout;
	cfg->dscp = query_tp_resp->dscp;
	cfg->oor_cnt = query_tp_resp->oor_cnt;
}

static int ubcore_query_initiator_tp_cfg(struct ubcore_tp_cfg *cfg,
					 struct ubcore_device *dev,
					 struct ubcore_vtp_param *tp_param)
{
	struct ubcore_nl_query_tp_resp query_tp_resp;

	if (ubcore_query_tp(dev, tp_param->trans_mode, &query_tp_resp) != 0) {
		ubcore_log_err("Failed to query tp");
		return -1;
	}
	ubcore_set_initiator_tp_cfg(cfg, tp_param, &query_tp_resp);
	return 0;
}

/* udata may be empty because the data may come from the user space or kernel space. */
int ubcore_bind_tp(struct ubcore_jetty *jetty, struct ubcore_tjetty *tjetty,
		   struct ubcore_tp_advice *advice, struct ubcore_udata *udata)
{
	struct ubcore_vtp_param tp_param = { 0 };
	struct ubcore_tp_cfg tp_cfg = { 0 };
	struct ubcore_tp *new_tp = NULL;
	struct ubcore_tp_node *tp_node;
	struct ubcore_device *dev;

	if (jetty == NULL || tjetty == NULL || advice == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}
	dev = jetty->ub_dev;

	ubcore_set_jetty_for_tp_param(&advice->ta, UBCORE_TP_RC, &tp_param);
	if (ubcore_query_initiator_tp_cfg(&tp_cfg, dev, &tp_param) != 0) {
		ubcore_log_err("Failed to init tp cfg.\n");
		return -1;
	}
	/* driver gurantee to return the same tp if we have created it as a target */
	new_tp = ubcore_create_tp(dev, &tp_cfg, udata);
	if (IS_ERR_OR_NULL(new_tp)) {
		ubcore_log_err("Failed to create tp");
		return PTR_ERR(new_tp);
	}
	tp_node = ubcore_add_tp_node(advice->meta.ht, advice->meta.hash,
				     &advice->meta.key, new_tp, &advice->ta);
	if (tp_node == NULL) {
		(void)ubcore_destroy_tp(new_tp);
		ubcore_log_err("Failed to find and add tp\n");
		return -1;
	} else if (tp_node != NULL && tp_node->tp != new_tp) {
		(void)ubcore_destroy_tp(new_tp);
		new_tp = NULL;
	}

	mutex_lock(&tjetty->lock);
	if (tjetty->tp != NULL) {
		mutex_unlock(&tjetty->lock);
		ubcore_tpnode_kref_put(tp_node);
		ubcore_find_remove_tp(advice->meta.ht, advice->meta.hash,
				      &advice->meta.key);
		ubcore_log_err(
			"The same tjetty, different jetty, prevent duplicate bind.\n");
		return -1;
	}

	if (ubcore_send_create_tp_req(dev, &tp_param, tp_node->tp, udata) !=
	    0) {
		ubcore_log_err("Failed to send tp req");
		mutex_unlock(&tjetty->lock);
		ubcore_tpnode_kref_put(tp_node);
		ubcore_find_remove_tp(advice->meta.ht, advice->meta.hash,
				      &advice->meta.key);
		return -1;
	}
	tjetty->tp = tp_node->tp;
	ubcore_tpnode_kref_put(tp_node);
	mutex_unlock(&tjetty->lock);
	return 0;
}
EXPORT_SYMBOL(ubcore_bind_tp);

int ubcore_unbind_tp(struct ubcore_jetty *jetty, struct ubcore_tjetty *tjetty,
		     struct ubcore_tp_advice *advice)
{
	struct ubcore_vtp_param tp_param;

	if (jetty == NULL || tjetty == NULL || advice == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	mutex_lock(&tjetty->lock);
	if (tjetty->tp == NULL) {
		mutex_unlock(&tjetty->lock);
		ubcore_log_warn(
			"TP is not found, already removed or under use\n");
		return 0;
	}
	mutex_unlock(&tjetty->lock);

	ubcore_set_jetty_for_tp_param(&advice->ta, UBCORE_TP_RC, &tp_param);
	if (ubcore_send_del_tp_req(jetty->ub_dev, &tp_param) != 0) {
		ubcore_log_warn("failed to unbind tp\n");
		/* It does not depend on the success of the peer TP,
		 * but depends on the success of the local cleanup,
		 * otherwise the TP remains.
		 */
	}
	ubcore_find_remove_tp(advice->meta.ht, advice->meta.hash,
			      &advice->meta.key);

	mutex_lock(&tjetty->lock);
	tjetty->tp = NULL;
	mutex_unlock(&tjetty->lock);
	return 0;
}
EXPORT_SYMBOL(ubcore_unbind_tp);

/* udata may be empty because the data may come from the user space or kernel space. */
int ubcore_advise_tp(struct ubcore_device *dev, union ubcore_eid *remote_eid,
		     struct ubcore_tp_advice *advice,
		     struct ubcore_udata *udata)
{
	struct ubcore_vtp_param tp_param = { 0 };
	struct ubcore_tp_cfg tp_cfg = { 0 };
	struct ubcore_tp_node *tp_node;
	struct ubcore_tp *new_tp;

	if (dev == NULL || remote_eid == NULL || advice == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	/* Must call driver->create_tp with udata if we are advising jetty */
	tp_node = ubcore_lookup_tpnode(advice->meta.ht, advice->meta.hash,
				       &advice->meta.key);
	if (tp_node != NULL && tp_node->tp != NULL &&
	    !tp_node->tp->flag.bs.target) {
		ubcore_tpnode_kref_put(tp_node);
		return 0;
	}

	ubcore_set_jetty_for_tp_param(&advice->ta, UBCORE_TP_RM, &tp_param);
	if (ubcore_query_initiator_tp_cfg(&tp_cfg, dev, &tp_param) != 0) {
		ubcore_log_err("Failed to init tp cfg.\n");
		return -1;
	}
	/* advise tp requires the user to pass in the pin memory operation
	 * and cannot be used in the uvs context ioctl to create tp
	 */
	new_tp = ubcore_create_tp(dev, &tp_cfg, udata);
	if (IS_ERR_OR_NULL(new_tp)) {
		ubcore_log_err("Failed to create tp");
		return PTR_ERR(new_tp);
	}
	tp_node = ubcore_add_tp_node(advice->meta.ht, advice->meta.hash,
				     &advice->meta.key, new_tp, &advice->ta);
	if (tp_node == NULL) {
		(void)ubcore_destroy_tp(new_tp);
		ubcore_log_err("Failed to find and add tp\n");
		return -1;
	} else if (tp_node != NULL && tp_node->tp != new_tp) {
		(void)ubcore_destroy_tp(new_tp);
		new_tp = NULL;
	}

	if (ubcore_send_create_tp_req(dev, &tp_param, tp_node->tp, udata) !=
	    0) {
		ubcore_tpnode_kref_put(tp_node);
		ubcore_find_remove_tp(advice->meta.ht, advice->meta.hash,
				      &advice->meta.key);
		ubcore_log_err("Failed to send tp req");
		return -1;
	}
	ubcore_tpnode_kref_put(tp_node);
	return 0;
}
EXPORT_SYMBOL(ubcore_advise_tp);

int ubcore_unadvise_tp(struct ubcore_device *dev,
		       struct ubcore_tp_advice *advice)
{
	struct ubcore_vtp_param tp_param;
	int ret;

	if (dev == NULL || advice == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	ubcore_set_jetty_for_tp_param(&advice->ta, UBCORE_TP_RM, &tp_param);
	ret = ubcore_send_del_tp_req(dev, &tp_param);
	if (ret != 0) {
		ubcore_log_warn("failed to unadvise tp\n");
		/* It does not depend on the success of the peer TP,
		 * but depends on the success of the local cleanup,
		 * otherwise the TP remains.
		 */
	}
	ubcore_find_remove_tp(advice->meta.ht, advice->meta.hash,
			      &advice->meta.key);
	return 0;
}
EXPORT_SYMBOL(ubcore_unadvise_tp);

static void ubcore_get_ta_from_tp(struct ubcore_ta *ta, struct ubcore_tp *tp)
{
	struct ubcore_tp_node *tp_node = (struct ubcore_tp_node *)tp->priv;

	ta->type = UBCORE_TA_NONE;
	switch (tp->trans_mode) {
	case UBCORE_TP_RC:
	case UBCORE_TP_RM:
		/* ta is none for UB native device */
		if (tp_node != NULL)
			*ta = tp_node->ta;
		break;
	case UBCORE_TP_UM:
	default:
		break;
	}
}

static struct ubcore_nlmsg *ubcore_get_restore_tp_req(struct ubcore_tp *tp)
{
	uint32_t payload_len =
		(uint32_t)sizeof(struct ubcore_nl_restore_tp_req);
	struct ubcore_nl_restore_tp_req *restore;
	struct ubcore_ta ta;
	struct ubcore_nlmsg *req;

	req = ubcore_alloc_nlmsg(payload_len, &tp->local_eid, &tp->peer_eid);
	if (req == NULL)
		return NULL;

	req->transport_type = tp->ub_dev->transport_type;
	req->msg_type = UBCORE_CMD_RESTORE_TP_REQ;
	restore = (struct ubcore_nl_restore_tp_req *)(void *)req->payload;
	restore->trans_mode = tp->trans_mode;
	restore->tpn = tp->tpn;
	restore->peer_tpn = tp->peer_tpn;
	restore->rx_psn = get_random_u32();

	ubcore_get_ta_from_tp(&ta, tp);
	ubcore_get_ta_data_from_ta(&ta, tp->ub_dev->transport_type,
				   &restore->ta);

	return req;
}

static struct ubcore_nlmsg *
ubcore_get_restore_tp_response(struct ubcore_nlmsg *req, uint32_t rx_psn,
			       enum ubcore_nl_resp_status status)
{
	struct ubcore_nl_restore_tp_resp *restore_resp;
	struct ubcore_nlmsg *resp = NULL;

	resp = ubcore_alloc_nlmsg(sizeof(struct ubcore_nl_restore_tp_resp),
				  &req->dst_eid, &req->src_eid);
	if (resp == NULL) {
		ubcore_log_err("Failed to alloc restore tp response");
		return NULL;
	}

	resp->msg_type = UBCORE_CMD_RESTORE_TP_RESP;
	resp->nlmsg_seq = req->nlmsg_seq;
	resp->transport_type = req->transport_type;
	restore_resp = (struct ubcore_nl_restore_tp_resp *)resp->payload;

	if (status == UBCORE_NL_RESP_FAIL) {
		restore_resp->ret = UBCORE_NL_RESP_FAIL;
		return resp;
	}

	restore_resp->peer_rx_psn = rx_psn;
	return resp;
}

static int ubcore_restore_tp_to_reset(struct ubcore_device *dev,
				      struct ubcore_tp *tp)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	mask.value = 0;
	mask.bs.state = 1;
	attr.state = UBCORE_TP_STATE_RESET;
	ubcore_log_debug("restore tp to reset(mask): state: %u", mask.bs.state);
	ubcore_log_debug("restore tp to reset(attr): state: %u",
			 (uint32_t)attr.state);

	if (ubcore_modify_tp_state(dev, tp, UBCORE_TP_STATE_RESET, &attr,
				   mask) != 0)
		return -1;

	return 0;
}

static int ubcore_restore_tp_to_rts(struct ubcore_device *dev,
				    struct ubcore_tp *tp, uint32_t rx_psn,
				    uint32_t tx_psn)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	mask.value = 0;
	mask.bs.state = 1;
	mask.bs.rx_psn = 1;
	mask.bs.tx_psn = 1;

	attr.state = UBCORE_TP_STATE_RTS;
	attr.rx_psn = rx_psn;
	attr.tx_psn = tx_psn;
	ubcore_log_info(
		"restore tp to rts(mask): state: %u, rx_psn: %u, tx_psn: %u",
		mask.bs.state, mask.bs.rx_psn, mask.bs.tx_psn);
	ubcore_log_info(
		"restore tp to rts(attr): state: %u, rx_psn: %u, tx_psn: %u",
		(uint32_t)attr.state, attr.rx_psn, attr.tx_psn);

	if (ubcore_modify_tp_state(dev, tp, UBCORE_TP_STATE_RTS, &attr, mask) !=
	    0)
		return -1;

	tp->rx_psn = rx_psn;
	tp->tx_psn = tx_psn;

	return 0;
}

int ubcore_restore_tp_error_to_rtr(struct ubcore_device *dev,
				   struct ubcore_tp *tp, uint32_t rx_psn,
				   uint32_t tx_psn, uint16_t data_udp_start,
				   uint16_t ack_udp_start)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	/* Tp param problem
	 * 1. In driver, tp params are stored in tp ctx. In ubcore, tp params are divided to store
	 *    tp cfg and tp attr. tp cfg stores params that are determined when create tp and will
	 *    not be updated then. tp attr stores params that may be updated after tp creation.
	 * 2. During tp err restoration, driver reset tp ctx when transitioned to RESET.
	 *    So, it needs to set all tp params when transitions to RTR.
	 * 3. Solution negotiated with driver:
	 * (1) For params in tp cfg, ubcore can't pass them down to driver in current restoration
	 *     procedure. Driver needs to set these part of param by ubcore_tp *tp.
	 * (2) For params in tp attr, driver will set them by tp attr according to the mask.
	 *     So, ubcore has to pass down all of them. Not only the updated.
	 */
	// set all bits in mask to pass down all params in tp attr
	mask.value = UBCORE_TP_ATTR_MASK;
	// updated params
	attr.state = UBCORE_TP_STATE_RTR;
	attr.rx_psn = rx_psn;
	attr.tx_psn = tx_psn;
	attr.data_udp_start = data_udp_start;
	attr.ack_udp_start = ack_udp_start;
	// not updated params. Set by ubcore_tp *tp
	attr.flag.bs.oor_en = tp->flag.bs.oor_en;
	attr.flag.bs.sr_en = tp->flag.bs.sr_en;
	attr.flag.bs.cc_en = tp->flag.bs.cc_en;
	attr.flag.bs.cc_alg = tp->flag.bs.cc_alg;
	attr.flag.bs.spray_en = tp->flag.bs.spray_en;
	attr.flag.bs.clan = tp->flag.bs.clan;
	attr.peer_tpn = tp->peer_tpn;
	attr.mtu = tp->mtu;
	attr.cc_pattern_idx = tp->cc_pattern_idx;
	attr.peer_ext = tp->peer_ext;
	attr.oos_cnt = tp->oos_cnt;
	attr.local_net_addr_idx = tp->local_net_addr_idx;
	attr.peer_net_addr = tp->peer_net_addr;
	attr.udp_range = tp->udp_range;
	attr.hop_limit = tp->hop_limit;
	attr.flow_label = tp->flow_label;
	attr.port_id = tp->port_id;
	attr.mn = tp->mn;
	attr.peer_trans_type = tp->peer_trans_type;

	ubcore_log_info(
		"restore tp to rtr(mask): state: %u, rx_psn: %u, tx_psn: %u, data_udp: %u, ack_udp: %u",
		mask.bs.state, mask.bs.rx_psn, mask.bs.tx_psn,
		mask.bs.data_udp_start, mask.bs.ack_udp_start);
	ubcore_log_info(
		"restore tp to rtr(attr): state: %u, rx_psn: %u, tx_psn: %u, data_udp: %u, ack_udp: %u",
		(uint32_t)attr.state, attr.rx_psn, attr.tx_psn,
		attr.data_udp_start, attr.ack_udp_start);

	if (ubcore_modify_tp_state(dev, tp, UBCORE_TP_STATE_RTR, &attr, mask) !=
	    0)
		return -1;

	tp->rx_psn = rx_psn;
	tp->tx_psn = tx_psn;
	tp->data_udp_start = data_udp_start;
	tp->ack_udp_start = ack_udp_start;

	return 0;
}

int ubcore_restore_tp_error_to_rts(struct ubcore_device *dev,
				   struct ubcore_tp *tp)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	mask.value = 0;
	mask.bs.state = 1;

	attr.state = UBCORE_TP_STATE_RTS;

	ubcore_log_info("restore tp to rts, state mask: %u state: %u",
			mask.bs.state, (uint32_t)attr.state);

	if (ubcore_modify_tp_state(dev, tp, UBCORE_TP_STATE_RTS, &attr, mask) !=
	    0)
		return -1;

	return 0;
}

int ubcore_change_tp_to_err(struct ubcore_device *dev, struct ubcore_tp *tp)
{
	union ubcore_tp_attr_mask mask;
	struct ubcore_tp_attr attr;

	mask.value = 0;
	mask.bs.state = 1;

	attr.state = UBCORE_TP_STATE_ERR;

	if (ubcore_modify_tp_state(dev, tp, UBCORE_TP_STATE_ERR, &attr, mask) !=
	    0)
		return -1;

	return 0;
}

void ubcore_restore_tp(struct ubcore_device *dev, struct ubcore_tp *tp)
{
	struct ubcore_nlmsg *req_msg, *resp_msg;
	struct ubcore_uvs_instance *uvs = NULL;
	struct ubcore_nl_restore_tp_resp *resp;
	struct ubcore_nl_restore_tp_req *req;

	/* Currently, only try to restore tp in the UBCORE_TRANSPORT_HNS_UB device,
	 * Do not send retore tp req from target to inititor,
	 * Do not restore UM TP, as it is only visable by the driver
	 */
	if (!ubcore_have_tp_ops(dev) || tp == NULL ||
	    tp->flag.bs.target || tp->priv == NULL ||
	    tp->trans_mode == UBCORE_TP_UM ||
	    tp->state != UBCORE_TP_STATE_ERR || tp->ub_dev == NULL)
		return;

	req_msg = ubcore_get_restore_tp_req(tp);
	if (req_msg == NULL) {
		ubcore_log_err("Failed to get restore tp req");
		return;
	}

	uvs = ubcore_find_get_uvs_by_ue(dev, tp->ue_idx);
	if (uvs == NULL) {
		ubcore_log_err("Failed to find uvs for ue %u", tp->ue_idx);
		kfree(req_msg);
		return;
	}

	resp_msg = ubcore_nl_send_wait(dev, req_msg, uvs);
	if (resp_msg == NULL) {
		ubcore_log_err("Failed to wait restore tp response %pI6c",
			       &tp->peer_eid);
		ubcore_uvs_kref_put(uvs);
		kfree(req_msg);
		return;
	}

	req = (struct ubcore_nl_restore_tp_req *)(void *)req_msg->payload;
	resp = (struct ubcore_nl_restore_tp_resp *)(void *)resp_msg->payload;
	if (resp_msg->msg_type != req_msg->msg_type + 1 || resp == NULL ||
	    resp->ret != UBCORE_NL_RESP_SUCCESS) {
		ubcore_log_err(
			"Restore tp request is rejected with type %d from uvs %s, id %u, ret %d",
			resp_msg->msg_type, uvs->name, uvs->id,
			(resp == NULL ? 1 : resp->ret));
		ubcore_uvs_kref_put(uvs);
		kfree(resp_msg);
		kfree(req_msg);
		return;
	}

	if (ubcore_restore_tp_to_rts(dev, tp, req->rx_psn, resp->peer_rx_psn) !=
	    0)
		ubcore_log_err("Failed to restore tp with tpn %u", tp->tpn);

	ubcore_uvs_kref_put(uvs);
	kfree(req_msg);
	kfree(resp_msg);
	ubcore_log_info("Restored tp with tpn %u", tp->tpn);
}
EXPORT_SYMBOL(ubcore_restore_tp);

static struct ubcore_nlmsg *
ubcore_get_tp_flush_done_req(struct ubcore_device *dev, struct ubcore_tp *tp)
{
	uint32_t payload_len =
		(uint32_t)sizeof(struct ubcore_tp_flush_done_req);
	struct ubcore_tp_flush_done_req *flush_done_req;
	struct ubcore_nlmsg *req;

	if (tp->ub_dev == NULL) {
		ubcore_log_err("Invalid parameter, tpn: %u", tp->tpn);
		return NULL;
	}
	req = ubcore_alloc_nlmsg(payload_len, &tp->local_eid, &tp->peer_eid);
	if (req == NULL)
		return NULL;

	req->transport_type = tp->ub_dev->transport_type;
	req->msg_type = UBCORE_CMD_TP_FLUSH_DONE_REQ;
	flush_done_req =
		(struct ubcore_tp_flush_done_req *)(void *)req->payload;
	flush_done_req->tpn = tp->tpn;
	flush_done_req->data_udp_start = tp->data_udp_start;
	flush_done_req->ack_udp_start = tp->ack_udp_start;
	flush_done_req->tx_psn = tp->tx_psn;
	flush_done_req->peer_tpn = tp->peer_tpn;
	flush_done_req->trans_mode = tp->trans_mode;
	flush_done_req->sip_idx = tp->local_net_addr_idx;
	if (tp->tpg) {
		flush_done_req->tpgn = tp->tpg->tpgn;
		flush_done_req->sip = tp->tpg->tpg_cfg.local_net_addr;
	}

	flush_done_req->local_eid = tp->local_eid;
	flush_done_req->peer_eid = tp->peer_eid;
	ubcore_log_debug(
		"report tp flush done: tx_psn: %u, data_udp: %u, ack_udp: %u",
		tp->tx_psn, tp->data_udp_start, tp->ack_udp_start);
	if (tp->trans_mode == UBCORE_TP_RC) {
		flush_done_req->local_jetty_id = tp->local_jetty.id;
		flush_done_req->peer_jetty_id = tp->peer_jetty.id;
	}
	(void)memcpy(flush_done_req->mue_dev_name, dev->dev_name,
		     UBCORE_MAX_DEV_NAME);

	return req;
}

static void ubcore_report_tp_flush_done_to_uvs(struct ubcore_nlmsg *req_msg,
					       struct ubcore_uvs_instance *uvs)
{
	int ret;

	ret = ubcore_nl_send_nowait_without_cb(req_msg, uvs);
	if (ret != 0)
		ubcore_log_err("Failed to nowait send tp error request to %s\n",
			       uvs->name);
	else
		ubcore_log_debug(
			"Success to nowait send tp error request to %s\n",
			uvs->name);
}

void ubcore_report_tp_flush_done(struct ubcore_device *dev,
				 struct ubcore_tp *tp)
{
	struct ubcore_uvs_instance **uvs_list = NULL;
	struct ubcore_nlmsg *req_msg;
	int count = 0;
	int i;

	if (tp->state != UBCORE_TP_STATE_RESET) {
		/* udma flushes without changing tp to ERR when qpc err. Thus, ubcore needs to
		 * change tp to ERR first.
		 */
		if (ubcore_change_tp_to_err(dev, tp) != 0) {
			ubcore_log_err("Failed to change tp: %u to err",
				       tp->tpn);
			return;
		}

		/*
		 * For 1650, modifying TP from ERR -> RESET is not supported.
		 * The result will not affect TP destroy.
		 */
		if (ubcore_restore_tp_to_reset(dev, tp) != 0)
			ubcore_log_err("Failed to restore tp: %u to reset",
				       tp->tpn);
	}

	req_msg = ubcore_get_tp_flush_done_req(dev, tp);
	if (req_msg == NULL) {
		ubcore_log_err("Failed to get tp flush done req");
		return;
	}

	/* Due to async reason, ue2uvs mapping is possibly already removed at this point.
	 * In this case, we should try to send flush done request to all UVS to ensure
	 * flush done count can be decreased correctly in UVS.
	 */
	uvs_list = ubcore_uvs_list_get_all_alive(&count);
	if (uvs_list == NULL || count == 0) {
		ubcore_log_err("Failed to find uvs for ue %u", tp->ue_idx);
		kfree(req_msg);
		return;
	}

	for (i = 0; i < count; i++)
		ubcore_report_tp_flush_done_to_uvs(req_msg, uvs_list[i]);

	ubcore_uvs_list_put(uvs_list, count);
	kfree(req_msg);
}

static struct ubcore_nlmsg *ubcore_get_tp_suspend_req(struct ubcore_device *dev,
						      struct ubcore_tp *tp)
{
	uint32_t payload_len = (uint32_t)sizeof(struct ubcore_tp_suspend_req);
	struct ubcore_tp_suspend_req *suspend_req;
	struct ubcore_nlmsg *req;

	/* tp is checked in outer function */
	if (tp->ub_dev == NULL || tp->tpg == NULL) {
		ubcore_log_err("Invalid parameter, tpn: %u.\n", tp->tpn);
		return NULL;
	}
	req = ubcore_alloc_nlmsg(payload_len, &tp->local_eid, &tp->peer_eid);
	if (req == NULL)
		return NULL;

	req->transport_type = tp->ub_dev->transport_type;
	req->msg_type = UBCORE_CMD_TP_SUSPEND_REQ;
	suspend_req = (struct ubcore_tp_suspend_req *)(void *)req->payload;
	suspend_req->tpgn = tp->tpg->tpgn;
	suspend_req->tpn = tp->tpn;
	suspend_req->data_udp_start = tp->data_udp_start;
	suspend_req->ack_udp_start = tp->ack_udp_start;
	suspend_req->sip_idx = tp->local_net_addr_idx;
	ubcore_log_info(
		"report tp suspend: data_udp_start: %u, ack_udp_start: %u",
		tp->data_udp_start, tp->ack_udp_start);
	(void)memcpy(suspend_req->mue_dev_name, dev->dev_name,
		     UBCORE_MAX_DEV_NAME);

	return req;
}

void ubcore_report_tp_suspend(struct ubcore_device *dev, struct ubcore_tp *tp)
{
	struct ubcore_uvs_instance *uvs = NULL;
	struct ubcore_nlmsg *req_msg;
	int ret;

	req_msg = ubcore_get_tp_suspend_req(dev, tp);
	if (req_msg == NULL) {
		ubcore_log_err("Failed to get tp suspend req");
		return;
	}

	uvs = ubcore_find_get_uvs_by_ue(dev, tp->ue_idx);
	if (uvs == NULL) {
		ubcore_log_err("Failed to find uvs for ue %u", tp->ue_idx);
		kfree(req_msg);
		return;
	}

	ret = ubcore_nl_send_nowait_without_cb(req_msg, uvs);
	if (ret)
		ubcore_log_err("Failed to nowait send tp suspend request");
	else
		ubcore_log_info("Success to nowait send tp suspend request");

	ubcore_uvs_kref_put(uvs);
	kfree(req_msg);
}

void ubcore_put_ta_jetty(struct ubcore_ta *ta)
{
	if (ta->type == UBCORE_TA_JFS_TJFR)
		ubcore_put_jfs(ta->jfs);
	else if (ta->type == UBCORE_TA_JETTY_TJETTY)
		ubcore_put_jetty(ta->jetty);
}

void ubcore_put_target_ta_jetty(struct ubcore_ta *ta)
{
	if (ta->type == UBCORE_TA_JFS_TJFR)
		ubcore_put_jfr(ta->jfr);
	else if (ta->type == UBCORE_TA_JETTY_TJETTY)
		ubcore_put_jetty(ta->jetty);
}

/* restore target RM tp created by ubcore_advise_target_tp */
static int
ubcore_restore_advised_target_tp(struct ubcore_device *dev,
				 struct ubcore_nl_restore_tp_req *restore,
				 uint32_t *rx_psn)
{
	struct ubcore_tp_meta meta = { 0 };
	struct ubcore_tp_node *tp_node;
	struct ubcore_tp *tp;

	if (ubcore_parse_ta(dev, &restore->ta, &meta) != 0) {
		ubcore_log_err("Failed to parse ta with type %u",
			       (uint32_t)restore->ta.ta_type);
		return -1;
	} else if (meta.ht == NULL) {
		ubcore_log_info("tp table is already released");
		return -1;
	}

	spin_lock(&meta.ht->lock);
	tp_node =
		ubcore_hash_table_lookup_nolock(meta.ht, meta.hash, &meta.key);
	/* pair with get_tptable in parse_ta */
	ubcore_put_tptable(meta.ht);
	if (tp_node == NULL) {
		spin_unlock(&meta.ht->lock);
		ubcore_log_err("tp is not found%u", restore->peer_tpn);
		return -1;
	}

	tp = tp_node->tp;
	if (ubcore_restore_tp_to_rts(dev, tp, get_random_u32(),
				     restore->rx_psn) != 0) {
		spin_unlock(&meta.ht->lock);
		ubcore_log_err("Failed to modify tp to rts %u",
			       restore->rx_psn);
		return -1;
	}
	*rx_psn = tp->rx_psn;
	spin_unlock(&meta.ht->lock);
	return 0;
}

static int
ubcore_restore_bound_target_tp(struct ubcore_device *dev,
			       struct ubcore_nl_restore_tp_req *restore,
			       uint32_t *rx_psn)
{
	return ubcore_restore_advised_target_tp(dev, restore, rx_psn);
}

static int ubcore_handle_restore_tp(struct ubcore_device *dev,
				    struct ubcore_nl_restore_tp_req *restore,
				    uint32_t *rx_psn)
{
	if (restore == NULL ||
	    restore->trans_mode == UBCORE_TP_UM ||
	    restore->ta.ta_type == UBCORE_TA_NONE ||
	    restore->ta.ta_type >= UBCORE_TA_VIRT)
		return -1;

	if (restore->trans_mode == UBCORE_TP_RM)
		return ubcore_restore_advised_target_tp(dev, restore, rx_psn);
	else
		return ubcore_restore_bound_target_tp(dev, restore, rx_psn);
}

struct ubcore_nlmsg *ubcore_handle_restore_tp_req(struct ubcore_nlmsg *req)
{
	enum ubcore_nl_resp_status status = UBCORE_NL_RESP_SUCCESS;
	struct ubcore_nl_restore_tp_req *restore;
	struct ubcore_device *dev;
	uint32_t rx_psn = 0;
	int ret = 0;

	if (req == NULL ||
	    req->payload_len != sizeof(struct ubcore_nl_restore_tp_req)) {
		ubcore_log_err("Invalid restore req");
		return NULL;
	}

	restore = (struct ubcore_nl_restore_tp_req *)(void *)req->payload;
	dev = ubcore_find_device(&req->dst_eid, req->transport_type);
	if (!ubcore_have_tp_ops(dev)) {
		if (dev != NULL)
			ubcore_put_device(dev);
		ubcore_log_err("Failed to find device or device ops invalid");
		return ubcore_get_restore_tp_response(req, rx_psn,
						      UBCORE_NL_RESP_FAIL);
	}

	ret = ubcore_handle_restore_tp(dev, restore, &rx_psn);
	if (ret != 0) {
		ubcore_log_err(
			"Failed to restore target tp towards remote eid %pI6c",
			&req->src_eid);
		status = UBCORE_NL_RESP_FAIL;
	}

	ubcore_put_device(dev);
	return ubcore_get_restore_tp_response(req, rx_psn, status);
}
EXPORT_SYMBOL(ubcore_handle_restore_tp_req);

int ubcore_get_tp_list(struct ubcore_device *dev, struct ubcore_get_tp_cfg *cfg,
		       uint32_t *tp_cnt, struct ubcore_tp_info *tp_list,
		       struct ubcore_udata *udata)
{
	int ret;

	if (dev == NULL || dev->ops == NULL || dev->ops->get_tp_list == NULL ||
	    cfg == NULL || tp_cnt == NULL || tp_list == NULL || *tp_cnt == 0) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	if (ubcore_check_trans_mode_valid(cfg->trans_mode) != true) {
		ubcore_log_err("Invalid parameter, trans_mode: %d.\n",
			       (int)cfg->trans_mode);
		return -EINVAL;
	}

	ret = dev->ops->get_tp_list(dev, cfg, tp_cnt, tp_list, udata);
	if (ret != 0)
		ubcore_log_err("Failed to get to list, ret: %d.\n", ret);

	return ret;
}
EXPORT_SYMBOL(ubcore_get_tp_list);

int ubcore_set_tp_attr(struct ubcore_device *dev, const uint64_t tp_handle,
		       const uint8_t tp_attr_cnt, const uint32_t tp_attr_bitmap,
		       const struct ubcore_tp_attr_value *tp_attr,
		       struct ubcore_udata *udata)
{
	int ret;

	if (dev == NULL || dev->ops == NULL || dev->ops->set_tp_attr == NULL ||
	    tp_attr == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	ret = dev->ops->set_tp_attr(dev, tp_handle, tp_attr_cnt, tp_attr_bitmap,
				    tp_attr, udata);
	if (ret != 0)
		ubcore_log_err("Failed to set tp attributions, ret: %d.\n",
			       ret);

	return ret;
}
EXPORT_SYMBOL(ubcore_set_tp_attr);

int ubcore_get_tp_attr(struct ubcore_device *dev, const uint64_t tp_handle,
		       uint8_t *tp_attr_cnt, uint32_t *tp_attr_bitmap,
		       struct ubcore_tp_attr_value *tp_attr,
		       struct ubcore_udata *udata)
{
	int ret;

	if (dev == NULL || dev->ops == NULL || dev->ops->get_tp_attr == NULL ||
	    tp_attr_cnt == NULL || tp_attr_bitmap == NULL || tp_attr == NULL) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}

	ret = dev->ops->get_tp_attr(dev, tp_handle, tp_attr_cnt, tp_attr_bitmap,
				    tp_attr, udata);
	if (ret != 0)
		ubcore_log_err("Failed to get tp attributions, ret: %d.\n",
			       ret);

	return ret;
}
EXPORT_SYMBOL(ubcore_get_tp_attr);
