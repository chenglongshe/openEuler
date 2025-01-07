// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/kernel.h>

#include "yt6801_desc.h"
#include "yt6801_net.h"

struct fxgmac_stats_desc {
	char stat_string[ETH_GSTRING_LEN];
	int stat_offset;
};

#define FXGMAC_STAT(str, var)                                                  \
	{                                                                      \
		str, offsetof(struct fxgmac_pdata, stats.var),                 \
	}

static const struct fxgmac_stats_desc fxgmac_gstring_stats[] = {
	/* MMC TX counters */
	FXGMAC_STAT("tx_bytes", txoctetcount_gb),
	FXGMAC_STAT("tx_bytes_good", txoctetcount_g),
	FXGMAC_STAT("tx_packets", txframecount_gb),
	FXGMAC_STAT("tx_packets_good", txframecount_g),
	FXGMAC_STAT("tx_unicast_packets", txunicastframes_gb),
	FXGMAC_STAT("tx_broadcast_packets", txbroadcastframes_gb),
	FXGMAC_STAT("tx_broadcast_packets_good", txbroadcastframes_g),
	FXGMAC_STAT("tx_multicast_packets", txmulticastframes_gb),
	FXGMAC_STAT("tx_multicast_packets_good", txmulticastframes_g),
	FXGMAC_STAT("tx_vlan_packets_good", txvlanframes_g),
	FXGMAC_STAT("tx_64_byte_packets", tx64octets_gb),
	FXGMAC_STAT("tx_65_to_127_byte_packets", tx65to127octets_gb),
	FXGMAC_STAT("tx_128_to_255_byte_packets", tx128to255octets_gb),
	FXGMAC_STAT("tx_256_to_511_byte_packets", tx256to511octets_gb),
	FXGMAC_STAT("tx_512_to_1023_byte_packets", tx512to1023octets_gb),
	FXGMAC_STAT("tx_1024_to_max_byte_packets", tx1024tomaxoctets_gb),
	FXGMAC_STAT("tx_underflow_errors", txunderflowerror),
	FXGMAC_STAT("tx_pause_frames", txpauseframes),
	FXGMAC_STAT("tx_single_collision", txsinglecollision_g),
	FXGMAC_STAT("tx_multiple_collision", txmultiplecollision_g),
	FXGMAC_STAT("tx_deferred_frames", txdeferredframes),
	FXGMAC_STAT("tx_late_collision_frames", txlatecollisionframes),
	FXGMAC_STAT("tx_excessive_collision_frames",
		    txexcessivecollisionframes),
	FXGMAC_STAT("tx_carrier_error_frames", txcarriererrorframes),
	FXGMAC_STAT("tx_excessive_deferral_error", txexcessivedeferralerror),
	FXGMAC_STAT("tx_oversize_frames_good", txoversize_g),

	/* MMC RX counters */
	FXGMAC_STAT("rx_bytes", rxoctetcount_gb),
	FXGMAC_STAT("rx_bytes_good", rxoctetcount_g),
	FXGMAC_STAT("rx_packets", rxframecount_gb),
	FXGMAC_STAT("rx_unicast_packets_good", rxunicastframes_g),
	FXGMAC_STAT("rx_broadcast_packets_good", rxbroadcastframes_g),
	FXGMAC_STAT("rx_multicast_packets_good", rxmulticastframes_g),
	FXGMAC_STAT("rx_vlan_packets_mac", rxvlanframes_gb),
	FXGMAC_STAT("rx_64_byte_packets", rx64octets_gb),
	FXGMAC_STAT("rx_65_to_127_byte_packets", rx65to127octets_gb),
	FXGMAC_STAT("rx_128_to_255_byte_packets", rx128to255octets_gb),
	FXGMAC_STAT("rx_256_to_511_byte_packets", rx256to511octets_gb),
	FXGMAC_STAT("rx_512_to_1023_byte_packets", rx512to1023octets_gb),
	FXGMAC_STAT("rx_1024_to_max_byte_packets", rx1024tomaxoctets_gb),
	FXGMAC_STAT("rx_undersize_packets_good", rxundersize_g),
	FXGMAC_STAT("rx_oversize_packets_good", rxoversize_g),
	FXGMAC_STAT("rx_crc_errors", rxcrcerror),
	FXGMAC_STAT("rx_align_error", rxalignerror),
	FXGMAC_STAT("rx_crc_errors_small_packets", rxrunterror),
	FXGMAC_STAT("rx_crc_errors_giant_packets", rxjabbererror),
	FXGMAC_STAT("rx_length_errors", rxlengtherror),
	FXGMAC_STAT("rx_out_of_range_errors", rxoutofrangetype),
	FXGMAC_STAT("rx_fifo_overflow_errors", rxfifooverflow),
	FXGMAC_STAT("rx_watchdog_errors", rxwatchdogerror),
	FXGMAC_STAT("rx_pause_frames", rxpauseframes),
	FXGMAC_STAT("rx_receive_error_frames", rxreceiveerrorframe),
	FXGMAC_STAT("rx_control_frames_good", rxcontrolframe_g),

	/* Extra counters */
	FXGMAC_STAT("tx_tso_packets", tx_tso_packets),
	FXGMAC_STAT("rx_split_header_packets", rx_split_header_packets),
	FXGMAC_STAT("tx_process_stopped", tx_process_stopped),
	FXGMAC_STAT("rx_process_stopped", rx_process_stopped),
	FXGMAC_STAT("tx_buffer_unavailable", tx_buffer_unavailable),
	FXGMAC_STAT("rx_buffer_unavailable", rx_buffer_unavailable),
	FXGMAC_STAT("fatal_bus_error", fatal_bus_error),
	FXGMAC_STAT("tx_vlan_packets_net", tx_vlan_packets),
	FXGMAC_STAT("rx_vlan_packets_net", rx_vlan_packets),
	FXGMAC_STAT("napi_poll_isr", napi_poll_isr),
	FXGMAC_STAT("napi_poll_txtimer", napi_poll_txtimer),
	FXGMAC_STAT("alive_cnt_txtimer", cnt_alive_txtimer),
	FXGMAC_STAT("ephy_poll_timer", ephy_poll_timer_cnt),
	FXGMAC_STAT("mgmt_int_isr", mgmt_int_isr),
};

#define FXGMAC_STATS_COUNT ARRAY_SIZE(fxgmac_gstring_stats)

static void fxgmac_ethtool_get_drvinfo(struct net_device *netdev,
				       struct ethtool_drvinfo *drvinfo)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	u32 ver = pdata->hw_feat.version;
	u32 sver, devid, userver;

	strscpy(drvinfo->version, pdata->drv_ver, sizeof(drvinfo->version));

	/* S|SVER: MAC Version
	 * D|DEVID: Indicates the Device family
	 * U|USERVER: User-defined Version
	 */
	sver = FXGMAC_GET_BITS(ver, MAC_VR_SVER_POS, MAC_VR_SVER_LEN);
	devid = FXGMAC_GET_BITS(ver, MAC_VR_DEVID_POS, MAC_VR_DEVID_LEN);
	userver = FXGMAC_GET_BITS(ver, MAC_VR_USERVER_POS, MAC_VR_USERVER_LEN);

	snprintf(drvinfo->fw_version, sizeof(drvinfo->fw_version),
		 "S.D.U: %x.%x.%x", sver, devid, userver);
}

static u32 fxgmac_ethtool_get_msglevel(struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	return pdata->msg_enable;
}

static void fxgmac_ethtool_set_msglevel(struct net_device *netdev, u32 msglevel)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	yt_dbg(pdata, "set msglvl from %08x to %08x\n", pdata->msg_enable,
	       msglevel);
	pdata->msg_enable = msglevel;
}

static void fxgmac_ethtool_get_channels(struct net_device *netdev,
					struct ethtool_channels *channel)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	/* report maximum channels */
	channel->max_rx = FXGMAC_MAX_DMA_RX_CHANNELS;
	channel->max_tx = FXGMAC_MAX_DMA_TX_CHANNELS;
	channel->max_other = 0;
	channel->max_combined =
		channel->max_rx + channel->max_tx + channel->max_other;

	yt_dbg(pdata, "channels max rx:%d, tx:%d, other:%d, combined:%d\n",
	       channel->max_rx, channel->max_tx, channel->max_other,
	       channel->max_combined);

	channel->rx_count = pdata->rx_q_count;
	channel->tx_count = FXGMAC_TX_1_Q;
	channel->other_count = 0;
	/* record RSS queues */
	channel->combined_count =
		channel->rx_count + channel->tx_count + channel->other_count;

	yt_dbg(pdata, "channels count rx:%d, tx:%d, other:%d, combined:%d\n",
	       channel->rx_count, channel->tx_count, channel->other_count,
	       channel->combined_count);
}

static int
fxgmac_ethtool_get_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)

{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	memset(ec, 0, sizeof(struct ethtool_coalesce));
	ec->rx_coalesce_usecs = pdata->rx_usecs;
	ec->tx_coalesce_usecs = pdata->tx_usecs;

	return 0;
}

#define FXGMAC_MAX_DMA_RIWT 0xff
#define FXGMAC_MIN_DMA_RIWT 0x01

static int
fxgmac_ethtool_set_coalesce(struct net_device *netdev,
			    struct ethtool_coalesce *ec,
			    struct kernel_ethtool_coalesce *kernel_coal,
			    struct netlink_ext_ack *extack)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	unsigned int rx_frames, rx_riwt, rx_usecs;
	unsigned int tx_frames;

	/* Check for not supported parameters */
	if (ec->rx_coalesce_usecs_irq || ec->rx_max_coalesced_frames_irq ||
	    ec->tx_coalesce_usecs_high || ec->tx_max_coalesced_frames_irq ||
	    ec->tx_coalesce_usecs_irq || ec->stats_block_coalesce_usecs ||
	    ec->pkt_rate_low || ec->use_adaptive_rx_coalesce ||
	    ec->use_adaptive_tx_coalesce || ec->rx_max_coalesced_frames_low ||
	    ec->rx_coalesce_usecs_low || ec->tx_coalesce_usecs_low ||
	    ec->tx_max_coalesced_frames_low || ec->pkt_rate_high ||
	    ec->rx_coalesce_usecs_high || ec->rx_max_coalesced_frames_high ||
	    ec->tx_max_coalesced_frames_high || ec->rate_sample_interval)
		return -EOPNOTSUPP;

	rx_usecs = ec->rx_coalesce_usecs;
	rx_riwt = hw_ops->usec_to_riwt(pdata, rx_usecs);
	rx_frames = ec->rx_max_coalesced_frames;
	tx_frames = ec->tx_max_coalesced_frames;

	if (rx_riwt > FXGMAC_MAX_DMA_RIWT || rx_riwt < FXGMAC_MIN_DMA_RIWT ||
	    rx_frames > pdata->rx_desc_count)
		return -EINVAL;

	if (tx_frames > pdata->tx_desc_count)
		return -EINVAL;

	pdata->rx_usecs = rx_usecs;
	pdata->rx_frames = rx_frames;
	pdata->rx_riwt = rx_riwt;
	hw_ops->config_rx_coalesce(pdata);

	pdata->tx_frames = tx_frames;
	pdata->tx_usecs = ec->tx_coalesce_usecs;
	hw_ops->set_interrupt_moderation(pdata);

	return 0;
}

static u32 fxgmac_get_rxfh_key_size(struct net_device *netdev)
{
	return FXGMAC_RSS_HASH_KEY_SIZE;
}

static u32 fxgmac_rss_indir_size(struct net_device *netdev)
{
	return FXGMAC_RSS_MAX_TABLE_SIZE;
}

static int fxgmac_get_rss_hash_opts(struct fxgmac_pdata *pdata,
				    struct ethtool_rxnfc *cmd)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	u32 reg_opt;

	cmd->data = 0;
	reg_opt = hw_ops->get_rss_options(pdata);
	yt_dbg(pdata, "%s, hw=%02x, %02x\n", __func__, reg_opt,
	       pdata->rss_options);

	if (reg_opt != pdata->rss_options)
		yt_dbg(pdata, "warning, options are not consistent\n");

	/* Report default options for RSS */
	switch (cmd->flow_type) {
	case TCP_V4_FLOW:
	case UDP_V4_FLOW:
		if ((cmd->flow_type == TCP_V4_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_TCP4TE)) ||
		    (cmd->flow_type == UDP_V4_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_UDP4TE))) {
			cmd->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
		}
		fallthrough;
	case SCTP_V4_FLOW:
	case AH_ESP_V4_FLOW:
	case AH_V4_FLOW:
	case ESP_V4_FLOW:
	case IPV4_FLOW:
		if ((cmd->flow_type == TCP_V4_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_TCP4TE)) ||
		    (cmd->flow_type == UDP_V4_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_UDP4TE)) ||
		    (pdata->rss_options & BIT(FXGMAC_RSS_IP4TE))) {
			cmd->data |= RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	case TCP_V6_FLOW:
	case UDP_V6_FLOW:
		if ((cmd->flow_type == TCP_V6_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_TCP6TE)) ||
		    (cmd->flow_type == UDP_V6_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_UDP6TE))) {
			cmd->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
		}
		fallthrough;
	case SCTP_V6_FLOW:
	case AH_ESP_V6_FLOW:
	case AH_V6_FLOW:
	case ESP_V6_FLOW:
	case IPV6_FLOW:
		if ((cmd->flow_type == TCP_V6_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_TCP6TE)) ||
		    (cmd->flow_type == UDP_V6_FLOW &&
		     (pdata->rss_options & FXGMAC_RSS_UDP6TE)) ||
		    (pdata->rss_options & FXGMAC_RSS_IP6TE)) {
			cmd->data |= RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fxgmac_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd,
			    u32 *rule_locs)
{
	struct fxgmac_pdata *pdata = netdev_priv(dev);
	int ret = 0;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = pdata->rx_q_count;
		yt_dbg(pdata, "%s, rx ring cnt\n", __func__);
		break;
	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = 0;
		yt_dbg(pdata, "%s, classify rule cnt\n", __func__);
		break;
	case ETHTOOL_GRXCLSRULE:
		yt_dbg(pdata, "%s, classify rules\n", __func__);
		break;
	case ETHTOOL_GRXCLSRLALL:
		cmd->rule_cnt = 0;
		yt_dbg(pdata, "%s, classify both cnt and rules\n", __func__);
		break;
	case ETHTOOL_GRXFH:
		ret = fxgmac_get_rss_hash_opts(pdata, cmd);
		yt_dbg(pdata, "%s, hash options\n", __func__);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}

	return ret;
}

#define UDP_RSS_FLAGS (FXGMAC_RSS_UDP4TE | FXGMAC_RSS_UDP6TE)

static int fxgmac_set_rss_hash_opt(struct fxgmac_pdata *pdata,
				   struct ethtool_rxnfc *nfc)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	__u64 data = nfc->data;
	u32 rssopt = 0;

	yt_dbg(pdata, "%s nfc_data=%llx,cur opt=%x\n", __func__, data,
	       pdata->rss_options);

	/* For RSS, it does not support anything other than hashing
	 * to queues on src,dst IPs and L4 ports
	 */
	if (data & ~(RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3))
		return -EINVAL;

	switch (nfc->flow_type) {
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
		/* default to TCP flow and do nothting */
		if (!(data &
		      (RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3)))
			return -EINVAL;
		if (nfc->flow_type == TCP_V4_FLOW)
			rssopt |= BIT(FXGMAC_RSS_IP4TE) | FXGMAC_RSS_TCP4TE;
		if (nfc->flow_type == TCP_V6_FLOW)
			rssopt |= FXGMAC_RSS_IP6TE | FXGMAC_RSS_TCP6TE;
		break;
	case UDP_V4_FLOW:
		if (!(data & (RXH_IP_SRC | RXH_IP_DST)))
			return -EINVAL;
		rssopt |= BIT(FXGMAC_RSS_IP4TE);
		switch (data & (RXH_L4_B_0_1 | RXH_L4_B_2_3)) {
		case 0:
			break;
		case (RXH_L4_B_0_1 | RXH_L4_B_2_3):
			rssopt |= FXGMAC_RSS_UDP4TE;
			break;
		default:
			return -EINVAL;
		}
		break;
	case UDP_V6_FLOW:
		if (!(data & (RXH_IP_SRC | RXH_IP_DST)))
			return -EINVAL;
		rssopt |= FXGMAC_RSS_IP6TE;

		switch (data & (RXH_L4_B_0_1 | RXH_L4_B_2_3)) {
		case 0:
			break;
		case (RXH_L4_B_0_1 | RXH_L4_B_2_3):
			rssopt |= FXGMAC_RSS_UDP6TE;
			break;
		default:
			return -EINVAL;
		}
		break;
	case AH_ESP_V4_FLOW:
	case AH_V4_FLOW:
	case ESP_V4_FLOW:
	case SCTP_V4_FLOW:
	case AH_ESP_V6_FLOW:
	case AH_V6_FLOW:
	case ESP_V6_FLOW:
	case SCTP_V6_FLOW:
		if (!(data &
		      (RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3)))
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	/* if options are changed, then update to hw */
	if (rssopt != pdata->rss_options) {
		if ((rssopt & UDP_RSS_FLAGS) &&
		    !(pdata->rss_options & UDP_RSS_FLAGS))
			yt_dbg(pdata,
			       "enabling UDP RSS: fragmented packets may arrive out of order to the stack above.");

		yt_dbg(pdata, "rss option changed from %x to %x\n",
		       pdata->rss_options, rssopt);
		pdata->rss_options = rssopt;
		hw_ops->set_rss_options(pdata);
	}

	return 0;
}

static int fxgmac_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *cmd)
{
	struct fxgmac_pdata *pdata = netdev_priv(dev);
	int ret = -EOPNOTSUPP;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		yt_dbg(pdata, "%s, rx cls rule insert-n\\a\n", __func__);
		break;
	case ETHTOOL_SRXCLSRLDEL:
		yt_dbg(pdata, "%s, rx cls rule del-n\\a\n", __func__);
		break;
	case ETHTOOL_SRXFH:
		yt_dbg(pdata, "%s, rx rss option\n", __func__);
		ret = fxgmac_set_rss_hash_opt(pdata, cmd);
		break;
	default:
		break;
	}

	return ret;
}

static void fxgmac_get_ringparam(struct net_device *netdev,
				 struct ethtool_ringparam *ring,
				 struct kernel_ethtool_ringparam *kernel_ring,
				 struct netlink_ext_ack *exact)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	ring->rx_max_pending = FXGMAC_RX_DESC_CNT;
	ring->tx_max_pending = FXGMAC_TX_DESC_CNT;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = pdata->rx_desc_count;
	ring->tx_pending = pdata->tx_desc_count;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int fxgmac_set_ringparam(struct net_device *netdev,
				struct ethtool_ringparam *ring,
				struct kernel_ethtool_ringparam *kernel_ring,
				struct netlink_ext_ack *exact)

{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	int ret;

	if (pdata->dev_state != FXGMAC_DEV_START)
		return 0;

	mutex_lock(&pdata->mutex);
	pdata->tx_desc_count = ring->tx_pending;
	pdata->rx_desc_count = ring->rx_pending;

	fxgmac_stop(pdata);
	fxgmac_free_tx_data(pdata);
	fxgmac_free_rx_data(pdata);
	ret = fxgmac_channels_rings_alloc(pdata);
	if (ret < 0)
		goto unlock;

	ret = fxgmac_start(pdata);

unlock:
	mutex_unlock(&pdata->mutex);
	return ret;
}

static void fxgmac_get_wol(struct net_device *netdev,
			   struct ethtool_wolinfo *wol)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	wol->supported = WAKE_UCAST | WAKE_MCAST | WAKE_BCAST | WAKE_MAGIC |
			 WAKE_ARP | WAKE_PHY;

	wol->wolopts = 0;
	if (!(pdata->hw_feat.rwk) || !device_can_wakeup(pdata->dev)) {
		yt_err(pdata, "%s, pci does not support wakeup.\n", __func__);
		return;
	}

	wol->wolopts = pdata->wol;
}

#pragma pack(1)
/* it's better to make this struct's size to 128byte. */
struct pattern_packet {
	u8 ether_daddr[ETH_ALEN];
	u8 ether_saddr[ETH_ALEN];
	u16 ether_type;

	__be16 ar_hrd;		/* format of hardware address  */
	__be16 ar_pro;		/* format of protocol          */
	u8 ar_hln;		/* length of hardware address  */
	u8 ar_pln;		/* length of protocol address  */
	__be16 ar_op;		/* ARP opcode (command)        */
	u8 ar_sha[ETH_ALEN];	/* sender hardware address     */
	u8 ar_sip[4];		/* sender IP address           */
	u8 ar_tha[ETH_ALEN];	/* target hardware address     */
	u8 ar_tip[4];		/* target IP address           */

	u8 reverse[86];
};

#pragma pack()

static int fxgmac_set_pattern_data(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	u8 type_offset, tip_offset, op_offset;
	struct wol_bitmap_pattern pattern[4];
	struct pattern_packet packet;
	u32 ip_addr, i = 0;

	memset(pattern, 0, sizeof(struct wol_bitmap_pattern) * 4);

	/* Config ucast */
	if (pdata->wol & WAKE_UCAST) {
		pattern[i].mask_info[0] = 0x3F;
		pattern[i].mask_size = sizeof(pattern[0].mask_info);
		memcpy(pattern[i].pattern_info, pdata->mac_addr, ETH_ALEN);
		pattern[i].pattern_offset = 0;
		i++;
	}

	/* Config bcast */
	if (pdata->wol & WAKE_BCAST) {
		pattern[i].mask_info[0] = 0x3F;
		pattern[i].mask_size = sizeof(pattern[0].mask_info);
		memset(pattern[i].pattern_info, 0xFF, ETH_ALEN);
		pattern[i].pattern_offset = 0;
		i++;
	}

	/* Config mcast */
	if (pdata->wol & WAKE_MCAST) {
		pattern[i].mask_info[0] = 0x7;
		pattern[i].mask_size = sizeof(pattern[0].mask_info);
		pattern[i].pattern_info[0] = 0x1;
		pattern[i].pattern_info[1] = 0x0;
		pattern[i].pattern_info[2] = 0x5E;
		pattern[i].pattern_offset = 0;
		i++;
	}

	/* Config arp */
	if (pdata->wol & WAKE_ARP) {
		memset(pattern[i].mask_info, 0, sizeof(pattern[0].mask_info));
		type_offset = offsetof(struct pattern_packet, ar_pro);
		pattern[i].mask_info[type_offset / 8] |= 1 << type_offset % 8;
		type_offset++;
		pattern[i].mask_info[type_offset / 8] |= 1 << type_offset % 8;
		op_offset = offsetof(struct pattern_packet, ar_op);
		pattern[i].mask_info[op_offset / 8] |= 1 << op_offset % 8;
		op_offset++;
		pattern[i].mask_info[op_offset / 8] |= 1 << op_offset % 8;
		tip_offset = offsetof(struct pattern_packet, ar_tip);
		pattern[i].mask_info[tip_offset / 8] |= 1 << tip_offset % 8;
		tip_offset++;
		pattern[i].mask_info[tip_offset / 8] |= 1 << type_offset % 8;
		tip_offset++;
		pattern[i].mask_info[tip_offset / 8] |= 1 << type_offset % 8;
		tip_offset++;
		pattern[i].mask_info[tip_offset / 8] |= 1 << type_offset % 8;

		packet.ar_pro = 0x0 << 8 | 0x08;
		/* ARP type is 0x0800, notice that ar_pro and ar_op is
		 * big endian
		 */
		packet.ar_op = 0x1 << 8;
		/* 1 is arp request,2 is arp replay, 3 is rarp request,
		 * 4 is rarp replay
		 */
		ip_addr = fxgmac_get_netdev_ip4addr(pdata);
		packet.ar_tip[0] = ip_addr & 0xFF;
		packet.ar_tip[1] = (ip_addr >> 8) & 0xFF;
		packet.ar_tip[2] = (ip_addr >> 16) & 0xFF;
		packet.ar_tip[3] = (ip_addr >> 24) & 0xFF;
		memcpy(pattern[i].pattern_info, &packet, MAX_PATTERN_SIZE);
		pattern[i].mask_size = sizeof(pattern[0].mask_info);
		pattern[i].pattern_offset = 0;
		i++;
	}

	return hw_ops->set_wake_pattern(pdata, pattern, i);
}

static int fxgmac_set_wol(struct net_device *netdev,
			  struct ethtool_wolinfo *wol)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	int ret;

	if (wol->wolopts & (WAKE_MAGICSECURE | WAKE_FILTER)) {
		yt_err(pdata, "%s, not supported wol options, 0x%x\n", __func__,
		       wol->wolopts);
		return -EOPNOTSUPP;
	}

	if (!(pdata->hw_feat.rwk)) {
		yt_err(pdata, "%s, hw wol feature is n/a\n", __func__);
		return wol->wolopts ? -EOPNOTSUPP : 0;
	}

	pdata->wol = 0;
	if (wol->wolopts & WAKE_UCAST)
		pdata->wol |= WAKE_UCAST;

	if (wol->wolopts & WAKE_MCAST)
		pdata->wol |= WAKE_MCAST;

	if (wol->wolopts & WAKE_BCAST)
		pdata->wol |= WAKE_BCAST;

	if (wol->wolopts & WAKE_MAGIC)
		pdata->wol |= WAKE_MAGIC;

	if (wol->wolopts & WAKE_PHY)
		pdata->wol |= WAKE_PHY;

	if (wol->wolopts & WAKE_ARP)
		pdata->wol |= WAKE_ARP;

	ret = fxgmac_set_pattern_data(pdata);
	if (ret < 0)
		return ret;

	ret = fxgmac_config_wol(pdata, (!!(pdata->wol)));
	yt_dbg(pdata, "%s, opt=0x%x, wol=0x%x\n", __func__, wol->wolopts,
	       pdata->wol);

	return ret;
}

static int fxgmac_get_regs_len(struct net_device __always_unused *netdev)
{
	return FXGMAC_PHY_REG_CNT * sizeof(u32);
}

static void fxgmac_get_regs(struct net_device *netdev,
			    struct ethtool_regs *regs, void *p)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	u32 *regs_buff = p;

	memset(p, 0, FXGMAC_PHY_REG_CNT * sizeof(u32));
	for (u32 i = MII_BMCR; i < FXGMAC_PHY_REG_CNT; i++)
		regs_buff[i] = phy_read(pdata->phydev, i);

	regs->version = regs_buff[MII_PHYSID1] << 16 | regs_buff[MII_PHYSID2];
}

static void fxgmac_get_pauseparam(struct net_device *dev,
				  struct ethtool_pauseparam *data)
{
	struct fxgmac_pdata *yt = netdev_priv(dev);
	bool tx_pause, rx_pause;

	phy_get_pause(yt->phydev, &tx_pause, &rx_pause);

	data->autoneg = yt->phydev->autoneg;
	data->tx_pause = tx_pause ? 1 : 0;
	data->rx_pause = rx_pause ? 1 : 0;

	yt_dbg(yt, "%s, rx=%d, tx=%d\n", __func__, data->rx_pause,
	       data->tx_pause);
}

static int fxgmac_set_pauseparam(struct net_device *dev,
				 struct ethtool_pauseparam *data)
{
	struct fxgmac_pdata *yt = netdev_priv(dev);

	if (dev->mtu > ETH_DATA_LEN)
		return -EOPNOTSUPP;

	phy_set_asym_pause(yt->phydev, data->rx_pause, data->tx_pause);

	yt_dbg(yt, "%s, autoneg=%d, rx=%d, tx=%d\n", __func__,
	       data->autoneg, data->rx_pause, data->tx_pause);

	return 0;
}

static void fxgmac_ethtool_get_strings(struct net_device *netdev, u32 stringset,
				       u8 *data)
{
	switch (stringset) {
	case ETH_SS_STATS:
		for (u32 i = 0; i < FXGMAC_STATS_COUNT; i++) {
			memcpy(data, fxgmac_gstring_stats[i].stat_string,
			       strlen(fxgmac_gstring_stats[i].stat_string));
			data += ETH_GSTRING_LEN;
		}
		break;
	default:
		WARN_ON(1);
		break;
	}
}

static int fxgmac_ethtool_get_sset_count(struct net_device *netdev,
					 int stringset)
{
	int ret;

	switch (stringset) {
	case ETH_SS_STATS:
		ret = FXGMAC_STATS_COUNT;
		break;

	default:
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static void fxgmac_ethtool_get_ethtool_stats(struct net_device *netdev,
					     struct ethtool_stats *stats,
					     u64 *data)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	if (!test_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate))
		pdata->hw_ops.read_mmc_stats(pdata);

	for (u32 i = 0; i < FXGMAC_STATS_COUNT; i++)
		*data++ = *(u64 *)((u8 *)pdata +
				   fxgmac_gstring_stats[i].stat_offset);
}

static int fxgmac_ethtool_reset(struct net_device *netdev, u32 *flag)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	int ret = 0;
	u32 val;

	val = *flag & (ETH_RESET_ALL | ETH_RESET_PHY);
	if (!val) {
		yt_err(pdata, "Operation not support.\n");
		return -EOPNOTSUPP;
	}

	switch (*flag) {
	case ETH_RESET_ALL:
		fxgmac_restart(pdata);
		*flag = 0;
		break;
	case ETH_RESET_PHY:
		/* Power off and on the phy in order to properly
		 * configure the MAC timing
		 */
		ret = phy_modify(pdata->phydev, MII_BMCR, BMCR_PDOWN,
				 BMCR_PDOWN);
		if (ret < 0)
			return ret;
		fsleep(9000);

		ret = phy_modify(pdata->phydev, MII_BMCR, BMCR_PDOWN, 0);
		if (ret < 0)
			return ret;
		*flag = 0;
		break;
	default:
		break;
	}

	return 0;
}

static const struct ethtool_ops fxgmac_ethtool_ops = {
	.get_drvinfo			= fxgmac_ethtool_get_drvinfo,
	.get_link			= ethtool_op_get_link,
	.get_msglevel			= fxgmac_ethtool_get_msglevel,
	.set_msglevel			= fxgmac_ethtool_set_msglevel,
	.get_channels			= fxgmac_ethtool_get_channels,
	.get_coalesce			= fxgmac_ethtool_get_coalesce,
	.set_coalesce			= fxgmac_ethtool_set_coalesce,
	.reset				= fxgmac_ethtool_reset,
	.supported_coalesce_params	= ETHTOOL_COALESCE_USECS,
	.get_strings			= fxgmac_ethtool_get_strings,
	.get_sset_count			= fxgmac_ethtool_get_sset_count,
	.get_ethtool_stats		= fxgmac_ethtool_get_ethtool_stats,
	.get_regs_len			= fxgmac_get_regs_len,
	.get_regs			= fxgmac_get_regs,
	.get_ringparam			= fxgmac_get_ringparam,
	.set_ringparam			= fxgmac_set_ringparam,
	.get_rxnfc			= fxgmac_get_rxnfc,
	.set_rxnfc			= fxgmac_set_rxnfc,
	.get_rxfh_indir_size		= fxgmac_rss_indir_size,
	.get_rxfh_key_size		= fxgmac_get_rxfh_key_size,
	.get_wol			= fxgmac_get_wol,
	.set_wol			= fxgmac_set_wol,
	.nway_reset			= phy_ethtool_nway_reset,
	.get_link_ksettings		= phy_ethtool_get_link_ksettings,
	.set_link_ksettings		= phy_ethtool_set_link_ksettings,
	.get_pauseparam			= fxgmac_get_pauseparam,
	.set_pauseparam			= fxgmac_set_pauseparam,
};

const struct ethtool_ops *fxgmac_get_ethtool_ops(void)
{
	return &fxgmac_ethtool_ops;
}
