// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/interrupt.h>
#include <net/addrconf.h>
#include <linux/inet.h>
#include <linux/tcp.h>

#include "yt6801_desc.h"
#include "yt6801_net.h"

const struct net_device_ops *fxgmac_get_netdev_ops(void);
static void fxgmac_napi_enable(struct fxgmac_pdata *pdata);

unsigned int fxgmac_get_netdev_ip4addr(struct fxgmac_pdata *pdata)
{
	unsigned int ipval = 0xc0a801ca; /* 192.168.1.202 */
	struct net_device *netdev = pdata->netdev;
	struct in_ifaddr *ifa;

	rcu_read_lock();

	/* We only get the first IPv4 addr. */
	ifa = rcu_dereference(netdev->ip_ptr->ifa_list);
	if (ifa) {
		ipval = (unsigned int)ifa->ifa_address;
		yt_dbg(pdata, "%s, netdev %s IPv4 address %pI4, mask: %pI4\n",
		       __func__, ifa->ifa_label, &ifa->ifa_address,
		       &ifa->ifa_mask);
	}

	rcu_read_unlock();

	return ipval;
}

unsigned char *fxgmac_get_netdev_ip6addr(struct fxgmac_pdata *pdata,
					 unsigned char *ipval,
					 unsigned char *ip6addr_solicited,
					 unsigned int ifa_flag)
{
	struct net_device *netdev = pdata->netdev;
	unsigned char solicited_ipval[16] = { 0 };
	unsigned char local_ipval[16] = { 0 };
	struct in6_addr *addr_ip6_solicited;
	struct in6_addr *addr_ip6;
	struct inet6_ifaddr *ifp;
	int err = -EADDRNOTAVAIL;
	struct inet6_dev *i6dev;

	if (!(ifa_flag &
	      (FXGMAC_NS_IFA_GLOBAL_UNICAST | FXGMAC_NS_IFA_LOCAL_LINK))) {
		yt_err(pdata, "%s, ifa_flag :%d is err.\n", __func__, ifa_flag);
		return NULL;
	}

	addr_ip6 = (struct in6_addr *)local_ipval;
	addr_ip6_solicited = (struct in6_addr *)solicited_ipval;

	if (ipval)
		addr_ip6 = (struct in6_addr *)ipval;

	if (ip6addr_solicited)
		addr_ip6_solicited = (struct in6_addr *)ip6addr_solicited;

	in6_pton("fe80::4808:8ffb:d93e:d753", -1, (u8 *)addr_ip6, -1, NULL);

	rcu_read_lock();
	i6dev = __in6_dev_get(netdev);
	if (!i6dev)
		goto err;

	read_lock_bh(&i6dev->lock);
	list_for_each_entry(ifp, &i6dev->addr_list, if_list) {
		if (((ifa_flag & FXGMAC_NS_IFA_GLOBAL_UNICAST) &&
		     ifp->scope != IFA_LINK) ||
		    ((ifa_flag & FXGMAC_NS_IFA_LOCAL_LINK) &&
		     ifp->scope == IFA_LINK)) {
			memcpy(addr_ip6, &ifp->addr, 16);
			addrconf_addr_solict_mult(addr_ip6, addr_ip6_solicited);
			err = 0;
			break;
		}
	}
	read_unlock_bh(&i6dev->lock);

	if (err)
		goto err;

	rcu_read_unlock();
	return ipval;
err:
	rcu_read_unlock();
	yt_err(pdata, "%s, get ipv6 addr err, use default.\n", __func__);
	return NULL;
}

static unsigned int fxgmac_desc_tx_avail(struct fxgmac_ring *ring)
{
	unsigned int avail;

	if (ring->dirty > ring->cur)
		avail = ring->dirty - ring->cur;
	else
		avail = ring->dma_desc_count - ring->cur + ring->dirty;

	return avail;
}

static unsigned int fxgmac_desc_rx_dirty(struct fxgmac_ring *ring)
{
	unsigned int dirty;

	if (ring->dirty <= ring->cur)
		dirty = ring->cur - ring->dirty;
	else
		dirty = ring->dma_desc_count - ring->dirty + ring->cur;

	return dirty;
}

void fxgmac_tx_start_xmit(struct fxgmac_channel *channel,
			  struct fxgmac_ring *ring)
{
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_desc_data *desc_data;

	/* Make sure everything is written before the register write */
	wmb();

	/* Issue a poll command to Tx DMA by writing address
	 * of next immediate free descriptor
	 */
	desc_data = FXGMAC_GET_DESC_DATA(ring, ring->cur);
	wr32_mac(pdata, lower_32_bits(desc_data->dma_desc_addr),
		 FXGMAC_DMA_REG(channel, DMA_CH_TDTR_LO));

	if (netif_msg_tx_done(pdata)) {
		yt_dbg(pdata,
		       "tx_start_xmit: dump before wr reg, dma base=0x%016llx,reg=0x%08x,",
		       desc_data->dma_desc_addr,
		       rd32_mac(pdata, FXGMAC_DMA_REG(channel, DMA_CH_TDTR_LO)));

		yt_dbg(pdata, "tx timer usecs=%u,tx_timer_active=%u\n",
		       pdata->tx_usecs, channel->tx_timer_active);
	}

	ring->tx.xmit_more = 0;
}

static netdev_tx_t fxgmac_maybe_stop_tx_queue(struct fxgmac_channel *channel,
					      struct fxgmac_ring *ring,
					      unsigned int count)
{
	struct fxgmac_pdata *pdata = channel->pdata;

	if (count > fxgmac_desc_tx_avail(ring)) {
		/* Avoid wrongly optimistic queue wake-up: tx poll thread must
		 * not miss a ring update when it notices a stopped queue.
		 */
		smp_wmb();
		netif_stop_subqueue(pdata->netdev, channel->queue_index);
		ring->tx.queue_stopped = 1;

		/* Sync with tx poll:
		 * - publish queue status and cur ring index (write barrier)
		 * - refresh dirty ring index (read barrier).
		 * May the current thread have a pessimistic view of the ring
		 * status and forget to wake up queue, a racing tx poll thread
		 * can't.
		 */
		smp_mb();
		if (count <= fxgmac_desc_tx_avail(ring)) {
			ring->tx.queue_stopped = 0;
			netif_start_subqueue(pdata->netdev,
					     channel->queue_index);
			fxgmac_tx_start_xmit(channel, ring);
		} else {
			/* If we haven't notified the hardware because of
			 * xmit_more support, tell it now
			 */
			if (ring->tx.xmit_more)
				fxgmac_tx_start_xmit(channel, ring);
			if (netif_msg_tx_done(pdata))
				yt_dbg(pdata, "about stop tx q, ret BUSY\n");
			return NETDEV_TX_BUSY;
		}
	}

	return NETDEV_TX_OK;
}

static void fxgmac_prep_vlan(struct sk_buff *skb,
			     struct fxgmac_pkt_info *pkt_info)
{
	if (skb_vlan_tag_present(skb))
		pkt_info->vlan_ctag = skb_vlan_tag_get(skb);
}

static int fxgmac_prep_tso(struct fxgmac_pdata *pdata, struct sk_buff *skb,
			   struct fxgmac_pkt_info *pkt_info)
{
	int ret;

	if (!FXGMAC_GET_BITS(pkt_info->attributes, TX_PKT_ATTR_TSO_ENABLE_POS,
			     TX_PKT_ATTR_TSO_ENABLE_LEN))
		return 0;

	ret = skb_cow_head(skb, 0);
	if (ret)
		return ret;

	pkt_info->header_len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	pkt_info->tcp_header_len = tcp_hdrlen(skb);
	pkt_info->tcp_payload_len = skb->len - pkt_info->header_len;
	pkt_info->mss = skb_shinfo(skb)->gso_size;

	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata,
		       "header_len=%u, tcp_header_len=%u, tcp_payload_len=%u, mss=%u\n",
		       pkt_info->header_len, pkt_info->tcp_header_len,
		       pkt_info->tcp_payload_len, pkt_info->mss);

	/* Update the number of packets that will ultimately be transmitted
	 * along with the extra bytes for each extra packet
	 */
	pkt_info->tx_packets = skb_shinfo(skb)->gso_segs;
	pkt_info->tx_bytes += (pkt_info->tx_packets - 1) * pkt_info->header_len;

	return 0;
}

static int fxgmac_is_tso(struct sk_buff *skb)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	return 1;
}

static void fxgmac_prep_tx_pkt(struct fxgmac_pdata *pdata,
			       struct fxgmac_ring *ring, struct sk_buff *skb,
			       struct fxgmac_pkt_info *pkt_info)
{
	u32 *attr = &pkt_info->attributes;
	u32 len, context_desc = 0;

	pkt_info->skb = skb;
	pkt_info->desc_count = 0;
	pkt_info->tx_packets = 1;
	pkt_info->tx_bytes = skb->len;

	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata, "%s, pkt desc cnt=%d,skb len=%d, skbheadlen=%d\n",
		       __func__, pkt_info->desc_count, skb->len,
		       skb_headlen(skb));

	if (fxgmac_is_tso(skb)) {
		/* TSO requires an extra descriptor if mss is different */
		if (skb_shinfo(skb)->gso_size != ring->tx.cur_mss) {
			context_desc = 1;
			pkt_info->desc_count++;
		}
		if (netif_msg_tx_done(pdata))
			yt_dbg(pdata,
			       "fxgmac_is_tso=%d, ip_summed=%d,skb gso=%d\n",
			       ((skb->ip_summed == CHECKSUM_PARTIAL) &&
				(skb_is_gso(skb))) ? 1 : 0,
			       skb->ip_summed, skb_is_gso(skb) ? 1 : 0);

		/* TSO requires an extra descriptor for TSO header */
		pkt_info->desc_count++;
		fxgmac_set_bits(attr, TX_PKT_ATTR_TSO_ENABLE_POS,
				TX_PKT_ATTR_TSO_ENABLE_LEN, 1);
		fxgmac_set_bits(attr, TX_PKT_ATTR_CSUM_ENABLE_POS,
				TX_PKT_ATTR_CSUM_ENABLE_LEN, 1);
		if (netif_msg_tx_done(pdata))
			yt_dbg(pdata, "%s,tso, pkt desc cnt=%d\n", __func__,
			       pkt_info->desc_count);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL)
		fxgmac_set_bits(attr, TX_PKT_ATTR_CSUM_ENABLE_POS,
				TX_PKT_ATTR_CSUM_ENABLE_LEN, 1);

	if (skb_vlan_tag_present(skb)) {
		/* VLAN requires an extra descriptor if tag is different */
		if (skb_vlan_tag_get(skb) != ring->tx.cur_vlan_ctag)
			/* We can share with the TSO context descriptor */
			if (!context_desc) {
				context_desc = 1;
				pkt_info->desc_count++;
			}

		fxgmac_set_bits(attr, TX_PKT_ATTR_VLAN_CTAG_POS,
				TX_PKT_ATTR_VLAN_CTAG_LEN, 1);
		if (netif_msg_tx_done(pdata))
			yt_dbg(pdata, "%s,VLAN, pkt desc cnt=%d,vlan=0x%04x\n",
			       __func__, pkt_info->desc_count,
			       skb_vlan_tag_get(skb));
	}

	for (len = skb_headlen(skb); len;) {
		pkt_info->desc_count++;
		len -= min_t(unsigned int, len, FXGMAC_TX_MAX_BUF_SIZE);
	}

	for (u32 i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		for (len = skb_frag_size(&skb_shinfo(skb)->frags[i]); len;) {
			pkt_info->desc_count++;
			len -= min_t(unsigned int, len, FXGMAC_TX_MAX_BUF_SIZE);
		}

	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata,
		       "%s,pkt desc cnt%d,skb len%d, skbheadlen=%d,frags=%d\n",
		       __func__, pkt_info->desc_count, skb->len,
		       skb_headlen(skb), skb_shinfo(skb)->nr_frags);
}

static int fxgmac_calc_rx_buf_size(struct fxgmac_pdata *pdata, unsigned int mtu)
{
	u32 rx_buf_size, max_mtu;

	max_mtu = FXGMAC_JUMBO_PACKET_MTU - ETH_HLEN;
	if (mtu > max_mtu) {
		yt_err(pdata, "MTU exceeds maximum supported value\n");
		return -EINVAL;
	}

	rx_buf_size = mtu + ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN;
	rx_buf_size =
		clamp_val(rx_buf_size, FXGMAC_RX_MIN_BUF_SIZE, PAGE_SIZE * 4);

	rx_buf_size = (rx_buf_size + FXGMAC_RX_BUF_ALIGN - 1) &
		      ~(FXGMAC_RX_BUF_ALIGN - 1);

	return rx_buf_size;
}

static void fxgmac_enable_rx_tx_ints(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	enum fxgmac_int int_id;

	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		if (channel->tx_ring && channel->rx_ring)
			int_id = FXGMAC_INT_DMA_CH_SR_TI_RI;
		else if (channel->tx_ring)
			int_id = FXGMAC_INT_DMA_CH_SR_TI;
		else if (channel->rx_ring)
			int_id = FXGMAC_INT_DMA_CH_SR_RI;
		else
			continue;

		hw_ops->enable_channel_irq(channel, int_id);
	}
}

static int fxgmac_misc_poll(struct napi_struct *napi, int budget)
{
	struct fxgmac_pdata *pdata =
		container_of(napi, struct fxgmac_pdata, napi_misc);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	if (napi_complete_done(napi, 0))
		hw_ops->enable_msix_one_irq(pdata, MSI_ID_PHY_OTHER);

	return 0;
}

static irqreturn_t fxgmac_misc_isr(int irq, void *data)
{
	struct fxgmac_pdata *pdata = data;
	struct fxgmac_hw_ops *hw_ops;
	u32 val;

	val = rd32_mem(pdata, MGMT_INT_CTRL0);
	if (!(val & MGMT_INT_CTRL0_INT_STATUS_MISC))
		return IRQ_HANDLED;

	hw_ops = &pdata->hw_ops;
	hw_ops->disable_msix_one_irq(pdata, MSI_ID_PHY_OTHER);
	hw_ops->clear_misc_int_status(pdata);

	napi_schedule_irqoff(&pdata->napi_misc);

	return IRQ_HANDLED;
}

static irqreturn_t fxgmac_isr(int irq, void *data)
{
	struct fxgmac_pdata *pdata = data;
	u32 val, mgm_intctrl_val, isr;
	struct fxgmac_hw_ops *hw_ops;

	val = rd32_mem(pdata, MGMT_INT_CTRL0);
	if (!(val &
	      (MGMT_INT_CTRL0_INT_STATUS_RX | MGMT_INT_CTRL0_INT_STATUS_TX |
	       MGMT_INT_CTRL0_INT_STATUS_MISC)))
		return IRQ_HANDLED;

	hw_ops = &pdata->hw_ops;
	hw_ops->disable_mgm_irq(pdata);
	mgm_intctrl_val = val;
	pdata->stats.mgmt_int_isr++;

	/* Handle dma channel isr */
	for (u32 i = 0; i < pdata->channel_count; i++) {
		isr = rd32_mac(pdata, FXGMAC_DMA_REG(pdata->channel_head + i, DMA_CH_SR));

		if (isr & BIT(DMA_CH_SR_TPS_POS))
			pdata->stats.tx_process_stopped++;

		if (isr & BIT(DMA_CH_SR_RPS_POS))
			pdata->stats.rx_process_stopped++;

		if (isr & BIT(DMA_CH_SR_TBU_POS))
			pdata->stats.tx_buffer_unavailable++;

		if (isr & BIT(DMA_CH_SR_RBU_POS))
			pdata->stats.rx_buffer_unavailable++;

		/* Restart the device on a Fatal Bus Error */
		if (isr & BIT(DMA_CH_SR_FBE_POS)) {
			pdata->stats.fatal_bus_error++;
			schedule_work(&pdata->restart_work);
		}

		/* Clear all interrupt signals */
		wr32_mac(pdata, isr, FXGMAC_DMA_REG(pdata->channel_head + i, DMA_CH_SR));
	}

	if (mgm_intctrl_val & MGMT_INT_CTRL0_INT_STATUS_MISC)
		hw_ops->clear_misc_int_status(pdata);

	if (napi_schedule_prep(&pdata->napi)) {
		pdata->stats.napi_poll_isr++;
		__napi_schedule_irqoff(&pdata->napi); /* Turn on polling */
	}

	return IRQ_HANDLED;
}

static irqreturn_t fxgmac_dma_isr(int irq, void *data)
{
	struct fxgmac_channel *channel = data;
	struct fxgmac_hw_ops *hw_ops;
	struct fxgmac_pdata *pdata;
	u32 message_id, val = 0;

	pdata = channel->pdata;
	hw_ops = &pdata->hw_ops;

	if (irq == channel->dma_irq_tx) {
		message_id = MSI_ID_TXQ0;
		hw_ops->disable_msix_one_irq(pdata, message_id);
		fxgmac_set_bits(&val, DMA_CH_SR_TI_POS, DMA_CH_SR_TI_LEN, 1);
		wr32_mac(pdata, val, FXGMAC_DMA_REG(channel, DMA_CH_SR));
		napi_schedule_irqoff(&channel->napi_tx);
		return IRQ_HANDLED;
	}

	message_id = channel->queue_index;
	hw_ops->disable_msix_one_irq(pdata, message_id);
	val = rd32_mac(pdata, FXGMAC_DMA_REG(channel, DMA_CH_SR));
	fxgmac_set_bits(&val, DMA_CH_SR_RI_POS, DMA_CH_SR_RI_LEN, 1);
	wr32_mac(pdata, val, FXGMAC_DMA_REG(channel, DMA_CH_SR));
	napi_schedule_irqoff(&channel->napi_rx);
	return IRQ_HANDLED;
}

#define FXGMAC_NAPI_ENABLE			0x1
#define FXGMAC_NAPI_DISABLE			0x0
static void fxgmac_napi_disable(struct fxgmac_pdata *pdata)
{
	u32 i, *flags = &pdata->int_flags;
	struct fxgmac_channel *channel;
	u32 misc_napi, tx, rx, val;

	misc_napi = FIELD_GET(BIT(FXGMAC_FLAG_MISC_NAPI_POS), *flags);
	tx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_TX_NAPI_POS,
			     FXGMAC_FLAG_TX_NAPI_LEN);
	rx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_RX_NAPI_POS,
			     FXGMAC_FLAG_RX_NAPI_LEN);

	if (!pdata->per_channel_irq) {
		val = FIELD_GET(BIT(FXGMAC_FLAG_LEGACY_NAPI_POS), *flags);
		if (val == 0)
			return;

		napi_disable(&pdata->napi);
		netif_napi_del(&pdata->napi);
		fxgmac_set_bits(flags, FXGMAC_FLAG_LEGACY_NAPI_POS,
				FXGMAC_FLAG_LEGACY_NAPI_LEN,
				FXGMAC_NAPI_DISABLE);
		return;
	}

	channel = pdata->channel_head;
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (FXGMAC_GET_BITS(rx, i, FXGMAC_FLAG_PER_RX_NAPI_LEN)) {
			napi_disable(&channel->napi_rx);
			netif_napi_del(&channel->napi_rx);
			fxgmac_set_bits(flags, FXGMAC_FLAG_RX_NAPI_POS + i,
					FXGMAC_FLAG_PER_RX_NAPI_LEN,
					FXGMAC_NAPI_DISABLE);
		}

		if (FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i) && tx) {
			napi_disable(&channel->napi_tx);
			netif_napi_del(&channel->napi_tx);
			fxgmac_set_bits(flags, FXGMAC_FLAG_TX_NAPI_POS,
					FXGMAC_FLAG_TX_NAPI_LEN,
					FXGMAC_NAPI_DISABLE);
		}

		if (netif_msg_drv(pdata))
			yt_dbg(pdata,
			       "napi_disable, msix ch%d, napi disabled done. ",
			       i);
	}

	if (misc_napi) {
		napi_disable(&pdata->napi_misc);
		netif_napi_del(&pdata->napi_misc);
		fxgmac_set_bits(flags, FXGMAC_FLAG_MISC_NAPI_POS,
				FXGMAC_FLAG_MISC_NAPI_LEN, FXGMAC_NAPI_DISABLE);
	}
}

#define FXGMAC_IRQ_ENABLE			0x1
#define FXGMAC_IRQ_DISABLE			0x0
static int fxgmac_request_irqs(struct fxgmac_pdata *pdata)
{
	struct net_device *netdev = pdata->netdev;
	u32 *flags = &pdata->int_flags;
	struct fxgmac_channel *channel;
	u32 misc, tx, rx, need_free;
	u32 i, msix, msi;
	int ret;

	msi = FIELD_GET(FXGMAC_FLAG_MSI_ENABLED, *flags);
	msix = FIELD_GET(FXGMAC_FLAG_MSIX_ENABLED, *flags);
	need_free = FIELD_GET(BIT(FXGMAC_FLAG_LEGACY_IRQ_POS), *flags);

	if (!msix && !need_free) {
		ret = devm_request_irq(pdata->dev, pdata->dev_irq, fxgmac_isr,
				       msi ? 0 : IRQF_SHARED, netdev->name,
				       pdata);
		if (ret) {
			yt_err(pdata, "error requesting irq %d, ret = %d\n",
			       pdata->dev_irq, ret);
			return ret;
		}

		fxgmac_set_bits(flags, FXGMAC_FLAG_LEGACY_IRQ_POS,
				FXGMAC_FLAG_LEGACY_IRQ_LEN, FXGMAC_IRQ_ENABLE);
	}

	if (!pdata->per_channel_irq)
		return 0;

	channel = pdata->channel_head;

	tx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_TX_IRQ_POS,
			     FXGMAC_FLAG_TX_IRQ_LEN);
	rx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_RX_IRQ_POS,
			     FXGMAC_FLAG_RX_IRQ_LEN);
	misc = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_MISC_IRQ_POS,
			       FXGMAC_FLAG_MISC_IRQ_LEN);
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		snprintf(channel->dma_irq_rx_name,
			 sizeof(channel->dma_irq_rx_name) - 1, "%s-ch%d-Rx-%u",
			 netdev_name(netdev), i, channel->queue_index);

		if (FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i) && !tx) {
			snprintf(channel->dma_irq_tx_name,
				 sizeof(channel->dma_irq_tx_name) - 1,
				 "%s-ch%d-Tx-%u", netdev_name(netdev), i,
				 channel->queue_index);
			ret = devm_request_irq(pdata->dev, channel->dma_irq_tx,
					       fxgmac_dma_isr, 0,
					       channel->dma_irq_tx_name,
					       channel);
			if (ret) {
				yt_err(pdata,
				       "%s, err with MSIx irq, request for ch %d tx, ret=%d\n",
				       __func__, i, ret);
				goto err_irq;
			}

			fxgmac_set_bits(flags, FXGMAC_FLAG_TX_IRQ_POS,
					FXGMAC_FLAG_TX_IRQ_LEN,
					FXGMAC_IRQ_ENABLE);

			if (netif_msg_drv(pdata)) {
				yt_dbg(pdata,
				       "%s, MSIx irq_tx request ok, ch=%d, irq=%d,%s\n",
				       __func__, i, channel->dma_irq_tx,
				       channel->dma_irq_tx_name);
			}
		}

		if (!FXGMAC_GET_BITS(rx, i, FXGMAC_FLAG_PER_RX_IRQ_LEN)) {
			ret = devm_request_irq(pdata->dev, channel->dma_irq_rx,
					       fxgmac_dma_isr, 0,
					       channel->dma_irq_rx_name,
					       channel);
			if (ret) {
				yt_err(pdata, "error requesting irq %d\n",
				       channel->dma_irq_rx);
				goto err_irq;
			}
			fxgmac_set_bits(flags, FXGMAC_FLAG_RX_IRQ_POS + i,
					FXGMAC_FLAG_PER_RX_IRQ_LEN,
					FXGMAC_IRQ_ENABLE);
		}
	}

	if (!misc) {
		snprintf(pdata->misc_irq_name, sizeof(pdata->misc_irq_name) - 1,
			 "%s-misc", netdev_name(netdev));
		ret = devm_request_irq(pdata->dev, pdata->misc_irq,
				       fxgmac_misc_isr, 0, pdata->misc_irq_name,
				       pdata);
		if (ret) {
			yt_err(pdata,
			       "error requesting misc irq %d, ret = %d\n",
			       pdata->misc_irq, ret);
			goto err_irq;
		}
		fxgmac_set_bits(flags, FXGMAC_FLAG_MISC_IRQ_POS,
				FXGMAC_FLAG_MISC_IRQ_LEN, FXGMAC_IRQ_ENABLE);
	}

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, MSIx irq request ok, total=%d,%d~%d\n",
		       __func__, i, (pdata->channel_head)[0].dma_irq_rx,
		       (pdata->channel_head)[i - 1].dma_irq_rx);

	return 0;

err_irq:
	yt_err(pdata, "%s, err with MSIx irq request at %d,ret=%d\n", __func__,
	       i, ret);

	for (i--, channel--; i < pdata->channel_count; i--, channel--) {
		if (FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i) && tx) {
			fxgmac_set_bits(flags, FXGMAC_FLAG_TX_IRQ_POS,
					FXGMAC_FLAG_TX_IRQ_LEN,
					FXGMAC_IRQ_DISABLE);
			devm_free_irq(pdata->dev, channel->dma_irq_tx, channel);
		}

		if (FXGMAC_GET_BITS(rx, i, FXGMAC_FLAG_PER_RX_IRQ_LEN)) {
			fxgmac_set_bits(flags, FXGMAC_FLAG_RX_IRQ_POS + i,
					FXGMAC_FLAG_PER_RX_IRQ_LEN,
					FXGMAC_IRQ_DISABLE);

			devm_free_irq(pdata->dev, channel->dma_irq_rx, channel);
		}
	}

	if (misc) {
		fxgmac_set_bits(flags, FXGMAC_FLAG_MISC_IRQ_POS,
				FXGMAC_FLAG_MISC_IRQ_LEN, FXGMAC_IRQ_DISABLE);
		devm_free_irq(pdata->dev, pdata->misc_irq, pdata);
	}

	return ret;
}

static void fxgmac_free_irqs(struct fxgmac_pdata *pdata)
{
	u32 i, need_free, misc, tx, rx, msix;
	u32 *flags = &pdata->int_flags;
	struct fxgmac_channel *channel;

	msix = FIELD_GET(FXGMAC_FLAG_MSIX_ENABLED, *flags);
	need_free = FIELD_GET(BIT(FXGMAC_FLAG_LEGACY_IRQ_POS), *flags);
	if (!msix && need_free) {
		devm_free_irq(pdata->dev, pdata->dev_irq, pdata);
		fxgmac_set_bits(flags, FXGMAC_FLAG_LEGACY_IRQ_POS,
				FXGMAC_FLAG_LEGACY_IRQ_LEN, FXGMAC_IRQ_DISABLE);
	}

	if (!pdata->per_channel_irq)
		return;

	channel = pdata->channel_head;

	misc = FIELD_GET(BIT(FXGMAC_FLAG_MISC_IRQ_POS), *flags);
	tx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_TX_IRQ_POS,
			     FXGMAC_FLAG_TX_IRQ_LEN);
	rx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_RX_IRQ_POS,
			     FXGMAC_FLAG_RX_IRQ_LEN);
	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i) && tx) {
			fxgmac_set_bits(flags, FXGMAC_FLAG_TX_IRQ_POS,
					FXGMAC_FLAG_TX_IRQ_LEN,
					FXGMAC_IRQ_DISABLE);
			devm_free_irq(pdata->dev, channel->dma_irq_tx, channel);
			if (netif_msg_drv(pdata))
				yt_dbg(pdata,
				       "%s, MSIx irq_tx clear done ch=%d\n",
				       __func__, i);
		}

		if (FXGMAC_GET_BITS(rx, i, FXGMAC_FLAG_PER_RX_IRQ_LEN)) {
			fxgmac_set_bits(flags, FXGMAC_FLAG_RX_IRQ_POS + i,
					FXGMAC_FLAG_PER_RX_IRQ_LEN,
					FXGMAC_IRQ_DISABLE);
			devm_free_irq(pdata->dev, channel->dma_irq_rx, channel);
		}
	}

	if (misc) {
		fxgmac_set_bits(flags, FXGMAC_FLAG_MISC_IRQ_POS,
				FXGMAC_FLAG_MISC_IRQ_LEN, FXGMAC_IRQ_DISABLE);
		devm_free_irq(pdata->dev, pdata->misc_irq, pdata);
	}
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, MSIx rx irq clear done, total=%d\n",
		       __func__, i);
}

void fxgmac_free_tx_data(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;
	struct fxgmac_desc_data *desc_data;
	struct fxgmac_ring *ring;

	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->tx_ring;
		if (!ring)
			break;

		for (u32 j = 0; j < ring->dma_desc_count; j++) {
			desc_data = FXGMAC_GET_DESC_DATA(ring, j);
			fxgmac_desc_data_unmap(pdata, desc_data);
		}
	}
}

void fxgmac_free_rx_data(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;
	struct fxgmac_desc_data *desc_data;
	struct fxgmac_ring *ring;

	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		ring = channel->rx_ring;
		if (!ring)
			break;

		for (u32 j = 0; j < ring->dma_desc_count; j++) {
			desc_data = FXGMAC_GET_DESC_DATA(ring, j);
			fxgmac_desc_data_unmap(pdata, desc_data);
		}
	}
}

static  void fxgmac_phylink_handler(struct net_device *ndev)
{
	struct fxgmac_pdata *pdata = netdev_priv(ndev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	pdata->phy_link = pdata->phydev->link;
	pdata->phy_speed = pdata->phydev->speed;
	pdata->phy_duplex = pdata->phydev->duplex;

	yt_dbg(pdata, "EPHY_CTRL:%x, link:%d, speed:%d,  duplex:%x.\n",
	       rd32_mem(pdata, EPHY_CTRL), pdata->phy_link, pdata->phy_speed,
	       pdata->phy_duplex);

	if (pdata->phy_link) {
		hw_ops->config_mac_speed(pdata);
		hw_ops->enable_rx(pdata);
		hw_ops->enable_tx(pdata);
		netif_carrier_on(pdata->netdev);
		if (netif_running(pdata->netdev)) {
			netif_tx_wake_all_queues(pdata->netdev);
			yt_dbg(pdata, "%s now is link up, mac_speed=%d.\n",
			       netdev_name(pdata->netdev), pdata->phy_speed);
		}
	} else {
		netif_carrier_off(pdata->netdev);
		netif_tx_stop_all_queues(pdata->netdev);
		hw_ops->disable_rx(pdata);
		hw_ops->disable_tx(pdata);
		yt_dbg(pdata, "%s now is link down\n",
		       netdev_name(pdata->netdev));
	}

	phy_print_status(pdata->phydev);
}

static int fxgmac_phy_connect(struct fxgmac_pdata *pdata)
{
	struct phy_device *phydev = pdata->phydev;
	int ret;

	ret = phy_connect_direct(pdata->netdev, phydev, fxgmac_phylink_handler,
				 PHY_INTERFACE_MODE_RGMII);
	if (ret)
		return ret;

	phy_attached_info(phydev);
	return 0;
}

static void fxgmac_enable_msix_irqs(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	for (u32 intid = 0; intid < MSIX_TBL_MAX_NUM; intid++)
		hw_ops->enable_msix_one_irq(pdata, intid);
}

int fxgmac_phy_irq_enable(struct fxgmac_pdata *pdata, bool clear_phy_interrupt)
{
	struct phy_device *phydev = pdata->phydev;

	if (clear_phy_interrupt &&
	    phy_read(phydev, PHY_INT_STATUS) < 0)
		return -ETIMEDOUT;

	return phy_modify(phydev, PHY_INT_MASK,
				     PHY_INT_MASK_LINK_UP |
					     PHY_INT_MASK_LINK_DOWN,
				     PHY_INT_MASK_LINK_UP |
					     PHY_INT_MASK_LINK_DOWN);
}

int fxgmac_start(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	u32 val;
	int ret;

	if (pdata->dev_state != FXGMAC_DEV_OPEN &&
	    pdata->dev_state != FXGMAC_DEV_STOP &&
	    pdata->dev_state != FXGMAC_DEV_RESUME) {
		yt_dbg(pdata, " dev_state err:%x\n", pdata->dev_state);
		return 0;
	}

	if (pdata->dev_state != FXGMAC_DEV_STOP) {
		hw_ops->reset_phy(pdata);
		hw_ops->release_phy(pdata);
		yt_dbg(pdata, "reset phy.\n");
	}

	if (pdata->dev_state == FXGMAC_DEV_OPEN) {
		ret = fxgmac_phy_connect(pdata);
		if (ret < 0)
			return ret;

		yt_dbg(pdata, "fxgmac_phy_connect.\n");
	}

	phy_init_hw(pdata->phydev);
	phy_resume(pdata->phydev);

	hw_ops->pcie_init(pdata);
	if (test_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate)) {
		yt_err(pdata,
		       "fxgmac powerstate is %lu when config power up.\n",
		       pdata->powerstate);
	}

	hw_ops->config_power_up(pdata);
	hw_ops->dismiss_all_int(pdata);
	ret = hw_ops->init(pdata);
	if (ret < 0) {
		yt_err(pdata, "fxgmac hw init error.\n");
		return ret;
	}

	fxgmac_napi_enable(pdata);
	ret = fxgmac_request_irqs(pdata);
	if (ret < 0)
		return ret;

	/* Config interrupt to level signal */
	val = rd32_mac(pdata, DMA_MR);
	fxgmac_set_bits(&val, DMA_MR_INTM_POS, DMA_MR_INTM_LEN, 2);
	fxgmac_set_bits(&val, DMA_MR_QUREAD_POS, DMA_MR_QUREAD_LEN, 1);
	wr32_mac(pdata, val, DMA_MR);

	hw_ops->enable_mgm_irq(pdata);
	hw_ops->set_interrupt_moderation(pdata);

	if (pdata->per_channel_irq) {
		fxgmac_enable_msix_irqs(pdata);
		ret = fxgmac_phy_irq_enable(pdata, true);
		if (ret < 0)
			goto dis_napi;
	}

	fxgmac_enable_rx_tx_ints(pdata);
	phy_speed_up(pdata->phydev);
	genphy_soft_reset(pdata->phydev);

	pdata->dev_state = FXGMAC_DEV_START;
	phy_start(pdata->phydev);

	return 0;

dis_napi:
	fxgmac_napi_disable(pdata);
	hw_ops->exit(pdata);
	yt_err(pdata, "%s irq err.\n", __func__);
	return ret;
}

static void fxgmac_disable_msix_irqs(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	for (u32 intid = 0; intid < MSIX_TBL_MAX_NUM; intid++)
		hw_ops->disable_msix_one_irq(pdata, intid);
}

void fxgmac_stop(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	struct netdev_queue *txq;

	if (pdata->dev_state != FXGMAC_DEV_START)
		return;

	pdata->dev_state = FXGMAC_DEV_STOP;

	if (pdata->per_channel_irq)
		fxgmac_disable_msix_irqs(pdata);
	else
		hw_ops->disable_mgm_irq(pdata);

	pdata->phy_link = false;
	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);
	hw_ops->disable_tx(pdata);
	hw_ops->disable_rx(pdata);
	fxgmac_free_irqs(pdata);
	fxgmac_napi_disable(pdata);
	phy_stop(pdata->phydev);

	txq = netdev_get_tx_queue(netdev, pdata->channel_head->queue_index);
	netdev_tx_reset_queue(txq);
}

void fxgmac_restart(struct fxgmac_pdata *pdata)
{
	int ret;

	/* If not running, "restart" will happen on open */
	if (!netif_running(pdata->netdev) &&
	    pdata->dev_state != FXGMAC_DEV_START)
		return;

	mutex_lock(&pdata->mutex);
	fxgmac_stop(pdata);
	fxgmac_free_tx_data(pdata);
	fxgmac_free_rx_data(pdata);
	ret = fxgmac_start(pdata);
	if (ret < 0)
		yt_err(pdata, "%s err.\n", __func__);

	mutex_unlock(&pdata->mutex);
}

static void fxgmac_restart_work(struct work_struct *work)
{
	struct fxgmac_pdata *pdata =
		container_of(work, struct fxgmac_pdata, restart_work);

	rtnl_lock();
	fxgmac_restart(pdata);
	rtnl_unlock();
}

int fxgmac_net_powerup(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	int ret;

	/* Signal that we are up now */
	pdata->powerstate = 0;
	if (__test_and_set_bit(FXGMAC_POWER_STATE_UP, &pdata->powerstate))
		return 0; /* do nothing if already up */

	ret = fxgmac_start(pdata);
	if (ret < 0) {
		yt_err(pdata, "%s: fxgmac_start err: %d\n", __func__, ret);
		return ret;
	}

	/* Must call it after fxgmac_start,because it will be
	 * enable in fxgmac_start
	 */
	hw_ops->disable_arp_offload(pdata);

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, powerstate :%d.\n", __func__,
		       pdata->powerstate);

	return 0;
}

int fxgmac_config_wol(struct fxgmac_pdata *pdata, bool en)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	if (!pdata->hw_feat.rwk) {
		yt_err(pdata, "error configuring WOL - not supported.\n");
		return -EOPNOTSUPP;
	}

	hw_ops->disable_wake_magic_pattern(pdata);
	hw_ops->disable_wake_pattern(pdata);
	hw_ops->disable_wake_link_change(pdata);

	if (en) {
		/* Config mac address for rx of magic or ucast */
		hw_ops->set_mac_address(pdata, (u8 *)(pdata->netdev->dev_addr));

		/* Enable Magic packet */
		if (pdata->wol & WAKE_MAGIC)
			hw_ops->enable_wake_magic_pattern(pdata);

		/* Enable global unicast packet */
		if (pdata->wol &
		    (WAKE_UCAST | WAKE_MCAST | WAKE_BCAST | WAKE_ARP))
			hw_ops->enable_wake_pattern(pdata);

		/* Enable ephy link change */
		if (pdata->wol & WAKE_PHY)
			hw_ops->enable_wake_link_change(pdata);
	}
	device_set_wakeup_enable((pdata->dev), en);

	return 0;
}

int fxgmac_net_powerdown(struct fxgmac_pdata *pdata, bool wake_en)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	int ret;

	/* Signal that we are down to the interrupt handler */
	if (__test_and_set_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate))
		return 0; /* do nothing if already down */

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, continue with down process.\n", __func__);

	__clear_bit(FXGMAC_POWER_STATE_UP, &pdata->powerstate);
	netif_tx_stop_all_queues(netdev); /* Shut off incoming Tx traffic */

	/* Call carrier off first to avoid false dev_watchdog timeouts */
	netif_carrier_off(netdev);
	netif_tx_disable(netdev);
	hw_ops->disable_rx(pdata); /* Disable Rx */

	/* Synchronize_rcu() needed for pending XDP buffers to drain */
	synchronize_rcu();

	fxgmac_stop(pdata);
	ret = hw_ops->pre_power_down(pdata);
	if (ret < 0) {
		yt_err(pdata, "pre_power_down err.\n");
		return ret;
	}

	if (!test_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate)) {
		yt_err(pdata,
		       "fxgmac powerstate is %lu when config powe down.\n",
		       pdata->powerstate);
	}

	/* Set mac to lowpower mode and enable wol accordingly */
	hw_ops->disable_tx(pdata);
	hw_ops->disable_rx(pdata);
	fxgmac_config_wol(pdata, wake_en);
	hw_ops->config_power_down(pdata, wake_en);

	fxgmac_free_tx_data(pdata);
	fxgmac_free_rx_data(pdata);

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, powerstate :%d.\n", __func__,
		       pdata->powerstate);

	return 0;
}

static int fxgmac_open(struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	int ret;

	mutex_lock(&pdata->mutex);
	pdata->dev_state = FXGMAC_DEV_OPEN;

	/* Calculate the Rx buffer size before allocating rings */
	ret = fxgmac_calc_rx_buf_size(pdata, netdev->mtu);
	if (ret < 0)
		goto unlock;
	pdata->rx_buf_size = ret;

	/* Allocate the channels and rings */
	ret = fxgmac_channels_rings_alloc(pdata);
	if (ret < 0)
		goto unlock;

	INIT_WORK(&pdata->restart_work, fxgmac_restart_work);

	ret = fxgmac_start(pdata);
	if (ret < 0)
		goto err_channels_and_rings;

	mutex_unlock(&pdata->mutex);

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s ok\n", __func__);

	return 0;

err_channels_and_rings:
	fxgmac_channels_rings_free(pdata);
	yt_dbg(pdata, "%s, channel alloc err\n", __func__);
unlock:
	mutex_unlock(&pdata->mutex);
	return ret;
}

static int fxgmac_close(struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	mutex_lock(&pdata->mutex);
	fxgmac_stop(pdata);		/* Stop the device */
	pdata->dev_state = FXGMAC_DEV_CLOSE;
	fxgmac_channels_rings_free(pdata); /* Free the channels and rings */
	hw_ops->reset_phy(pdata);
	phy_disconnect(pdata->phydev);
	mutex_unlock(&pdata->mutex);

	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s ok\n", __func__);

	return 0;
}

static void fxgmac_dump_state(struct fxgmac_pdata *pdata)
{
	struct fxgmac_channel *channel = pdata->channel_head;
	struct fxgmac_stats *pstats = &pdata->stats;
	struct fxgmac_ring *ring;

	ring = &channel->tx_ring[0];
	yt_err(pdata, "Tx descriptor info:\n");
	yt_err(pdata, "Tx cur = 0x%x\n", ring->cur);
	yt_err(pdata, "Tx dirty = 0x%x\n", ring->dirty);
	yt_err(pdata, "Tx dma_desc_head = %pad\n", &ring->dma_desc_head);
	yt_err(pdata, "Tx desc_data_head = %pad\n", &ring->desc_data_head);

	for (u32 i = 0; i < pdata->channel_count; i++, channel++) {
		ring = &channel->rx_ring[0];
		yt_err(pdata, "Rx[%d] descriptor info:\n", i);
		yt_err(pdata, "Rx cur = 0x%x\n", ring->cur);
		yt_err(pdata, "Rx dirty = 0x%x\n", ring->dirty);
		yt_err(pdata, "Rx dma_desc_head = %pad\n",
		       &ring->dma_desc_head);
		yt_err(pdata, "Rx desc_data_head = %pad\n",
		       &ring->desc_data_head);
	}

	yt_err(pdata, "Device Registers:\n");
	yt_err(pdata, "MAC_ISR = %08x\n", rd32_mac(pdata, MAC_ISR));
	yt_err(pdata, "MAC_IER = %08x\n", rd32_mac(pdata, MAC_IER));
	yt_err(pdata, "MMC_RISR = %08x\n", rd32_mac(pdata, MMC_RISR));
	yt_err(pdata, "MMC_RIER = %08x\n", rd32_mac(pdata, MMC_RIER));
	yt_err(pdata, "MMC_TISR = %08x\n", rd32_mac(pdata, MMC_TISR));
	yt_err(pdata, "MMC_TIER = %08x\n", rd32_mac(pdata, MMC_TIER));

	yt_err(pdata, "EPHY_CTRL = %04x\n", rd32_mem(pdata, EPHY_CTRL));
	yt_err(pdata, "MGMT_INT_CTRL0 = %04x\n",
	       rd32_mem(pdata, MGMT_INT_CTRL0));
	yt_err(pdata, "LPW_CTRL = %04x\n", rd32_mem(pdata, LPW_CTRL));
	yt_err(pdata, "MSIX_TBL_MASK = %04x\n", rd32_mem(pdata, MSIX_TBL_MASK));

	yt_err(pdata, "Dump nonstick regs:\n");
	for (u32 i = GLOBAL_CTRL0; i < MSI_PBA; i += 4)
		yt_err(pdata, "[%d] = %04x\n", i / 4, rd32_mem(pdata, i));

	pdata->hw_ops.read_mmc_stats(pdata);

	yt_err(pdata, "Dump TX counters:\n");
	yt_err(pdata, "tx_packets %lld\n", pstats->txframecount_gb);
	yt_err(pdata, "tx_errors %lld\n",
	       pstats->txframecount_gb - pstats->txframecount_g);
	yt_err(pdata, "tx_multicastframes_errors %lld\n",
	       pstats->txmulticastframes_gb - pstats->txmulticastframes_g);
	yt_err(pdata, "tx_broadcastframes_errors %lld\n",
	       pstats->txbroadcastframes_gb - pstats->txbroadcastframes_g);

	yt_err(pdata, "txunderflowerror %lld\n", pstats->txunderflowerror);
	yt_err(pdata, "txdeferredframes %lld\n",
	       pstats->txdeferredframes);
	yt_err(pdata, "txlatecollisionframes %lld\n",
	       pstats->txlatecollisionframes);
	yt_err(pdata, "txexcessivecollisionframes %lld\n",
	       pstats->txexcessivecollisionframes);
	yt_err(pdata, "txcarriererrorframes %lld\n",
	       pstats->txcarriererrorframes);
	yt_err(pdata, "txexcessivedeferralerror %lld\n",
	       pstats->txexcessivedeferralerror);

	yt_err(pdata, "txsinglecollision_g %lld\n",
	       pstats->txsinglecollision_g);
	yt_err(pdata, "txmultiplecollision_g %lld\n",
	       pstats->txmultiplecollision_g);
	yt_err(pdata, "txoversize_g %lld\n", pstats->txoversize_g);

	yt_err(pdata, "Dump RX counters:\n");
	yt_err(pdata, "rx_packets %lld\n", pstats->rxframecount_gb);
	yt_err(pdata, "rx_errors %lld\n",
	       pstats->rxframecount_gb - pstats->rxbroadcastframes_g -
	       pstats->rxmulticastframes_g - pstats->rxunicastframes_g);

	yt_err(pdata, "rx_crc_errors %lld\n", pstats->rxcrcerror);
	yt_err(pdata, "rxalignerror %lld\n", pstats->rxalignerror);
	yt_err(pdata, "rxrunterror %lld\n", pstats->rxrunterror);
	yt_err(pdata, "rxjabbererror %lld\n", pstats->rxjabbererror);
	yt_err(pdata, "rx_length_errors %lld\n", pstats->rxlengtherror);
	yt_err(pdata, "rxoutofrangetype %lld\n", pstats->rxoutofrangetype);
	yt_err(pdata, "rx_fifo_errors %lld\n", pstats->rxfifooverflow);
	yt_err(pdata, "rxwatchdogerror %lld\n", pstats->rxwatchdogerror);
	yt_err(pdata, "rxreceiveerrorframe %lld\n",
	       pstats->rxreceiveerrorframe);

	yt_err(pdata, "rxbroadcastframes_g %lld\n",
	       pstats->rxbroadcastframes_g);
	yt_err(pdata, "rxmulticastframes_g %lld\n",
	       pstats->rxmulticastframes_g);
	yt_err(pdata, "rxundersize_g %lld\n", pstats->rxundersize_g);
	yt_err(pdata, "rxoversize_g %lld\n", pstats->rxoversize_g);
	yt_err(pdata, "rxunicastframes_g %lld\n", pstats->rxunicastframes_g);
	yt_err(pdata, "rxcontrolframe_g %lld\n", pstats->rxcontrolframe_g);

	yt_err(pdata, "Dump Extra counters:\n");
	yt_err(pdata, "tx_tso_packets %lld\n", pstats->tx_tso_packets);
	yt_err(pdata, "rx_split_header_packets %lld\n",
	       pstats->rx_split_header_packets);
	yt_err(pdata, "tx_process_stopped %lld\n", pstats->tx_process_stopped);
	yt_err(pdata, "rx_process_stopped %lld\n", pstats->rx_process_stopped);
	yt_err(pdata, "tx_buffer_unavailable %lld\n",
	       pstats->tx_buffer_unavailable);
	yt_err(pdata, "rx_buffer_unavailable %lld\n",
	       pstats->rx_buffer_unavailable);
	yt_err(pdata, "fatal_bus_error %lld\n", pstats->fatal_bus_error);
	yt_err(pdata, "napi_poll_isr %lld\n", pstats->napi_poll_isr);
	yt_err(pdata, "napi_poll_txtimer %lld\n", pstats->napi_poll_txtimer);
	yt_err(pdata, "ephy_poll_timer_cnt %lld\n",
	       pstats->ephy_poll_timer_cnt);
	yt_err(pdata, "mgmt_int_isr %lld\n", pstats->mgmt_int_isr);
}

static void fxgmac_tx_timeout(struct net_device *netdev, unsigned int unused)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);

	fxgmac_dump_state(pdata);
	schedule_work(&pdata->restart_work);
}

#define EFUSE_FISRT_UPDATE_ADDR				255
#define EFUSE_SECOND_UPDATE_ADDR			209
#define EFUSE_MAX_ENTRY					39
#define EFUSE_PATCH_ADDR_START				0
#define EFUSE_PATCH_DATA_START				2
#define EFUSE_PATCH_SIZE				6
#define EFUSE_REGION_A_B_LENGTH				18

static bool fxgmac_efuse_read_data(struct fxgmac_pdata *pdata, u32 offset,
				   u8 *value)
{
	bool ret = false;
	u32 wait = 1000;
	u32 val = 0;

	fxgmac_set_bits(&val, EFUSE_OP_ADDR_POS, EFUSE_OP_ADDR_LEN, offset);
	fxgmac_set_bits(&val, EFUSE_OP_START_POS, EFUSE_OP_START_LEN, 1);
	fxgmac_set_bits(&val, EFUSE_OP_MODE_POS, EFUSE_OP_MODE_LEN,
			EFUSE_OP_MODE_ROW_READ);
	wr32_mem(pdata, val, EFUSE_OP_CTRL_0);

	while (wait--) {
		fsleep(20);
		val = rd32_mem(pdata, EFUSE_OP_CTRL_1);
		if (FXGMAC_GET_BITS(val, EFUSE_OP_DONE_POS,
				    EFUSE_OP_DONE_LEN)) {
			ret = true;
			break;
		}
	}

	if (!ret) {
		yt_err(pdata, "Fail to reading efuse Byte%d\n", offset);
		return ret;
	}

	if (value)
		*value = FXGMAC_GET_BITS(val, EFUSE_OP_RD_DATA_POS,
					 EFUSE_OP_RD_DATA_LEN) & 0xff;

	return ret;
}

static bool fxgmac_efuse_read_index_patch(struct fxgmac_pdata *pdata, u8 index,
					  u32 *offset, u32 *value)
{
	u8 tmp[EFUSE_PATCH_SIZE - EFUSE_PATCH_DATA_START];
	u32 addr, i;
	bool ret;

	if (index >= EFUSE_MAX_ENTRY) {
		yt_err(pdata, "Reading efuse out of range, index %d\n", index);
		return false;
	}

	for (i = EFUSE_PATCH_ADDR_START; i < EFUSE_PATCH_DATA_START; i++) {
		addr = EFUSE_REGION_A_B_LENGTH + index * EFUSE_PATCH_SIZE + i;
		ret = fxgmac_efuse_read_data(pdata, addr,
					     tmp + i - EFUSE_PATCH_ADDR_START);
		if (!ret) {
			yt_err(pdata, "Fail to reading efuse Byte%d\n", addr);
			return ret;
		}
	}
	if (offset) {
		/* tmp[0] is low 8bit date, tmp[1] is high 8bit date */
		*offset = tmp[0] | (tmp[1] << 8);
	}

	for (i = EFUSE_PATCH_DATA_START; i < EFUSE_PATCH_SIZE; i++) {
		addr = EFUSE_REGION_A_B_LENGTH + index * EFUSE_PATCH_SIZE + i;
		ret = fxgmac_efuse_read_data(pdata, addr,
					     tmp + i - EFUSE_PATCH_DATA_START);
		if (!ret) {
			yt_err(pdata, "Fail to reading efuse Byte%d\n", addr);
			return ret;
		}
	}
	if (value) {
		/* tmp[0] is low 8bit date, tmp[1] is low 8bit date
		 * ...  tmp[3] is highest 8bit date
		 */
		*value = tmp[0] | (tmp[1] << 8) | (tmp[2] << 16) |
			 (tmp[3] << 24);
	}

	return ret;
}

static bool fxgmac_efuse_read_mac_subsys(struct fxgmac_pdata *pdata,
					 u8 *mac_addr, u32 *subsys, u32 *revid)
{
	u32 machr = 0, maclr = 0;
	u32 offset = 0, val = 0;
	bool ret = true;
	u8 index;

	for (index = 0;; index++) {
		if (!fxgmac_efuse_read_index_patch(pdata, index, &offset, &val))
			return false;

		if (offset == 0x00)
			break; /* Reach the blank. */

		if (offset == MACA0LR_FROM_EFUSE)
			maclr = val;

		if (offset == MACA0HR_FROM_EFUSE)
			machr = val;

		if (offset == PCI_REVISION_ID && revid)
			*revid = val;

		if (offset == PCI_SUBSYSTEM_VENDOR_ID && subsys)
			*subsys = val;
	}

	if (mac_addr) {
		mac_addr[5] = (u8)(maclr & 0xFF);
		mac_addr[4] = (u8)((maclr >> 8) & 0xFF);
		mac_addr[3] = (u8)((maclr >> 16) & 0xFF);
		mac_addr[2] = (u8)((maclr >> 24) & 0xFF);
		mac_addr[1] = (u8)(machr & 0xFF);
		mac_addr[0] = (u8)((machr >> 8) & 0xFF);
	}

	return ret;
}

static int fxgmac_read_mac_addr(struct fxgmac_pdata *pdata)
{
	u8 default_addr[ETH_ALEN] = { 0, 0x55, 0x7b, 0xb5, 0x7d, 0xf7 };
	struct net_device *netdev = pdata->netdev;
	int ret;

	/* If efuse have mac addr, use it. if not, use static mac address. */
	ret = fxgmac_efuse_read_mac_subsys(pdata, pdata->mac_addr, NULL, NULL);
	if (!ret)
		return -1;

	if (is_zero_ether_addr(pdata->mac_addr))
		/* Use a static mac address for test */
		memcpy(pdata->mac_addr, default_addr, netdev->addr_len);

	return 0;
}

#define FXGMAC_SYSCLOCK 125000000 /* System clock is 125 MHz */

static void fxgmac_default_config(struct fxgmac_pdata *pdata)
{
	pdata->tx_threshold = MTL_TX_THRESHOLD_128;
	pdata->rx_threshold = MTL_RX_THRESHOLD_128;
	pdata->tx_osp_mode = DMA_OSP_ENABLE;
	pdata->tx_sf_mode = MTL_TSF_ENABLE;
	pdata->rx_sf_mode = MTL_RSF_ENABLE;
	pdata->pblx8 = DMA_PBL_X8_ENABLE;
	pdata->tx_pbl = DMA_PBL_16;
	pdata->rx_pbl = DMA_PBL_4;
	pdata->crc_check = 1;
	pdata->tx_pause = 1;	/* Enable tx pause */
	pdata->rx_pause = 1;	/* Enable rx pause */
	pdata->intr_mod = 1;
	pdata->intr_mod_timer = INT_MOD_200_US;
	pdata->vlan_strip = 1;
	pdata->rss = 1;

	pdata->phy_autoeng = AUTONEG_ENABLE;
	pdata->phy_duplex = DUPLEX_FULL;
	pdata->phy_speed = SPEED_1000;
	pdata->phy_link = false;

	pdata->sysclk_rate = FXGMAC_SYSCLOCK;
	pdata->wol = WAKE_MAGIC;

	strscpy(pdata->drv_name, FXGMAC_DRV_NAME, sizeof(pdata->drv_name));
	strscpy(pdata->drv_ver, FXGMAC_DRV_VERSION, sizeof(pdata->drv_ver));
	yt_dbg(pdata, "drv_name:%s, drv_ver:%s\n", FXGMAC_DRV_NAME,
	       FXGMAC_DRV_VERSION);
}

static void fxgmac_get_all_hw_features(struct fxgmac_pdata *pdata)
{
	struct fxgmac_hw_features *hw_feat = &pdata->hw_feat;
	unsigned int mac_hfr0, mac_hfr1, mac_hfr2, mac_hfr3;

	mac_hfr0 = rd32_mac(pdata, MAC_HWF0R);
	mac_hfr1 = rd32_mac(pdata, MAC_HWF1R);
	mac_hfr2 = rd32_mac(pdata, MAC_HWF2R);
	mac_hfr3 = rd32_mac(pdata, MAC_HWF3R);
	memset(hw_feat, 0, sizeof(*hw_feat));
	hw_feat->version = rd32_mac(pdata, MAC_VR);
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "Mac ver=%#x\n", hw_feat->version);

	/* Hardware feature register 0 */
	hw_feat->phyifsel = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_ACTPHYIFSEL_POS,
					    MAC_HWF0R_ACTPHYIFSEL_LEN);
	hw_feat->vlhash = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_VLHASH_POS,
					  MAC_HWF0R_VLHASH_LEN);
	hw_feat->sma = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_SMASEL_POS,
				       MAC_HWF0R_SMASEL_LEN);
	hw_feat->rwk = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_RWKSEL_POS,
				       MAC_HWF0R_RWKSEL_LEN);
	hw_feat->mgk = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_MGKSEL_POS,
				       MAC_HWF0R_MGKSEL_LEN);
	hw_feat->mmc = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_MMCSEL_POS,
				       MAC_HWF0R_MMCSEL_LEN);
	hw_feat->aoe = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_ARPOFFSEL_POS,
				       MAC_HWF0R_ARPOFFSEL_LEN);
	hw_feat->ts = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_TSSEL_POS,
				      MAC_HWF0R_TSSEL_LEN);
	hw_feat->eee = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_EEESEL_POS,
				       MAC_HWF0R_EEESEL_LEN);
	hw_feat->tx_coe = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_TXCOESEL_POS,
					  MAC_HWF0R_TXCOESEL_LEN);
	hw_feat->rx_coe = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_RXCOESEL_POS,
					  MAC_HWF0R_RXCOESEL_LEN);
	hw_feat->addn_mac = FXGMAC_GET_BITS(mac_hfr0,
					    MAC_HWF0R_ADDMACADRSEL_POS,
					    MAC_HWF0R_ADDMACADRSEL_LEN);
	hw_feat->ts_src = FXGMAC_GET_BITS(mac_hfr0, MAC_HWF0R_TSSTSSEL_POS,
					  MAC_HWF0R_TSSTSSEL_LEN);
	hw_feat->sa_vlan_ins = FXGMAC_GET_BITS(mac_hfr0,
					       MAC_HWF0R_SAVLANINS_POS,
					       MAC_HWF0R_SAVLANINS_LEN);

	/* Hardware feature register 1 */
	hw_feat->rx_fifo_size = FXGMAC_GET_BITS(mac_hfr1,
						MAC_HWF1R_RXFIFOSIZE_POS,
						MAC_HWF1R_RXFIFOSIZE_LEN);
	hw_feat->tx_fifo_size = FXGMAC_GET_BITS(mac_hfr1,
						MAC_HWF1R_TXFIFOSIZE_POS,
						MAC_HWF1R_TXFIFOSIZE_LEN);
	hw_feat->adv_ts_hi = FXGMAC_GET_BITS(mac_hfr1,
					     MAC_HWF1R_ADVTHWORD_POS,
					     MAC_HWF1R_ADVTHWORD_LEN);
	hw_feat->dma_width = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_ADDR64_POS,
					     MAC_HWF1R_ADDR64_LEN);
	hw_feat->dcb = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_DCBEN_POS,
				       MAC_HWF1R_DCBEN_LEN);
	hw_feat->sph = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_SPHEN_POS,
				       MAC_HWF1R_SPHEN_LEN);
	hw_feat->tso = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_TSOEN_POS,
				       MAC_HWF1R_TSOEN_LEN);
	hw_feat->dma_debug = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_DBGMEMA_POS,
					     MAC_HWF1R_DBGMEMA_LEN);
	hw_feat->avsel = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_RAVSEL_POS,
					 MAC_HWF1R_RAVSEL_LEN);
	hw_feat->ravsel = FXGMAC_GET_BITS(mac_hfr1, MAC_HWF1R_RAVSEL_POS,
					  MAC_HWF1R_RAVSEL_LEN);
	hw_feat->hash_table_size = FXGMAC_GET_BITS(mac_hfr1,
						   MAC_HWF1R_HASHTBLSZ_POS,
						   MAC_HWF1R_HASHTBLSZ_LEN);
	hw_feat->l3l4_filter_num = FXGMAC_GET_BITS(mac_hfr1,
						   MAC_HWF1R_L3L4FNUM_POS,
						   MAC_HWF1R_L3L4FNUM_LEN);
	hw_feat->tx_q_cnt = FXGMAC_GET_BITS(mac_hfr2, MAC_HWF2R_TXQCNT_POS,
					    MAC_HWF2R_TXQCNT_LEN);
	hw_feat->rx_ch_cnt = FXGMAC_GET_BITS(mac_hfr2, MAC_HWF2R_RXCHCNT_POS,
					     MAC_HWF2R_RXCHCNT_LEN);
	hw_feat->tx_ch_cnt = FXGMAC_GET_BITS(mac_hfr2, MAC_HWF2R_TXCHCNT_POS,
					     MAC_HWF2R_TXCHCNT_LEN);
	hw_feat->pps_out_num = FXGMAC_GET_BITS(mac_hfr2,
					       MAC_HWF2R_PPSOUTNUM_POS,
					       MAC_HWF2R_PPSOUTNUM_LEN);
	hw_feat->aux_snap_num = FXGMAC_GET_BITS(mac_hfr2,
						MAC_HWF2R_AUXSNAPNUM_POS,
						MAC_HWF2R_AUXSNAPNUM_LEN);

	/* Translate the Hash Table size into actual number */
	switch (hw_feat->hash_table_size) {
	case 0:
		break;
	case 1:
		hw_feat->hash_table_size = 64;
		break;
	case 2:
		hw_feat->hash_table_size = 128;
		break;
	case 3:
		hw_feat->hash_table_size = 256;
		break;
	}

	/* Translate the address width setting into actual number */
	switch (hw_feat->dma_width) {
	case 0:
		hw_feat->dma_width = 32;
		break;
	case 1:
		hw_feat->dma_width = 40;
		break;
	case 2:
		hw_feat->dma_width = 48;
		break;
	default:
		hw_feat->dma_width = 32;
	}

	/* The Queue, Channel are zero based so increment them
	 * to get the actual number
	 */
	hw_feat->tx_q_cnt++;
	hw_feat->rx_ch_cnt++;
	hw_feat->tx_ch_cnt++;

	/* HW implement 1 rx fifo, 4 dma channel.  but from software
	 * we see 4 logical queues. hardcode to 4 queues.
	 */
	hw_feat->rx_q_cnt = 4;

	hw_feat->hwfr3 = mac_hfr3;
}

static void fxgmac_print_all_hw_features(struct fxgmac_pdata *pdata)
{
	char *str;

	yt_dbg(pdata, "\n");
	yt_dbg(pdata, "====================================================\n");
	yt_dbg(pdata, "\n");
	yt_dbg(pdata, "HW support following feature\n");
	yt_dbg(pdata, "\n");

	/* HW Feature Register0 */
	yt_dbg(pdata, "VLAN Hash Filter Selected                        : %s\n",
	       pdata->hw_feat.vlhash ? "YES" : "NO");
	yt_dbg(pdata, "SMA (MDIO) Interface                             : %s\n",
	       pdata->hw_feat.sma ? "YES" : "NO");
	yt_dbg(pdata, "PMT Remote Wake-up Packet Enable                 : %s\n",
	       pdata->hw_feat.rwk ? "YES" : "NO");
	yt_dbg(pdata, "PMT Magic Packet Enable                          : %s\n",
	       pdata->hw_feat.mgk ? "YES" : "NO");
	yt_dbg(pdata, "RMON/MMC Module Enable                           : %s\n",
	       pdata->hw_feat.mmc ? "YES" : "NO");
	yt_dbg(pdata, "ARP Offload Enabled                              : %s\n",
	       pdata->hw_feat.aoe ? "YES" : "NO");
	yt_dbg(pdata, "IEEE 1588-2008 Timestamp Enabled                 : %s\n",
	       pdata->hw_feat.ts ? "YES" : "NO");
	yt_dbg(pdata, "Energy Efficient Ethernet Enabled                : %s\n",
	       pdata->hw_feat.eee ? "YES" : "NO");
	yt_dbg(pdata, "Transmit Checksum Offload Enabled                : %s\n",
	       pdata->hw_feat.tx_coe ? "YES" : "NO");
	yt_dbg(pdata, "Receive Checksum Offload Enabled                 : %s\n",
	       pdata->hw_feat.rx_coe ? "YES" : "NO");
	yt_dbg(pdata, "Additional MAC Addresses 1-31 Selected           : %s\n",
	       pdata->hw_feat.addn_mac ? "YES" : "NO");

	switch (pdata->hw_feat.ts_src) {
	case 0:
		str = "RESERVED";
		break;
	case 1:
		str = "INTERNAL";
		break;
	case 2:
		str = "EXTERNAL";
		break;
	case 3:
		str = "BOTH";
		break;
	}
	yt_dbg(pdata, "Timestamp System Time Source                     : %s\n",
	       str);
	yt_dbg(pdata, "Source Address or VLAN Insertion Enable          : %s\n",
	       pdata->hw_feat.sa_vlan_ins ? "YES" : "NO");

	/* HW Feature Register1 */
	switch (pdata->hw_feat.rx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	yt_dbg(pdata, "MTL Receive FIFO Size                            : %s\n",
	       str);

	switch (pdata->hw_feat.tx_fifo_size) {
	case 0:
		str = "128 bytes";
		break;
	case 1:
		str = "256 bytes";
		break;
	case 2:
		str = "512 bytes";
		break;
	case 3:
		str = "1 KBytes";
		break;
	case 4:
		str = "2 KBytes";
		break;
	case 5:
		str = "4 KBytes";
		break;
	case 6:
		str = "8 KBytes";
		break;
	case 7:
		str = "16 KBytes";
		break;
	case 8:
		str = "32 kBytes";
		break;
	case 9:
		str = "64 KBytes";
		break;
	case 10:
		str = "128 KBytes";
		break;
	case 11:
		str = "256 KBytes";
		break;
	default:
		str = "RESERVED";
	}
	yt_dbg(pdata, "MTL Transmit FIFO Size                           : %s\n",
	       str);
	yt_dbg(pdata, "IEEE 1588 High Word Register Enable              : %s\n",
	       pdata->hw_feat.adv_ts_hi ? "YES" : "NO");
	yt_dbg(pdata, "Address width                                    : %u\n",
	       pdata->hw_feat.dma_width);
	yt_dbg(pdata, "DCB Feature Enable                               : %s\n",
	       pdata->hw_feat.dcb ? "YES" : "NO");
	yt_dbg(pdata, "Split Header Feature Enable                      : %s\n",
	       pdata->hw_feat.sph ? "YES" : "NO");
	yt_dbg(pdata, "TCP Segmentation Offload Enable                  : %s\n",
	       pdata->hw_feat.tso ? "YES" : "NO");
	yt_dbg(pdata, "DMA Debug Registers Enabled                      : %s\n",
	       pdata->hw_feat.dma_debug ? "YES" : "NO");
	yt_dbg(pdata, "RSS Feature Enabled                              : %s\n",
	       "YES");
	yt_dbg(pdata, "AV Feature Enabled                               : %s\n",
	       pdata->hw_feat.avsel ? "YES" : "NO");
	yt_dbg(pdata, "Rx Side Only AV Feature Enabled                  : %s\n",
	       (pdata->hw_feat.ravsel ? "YES" : "NO"));
	yt_dbg(pdata, "Hash Table Size                                  : %u\n",
	       pdata->hw_feat.hash_table_size);
	yt_dbg(pdata, "Total number of L3 or L4 Filters                 : %u\n",
	       pdata->hw_feat.l3l4_filter_num);

	/* HW Feature Register2 */
	yt_dbg(pdata, "Number of MTL Receive Queues                     : %u\n",
	       pdata->hw_feat.rx_q_cnt);
	yt_dbg(pdata, "Number of MTL Transmit Queues                    : %u\n",
	       pdata->hw_feat.tx_q_cnt);
	yt_dbg(pdata, "Number of DMA Receive Channels                   : %u\n",
	       pdata->hw_feat.rx_ch_cnt);
	yt_dbg(pdata, "Number of DMA Transmit Channels                  : %u\n",
	       pdata->hw_feat.tx_ch_cnt);

	switch (pdata->hw_feat.pps_out_num) {
	case 0:
		str = "No PPS output";
		break;
	case 1:
		str = "1 PPS output";
		break;
	case 2:
		str = "2 PPS output";
		break;
	case 3:
		str = "3 PPS output";
		break;
	case 4:
		str = "4 PPS output";
		break;
	default:
		str = "RESERVED";
	}
	yt_dbg(pdata, "Number of PPS Outputs                            : %s\n",
	       str);

	switch (pdata->hw_feat.aux_snap_num) {
	case 0:
		str = "No auxiliary input";
		break;
	case 1:
		str = "1 auxiliary input";
		break;
	case 2:
		str = "2 auxiliary input";
		break;
	case 3:
		str = "3 auxiliary input";
		break;
	case 4:
		str = "4 auxiliary input";
		break;
	default:
		str = "RESERVED";
	}
	yt_dbg(pdata, "Number of Auxiliary Snapshot Inputs              : %s\n",
	       str);

	yt_dbg(pdata, "\n");
	yt_dbg(pdata, "====================================================\n");
	yt_dbg(pdata, "\n");
}

static int fxgmac_init(struct fxgmac_pdata *pdata, bool save_private_reg)
{
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct net_device *netdev = pdata->netdev;
	int ret;

	fxgmac_hw_ops_init(hw_ops);	/* Set hw the function pointers */
	fxgmac_default_config(pdata);	/* Set default configuration data */

	/* Set irq, base_addr, MAC address */
	netdev->irq = pdata->dev_irq;
	netdev->base_addr = (unsigned long)pdata->hw_addr;

	ret = fxgmac_read_mac_addr(pdata);
	if (ret) {
		yt_err(pdata, "fxgmac_read_mac_addr err:%d\n", ret);
		return ret;
	}
	eth_hw_addr_set(netdev, pdata->mac_addr);

	if (save_private_reg)
		hw_ops->save_nonstick_reg(pdata);

	hw_ops->exit(pdata);	/* Reset here to get hw features correctly */

	/* Populate the hardware features */
	fxgmac_get_all_hw_features(pdata);
	fxgmac_print_all_hw_features(pdata);

	/* Set the DMA mask */
	ret = dma_set_mask_and_coherent(pdata->dev,
					DMA_BIT_MASK(pdata->hw_feat.dma_width));
	if (ret) {
		yt_err(pdata, "dma_set_mask_and_coherent err:%d\n", ret);
		return ret;
	}
	if (pdata->int_flags & FXGMAC_FLAG_LEGACY_ENABLED) {
		/* We should disable msi and msix here when we use legacy
		 * interrupt,for two reasons:
		 * 1. Exit will restore msi and msix config regisiter,
		 * that may enable them.
		 * 2. When the driver that uses the msix interrupt by default
		 * is compiled into the OS, uninstall the driver through rmmod,
		 * and then install the driver that uses the legacy interrupt,
		 * at which time the msix enable will be turned on again by
		 * default after waking up from S4 on some
		 * platform. such as UOS platform.
		 */
		pci_disable_msi(to_pci_dev(pdata->dev));
		pci_disable_msix(to_pci_dev(pdata->dev));
	}

	BUILD_BUG_ON_NOT_POWER_OF_2(FXGMAC_TX_DESC_CNT);
	pdata->tx_desc_count = FXGMAC_TX_DESC_CNT;
	BUILD_BUG_ON_NOT_POWER_OF_2(FXGMAC_RX_DESC_CNT);
	pdata->rx_desc_count = FXGMAC_RX_DESC_CNT;

	ret = netif_set_real_num_tx_queues(netdev, FXGMAC_TX_1_Q);
	yt_dbg(pdata,
	       "num_online_cpus:%u, tx:ch_cnt:%u, q_cnt:%u, ring_count:%u\n",
	       num_online_cpus(), pdata->hw_feat.tx_ch_cnt,
	       pdata->hw_feat.tx_q_cnt, FXGMAC_TX_1_RING);
	if (ret) {
		yt_err(pdata, "error setting real tx queue count\n");
		return ret;
	}

	pdata->rx_ring_count = min_t(unsigned int,
				     netif_get_num_default_rss_queues(),
				     pdata->hw_feat.rx_ch_cnt);
	pdata->rx_ring_count = min_t(unsigned int, pdata->rx_ring_count,
				     pdata->hw_feat.rx_q_cnt);
	pdata->rx_q_count = pdata->rx_ring_count;
	ret = netif_set_real_num_rx_queues(netdev, pdata->rx_q_count);
	if (ret) {
		yt_err(pdata, "error setting real rx queue count\n");
		return ret;
	}

	pdata->channel_count =
		max_t(unsigned int, FXGMAC_TX_1_RING, pdata->rx_ring_count);

	yt_dbg(pdata,
	       "default rss queues:%u, rx:ch_cnt:%u, q_cnt:%u, ring_count:%u, channel_count:%u, netdev tx channel_num=%u\n",
	       netif_get_num_default_rss_queues(), pdata->hw_feat.rx_ch_cnt,
	       pdata->hw_feat.rx_q_cnt, pdata->rx_ring_count,
	       pdata->channel_count, netdev->real_num_tx_queues);

	/* Initialize RSS hash key and lookup table */
	netdev_rss_key_fill(pdata->rss_key, sizeof(pdata->rss_key));

	for (u32 i = 0; i < FXGMAC_RSS_MAX_TABLE_SIZE; i++) {
		fxgmac_set_bits(&pdata->rss_table[i], MAC_RSSDR_DMCH_POS,
				MAC_RSSDR_DMCH_LEN, i % pdata->rx_ring_count);
	}

	pdata->rss_options |= FXGMAC_RSS_IP4TE | FXGMAC_RSS_TCP4TE |
			      FXGMAC_RSS_UDP4TE;

	netdev->min_mtu = ETH_MIN_MTU;
	netdev->max_mtu =
		FXGMAC_JUMBO_PACKET_MTU + (ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN);

	yt_dbg(pdata, "rss_options:0x%x\n", pdata->rss_options);

	/* Set device operations */
	netdev->netdev_ops = fxgmac_get_netdev_ops();
	netdev->ethtool_ops = fxgmac_get_ethtool_ops();

	/* Set device features */
	if (pdata->hw_feat.tso) {
		netdev->hw_features = NETIF_F_TSO;
		netdev->hw_features |= NETIF_F_TSO6;
		netdev->hw_features |= NETIF_F_SG;
		netdev->hw_features |= NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	} else if (pdata->hw_feat.tx_coe) {
		netdev->hw_features = NETIF_F_IP_CSUM;
		netdev->hw_features |= NETIF_F_IPV6_CSUM;
	}

	if (pdata->hw_feat.rx_coe) {
		netdev->hw_features |= NETIF_F_RXCSUM;
		netdev->hw_features |= NETIF_F_GRO;
	}

	netdev->hw_features |= NETIF_F_RXHASH;
	netdev->vlan_features |= netdev->hw_features;

	netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_RX;

	if (pdata->hw_feat.sa_vlan_ins)
		netdev->hw_features |= NETIF_F_HW_VLAN_CTAG_TX;

	netdev->features |= netdev->hw_features;
	pdata->netdev_features = netdev->features;

	netdev->priv_flags |= IFF_UNICAST_FLT;
	netdev->watchdog_timeo = msecs_to_jiffies(5000);

#define NIC_MAX_TCP_OFFLOAD_SIZE 7300

	netif_set_tso_max_size(netdev, NIC_MAX_TCP_OFFLOAD_SIZE);

/* Default coalescing parameters */
#define FXGMAC_INIT_DMA_TX_USECS INT_MOD_200_US
#define FXGMAC_INIT_DMA_TX_FRAMES 25
#define FXGMAC_INIT_DMA_RX_USECS INT_MOD_200_US
#define FXGMAC_INIT_DMA_RX_FRAMES 25

	/* Tx coalesce parameters initialization */
	pdata->tx_usecs = FXGMAC_INIT_DMA_TX_USECS;
	pdata->tx_frames = FXGMAC_INIT_DMA_TX_FRAMES;

	/* Rx coalesce parameters initialization */
	pdata->rx_riwt = hw_ops->usec_to_riwt(pdata, FXGMAC_INIT_DMA_RX_USECS);
	pdata->rx_usecs = FXGMAC_INIT_DMA_RX_USECS;
	pdata->rx_frames = FXGMAC_INIT_DMA_RX_FRAMES;

	mutex_init(&pdata->mutex);
	yt_dbg(pdata, "%s ok.\n", __func__);

	return 0;
}

#ifdef CONFIG_PCI_MSI
static void fxgmac_init_interrupt_scheme(struct fxgmac_pdata *pdata)
{
	struct pci_dev *pdev = to_pci_dev(pdata->dev);
	u32 i, *flags = &pdata->int_flags;
	int vectors, rc, req_vectors;

	/* Since we have 4 channels, we must ensure the number of cpu core > 4
	 * otherwise, just roll back to legacy
	 *  0-3 for rx, 4 for tx, 5 for misc
	 */
	vectors = num_online_cpus();
	if (vectors < FXGMAC_MAX_DMA_RX_CHANNELS)
		goto enable_msi_interrupt;

	req_vectors = FXGMAC_MSIX_INT_NUMS;
	pdata->msix_entries =
		kcalloc(req_vectors, sizeof(struct msix_entry), GFP_KERNEL);
	if (!pdata->msix_entries)
		goto enable_msi_interrupt;

	for (i = 0; i < req_vectors; i++)
		pdata->msix_entries[i].entry = i;

	rc = pci_enable_msix_exact(pdev, pdata->msix_entries, req_vectors);
	if (rc < 0) {
		yt_err(pdata, "enable MSIx err, clear msix entries.\n");
		/* Roll back to msi */
		kfree(pdata->msix_entries);
		pdata->msix_entries = NULL;
		req_vectors = 0;
		goto enable_msi_interrupt;
	}

	yt_dbg(pdata, "enable MSIx ok, cpu=%d,vectors=%d.\n", vectors,
	       req_vectors);
	fxgmac_set_bits(flags, FXGMAC_FLAG_INTERRUPT_POS,
			FXGMAC_FLAG_INTERRUPT_LEN,
			FXGMAC_FLAG_MSIX_ENABLED);
	pdata->per_channel_irq = 1;
	pdata->misc_irq = pdata->msix_entries[MSI_ID_PHY_OTHER].vector;
	return;

enable_msi_interrupt:
	rc = pci_enable_msi(pdev);
	if (rc < 0) {
		fxgmac_set_bits(flags, FXGMAC_FLAG_INTERRUPT_POS,
				FXGMAC_FLAG_INTERRUPT_LEN,
				FXGMAC_FLAG_LEGACY_ENABLED);
		yt_err(pdata, "MSI err, rollback to LEGACY.\n");
	} else {
		fxgmac_set_bits(flags, FXGMAC_FLAG_INTERRUPT_POS,
				FXGMAC_FLAG_INTERRUPT_LEN,
				FXGMAC_FLAG_MSI_ENABLED);
		pdata->dev_irq = pdev->irq;
		yt_dbg(pdata, "enable MSI ok, cpu=%d, irq=%d.\n", vectors,
		       pdev->irq);
	}
}
#endif

static int fxgmac_mdio_write_reg(struct mii_bus *mii_bus, int phyaddr,
				 int phyreg, u16 val)
{
	struct fxgmac_pdata *yt = mii_bus->priv;

	if (phyaddr > 0)
		return -ENODEV;

	return yt->hw_ops.write_phy_reg(yt, phyreg, val);
}

static int fxgmac_mdio_read_reg(struct mii_bus *mii_bus, int phyaddr, int phyreg)
{
	struct fxgmac_pdata *yt = mii_bus->priv;

	if (phyaddr > 0)
		return -ENODEV;

	return  yt->hw_ops.read_phy_reg(yt, phyreg);
}

static int fxgmac_mdio_register(struct fxgmac_pdata *pdata)
{
	struct pci_dev *pdev = to_pci_dev(pdata->dev);
	struct phy_device *phydev;
	struct mii_bus *new_bus;
	int ret;

	new_bus = devm_mdiobus_alloc(&pdev->dev);
	if (!new_bus) {
		yt_err(pdata, "devm_mdiobus_alloc err\n");
		return -ENOMEM;
	}

	new_bus->name = "yt6801";
	new_bus->priv = pdata;
	new_bus->parent = &pdev->dev;
	new_bus->irq[0] = PHY_MAC_INTERRUPT;
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "yt6801-%x-%x",
		 pci_domain_nr(pdev->bus), pci_dev_id(pdev));

	new_bus->read = fxgmac_mdio_read_reg;
	new_bus->write = fxgmac_mdio_write_reg;

	ret = devm_mdiobus_register(&pdev->dev, new_bus);
	if (ret < 0) {
		yt_err(pdata, "devm_mdiobus_register err:%x\n", ret);
		return ret;
	}

	phydev = mdiobus_get_phy(new_bus, 0);
	if (!phydev) {
		yt_err(pdata, "mdiobus_get_phy err\n");
		return -ENODEV;
	}

	pdata->phydev = phydev;
	phydev->mac_managed_pm = true;
	phy_support_asym_pause(phydev);

	/* PHY will be woken up in rtl_open() */
	phy_suspend(phydev);

	return 0;
}

int fxgmac_drv_probe(struct device *dev, struct fxgmac_resources *res)
{
	struct fxgmac_hw_ops *hw_ops;
	struct fxgmac_pdata *pdata;
	struct net_device *netdev;
	int ret;

	netdev = alloc_etherdev_mq(sizeof(struct fxgmac_pdata),
				   FXGMAC_MAX_DMA_RX_CHANNELS);
	if (!netdev) {
		dev_err(dev, "alloc_etherdev_mq err\n");
		return -ENOMEM;
	}

	SET_NETDEV_DEV(netdev, dev);
	pdata = netdev_priv(netdev);

	pdata->dev = dev;
	pdata->netdev = netdev;
	pdata->dev_irq = res->irq;
	pdata->hw_addr = res->addr;
	pdata->msg_enable = NETIF_MSG_DRV;
	pdata->dev_state = FXGMAC_DEV_PROBE;

	/* Default to legacy interrupt */
	fxgmac_set_bits(&pdata->int_flags, FXGMAC_FLAG_INTERRUPT_POS,
			FXGMAC_FLAG_INTERRUPT_LEN, FXGMAC_FLAG_LEGACY_ENABLED);
	pdata->misc_irq = pdata->dev_irq;
	pci_set_drvdata(to_pci_dev(pdata->dev), pdata);

#ifdef CONFIG_PCI_MSI
	fxgmac_init_interrupt_scheme(pdata);
#endif

	ret = fxgmac_init(pdata, true);
	if (ret < 0) {
		yt_err(pdata, "fxgmac_init err:%d\n", ret);
		goto err_free_netdev;
	}

	hw_ops = &pdata->hw_ops;
	hw_ops->reset_phy(pdata);
	hw_ops->release_phy(pdata);
	ret = fxgmac_mdio_register(pdata);
	if (ret < 0) {
		yt_err(pdata, "fxgmac_mdio_register err:%d\n", ret);
		goto err_free_netdev;
	}

	netif_carrier_off(netdev);
	ret = register_netdev(netdev);
	if (ret) {
		yt_err(pdata, "register_netdev err:%d\n", ret);
		goto err_free_netdev;
	}
	if (netif_msg_drv(pdata))
		yt_dbg(pdata, "%s, netdev num_tx_q=%u\n", __func__,
		       netdev->real_num_tx_queues);

	return 0;

err_free_netdev:
	free_netdev(netdev);
	return ret;
}

void fxgmac_dbg_pkt(struct fxgmac_pdata *pdata, struct sk_buff *skb, bool tx_rx)
{
	struct ethhdr *eth = (struct ethhdr *)skb->data;
	unsigned char buffer[128];

	yt_dbg(pdata, "\n************** SKB dump ****************\n");
	yt_dbg(pdata, "%s, packet of %d bytes\n", (tx_rx ? "TX" : "RX"),
	       skb->len);
	yt_dbg(pdata, "Dst MAC addr: %pM\n", eth->h_dest);
	yt_dbg(pdata, "Src MAC addr: %pM\n", eth->h_source);
	yt_dbg(pdata, "Protocol: %#06x\n", ntohs(eth->h_proto));

	for (u32 i = 0; i < skb->len; i += 32) {
		unsigned int len = min(skb->len - i, 32U);

		hex_dump_to_buffer(&skb->data[i], len, 32, 1, buffer,
				   sizeof(buffer), false);
		yt_dbg(pdata, "  %#06x: %s\n", i, buffer);
	}

	yt_dbg(pdata, "\n************** SKB dump ****************\n");
}

static netdev_tx_t fxgmac_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_pkt_info *tx_pkt_info;
	struct fxgmac_channel *channel;
	struct fxgmac_hw_ops *hw_ops;
	struct netdev_queue *txq;
	struct fxgmac_ring *ring;
	int ret;

	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata, "%s, skb->len=%d,q=%d\n", __func__, skb->len,
		       skb->queue_mapping);

	channel = pdata->channel_head + skb->queue_mapping;
	txq = netdev_get_tx_queue(netdev, channel->queue_index);
	ring = channel->tx_ring;
	tx_pkt_info = &ring->pkt_info;

	hw_ops = &pdata->hw_ops;

	if (skb->len == 0) {
		yt_err(pdata, "empty skb received from stack\n");
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Prepare preliminary packet info for TX */
	memset(tx_pkt_info, 0, sizeof(*tx_pkt_info));
	fxgmac_prep_tx_pkt(pdata, ring, skb, tx_pkt_info);

	/* Check that there are enough descriptors available */
	ret = fxgmac_maybe_stop_tx_queue(channel, ring,
					 tx_pkt_info->desc_count);
	if (ret == NETDEV_TX_BUSY)
		return ret;

	ret = fxgmac_prep_tso(pdata, skb, tx_pkt_info);
	if (ret < 0) {
		yt_err(pdata, "error processing TSO packet\n");
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	fxgmac_prep_vlan(skb, tx_pkt_info);

	if (!fxgmac_tx_skb_map(channel, skb)) {
		dev_kfree_skb_any(skb);
		yt_err(pdata, "xmit, map tx skb err\n");
		return NETDEV_TX_OK;
	}

	/* Report on the actual number of bytes (to be) sent */
	netdev_tx_sent_queue(txq, tx_pkt_info->tx_bytes);
	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata, "xmit,before hw_xmit, byte len=%d\n",
		       tx_pkt_info->tx_bytes);

	/* Configure required descriptor fields for transmission */
	hw_ops->dev_xmit(channel);

	if (netif_msg_pktdata(pdata))
		fxgmac_dbg_pkt(pdata, skb, true);

	/* Stop the queue in advance if there may not be enough descriptors */
	fxgmac_maybe_stop_tx_queue(channel, ring, FXGMAC_TX_MAX_DESC_NR);

	return NETDEV_TX_OK;
}

static void fxgmac_get_stats64(struct net_device *netdev,
			       struct rtnl_link_stats64 *s)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_stats *pstats = &pdata->stats;

	if (test_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate))
		return;

	pdata->hw_ops.read_mmc_stats(pdata);

	s->rx_packets = pstats->rxframecount_gb;
	s->rx_bytes = pstats->rxoctetcount_gb;
	s->rx_errors = pstats->rxframecount_gb - pstats->rxbroadcastframes_g -
		       pstats->rxmulticastframes_g - pstats->rxunicastframes_g;

	s->rx_length_errors = pstats->rxlengtherror;
	s->rx_crc_errors = pstats->rxcrcerror;
	s->rx_fifo_errors = pstats->rxfifooverflow;

	s->tx_packets = pstats->txframecount_gb;
	s->tx_bytes = pstats->txoctetcount_gb;
	s->tx_errors = pstats->txframecount_gb - pstats->txframecount_g;
	s->tx_dropped = netdev->stats.tx_dropped;
}

static int fxgmac_set_mac_address(struct net_device *netdev, void *addr)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	eth_hw_addr_set(netdev, saddr->sa_data);
	memcpy(pdata->mac_addr, saddr->sa_data, netdev->addr_len);
	hw_ops->set_mac_address(pdata, saddr->sa_data);
	hw_ops->set_mac_hash(pdata);

	yt_dbg(pdata, "fxgmac,set mac addr to %pM\n", netdev->dev_addr);

	return 0;
}

static int fxgmac_change_mtu(struct net_device *netdev, int mtu)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	int old_mtu = netdev->mtu;
	int ret;

	mutex_lock(&pdata->mutex);
	fxgmac_stop(pdata);
	fxgmac_free_tx_data(pdata);

	/* We must unmap rx desc's dma before we change rx_buf_size.
	 * Becaues the size of the unmapped DMA is set according to rx_buf_size
	 */
	fxgmac_free_rx_data(pdata);
	pdata->jumbo = mtu > ETH_DATA_LEN ? 1 : 0;
	ret = fxgmac_calc_rx_buf_size(pdata, mtu);
	if (ret < 0)
		return ret;

	pdata->rx_buf_size = ret;
	netdev->mtu = mtu;

	if (netif_running(netdev))
		fxgmac_start(pdata);

	netdev_update_features(netdev);

	mutex_unlock(&pdata->mutex);

	yt_dbg(pdata, "fxgmac,set MTU from %d to %d. min, max=(%d,%d)\n",
	       old_mtu, netdev->mtu, netdev->min_mtu, netdev->max_mtu);

	return 0;
}

static int fxgmac_vlan_rx_add_vid(struct net_device *netdev, __be16 proto,
				  u16 vid)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	set_bit(vid, pdata->active_vlans);
	hw_ops->update_vlan_hash_table(pdata);

	yt_dbg(pdata, "fxgmac,add rx vlan %d\n", vid);

	return 0;
}

static int fxgmac_vlan_rx_kill_vid(struct net_device *netdev, __be16 proto,
				   u16 vid)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	clear_bit(vid, pdata->active_vlans);
	hw_ops->update_vlan_hash_table(pdata);

	yt_dbg(pdata, "fxgmac,del rx vlan %d\n", vid);

	return 0;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void fxgmac_poll_controller(struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_channel *channel;

	if (pdata->per_channel_irq) {
		channel = pdata->channel_head;
		for (u32 i = 0; i < pdata->channel_count; i++, channel++)
			fxgmac_dma_isr(channel->dma_irq_rx, channel);
	} else {
		disable_irq(pdata->dev_irq);
		fxgmac_isr(pdata->dev_irq, pdata);
		enable_irq(pdata->dev_irq);
	}
}
#endif /* CONFIG_NET_POLL_CONTROLLER */

static netdev_features_t fxgmac_fix_features(struct net_device *netdev,
					     netdev_features_t features)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	u32 fifo_size;

	fifo_size = pdata->hw_ops.calculate_max_checksum_size(pdata);
	if (netdev->mtu > fifo_size) {
		features &= ~NETIF_F_IP_CSUM;
		features &= ~NETIF_F_IPV6_CSUM;
	}

	return features;
}

static int fxgmac_set_features(struct net_device *netdev,
			       netdev_features_t features)
{
	netdev_features_t rxhash, rxcsum, rxvlan, rxvlan_filter, tso;
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops;

	hw_ops = &pdata->hw_ops;
	rxhash = pdata->netdev_features & NETIF_F_RXHASH;
	rxcsum = pdata->netdev_features & NETIF_F_RXCSUM;
	rxvlan = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_RX;
	rxvlan_filter = pdata->netdev_features & NETIF_F_HW_VLAN_CTAG_FILTER;
	tso = pdata->netdev_features & (NETIF_F_TSO | NETIF_F_TSO6);

	if ((features & (NETIF_F_TSO | NETIF_F_TSO6)) && !tso) {
		yt_dbg(pdata, "enable tso.\n");
		pdata->hw_feat.tso = 1;
		hw_ops->config_tso(pdata);
	} else if (!(features & (NETIF_F_TSO | NETIF_F_TSO6)) && tso) {
		yt_dbg(pdata, "disable tso.\n");
		pdata->hw_feat.tso = 0;
		hw_ops->config_tso(pdata);
	}

	if ((features & NETIF_F_RXHASH) && !rxhash)
		hw_ops->enable_rss(pdata);
	else if (!(features & NETIF_F_RXHASH) && rxhash)
		hw_ops->disable_rss(pdata);

	if ((features & NETIF_F_RXCSUM) && !rxcsum)
		hw_ops->enable_rx_csum(pdata);
	else if (!(features & NETIF_F_RXCSUM) && rxcsum)
		hw_ops->disable_rx_csum(pdata);

	if ((features & NETIF_F_HW_VLAN_CTAG_RX) && !rxvlan)
		hw_ops->enable_rx_vlan_stripping(pdata);
	else if (!(features & NETIF_F_HW_VLAN_CTAG_RX) && rxvlan)
		hw_ops->disable_rx_vlan_stripping(pdata);

	if ((features & NETIF_F_HW_VLAN_CTAG_FILTER) && !rxvlan_filter)
		hw_ops->enable_rx_vlan_filtering(pdata);
	else if (!(features & NETIF_F_HW_VLAN_CTAG_FILTER) && rxvlan_filter)
		hw_ops->disable_rx_vlan_filtering(pdata);

	pdata->netdev_features = features;

	yt_dbg(pdata, "fxgmac,set features done,%llx\n", (u64)features);
	return 0;
}

static void fxgmac_set_rx_mode(struct net_device *netdev)
{
	struct fxgmac_pdata *pdata = netdev_priv(netdev);
	struct fxgmac_hw_ops *hw_ops = &pdata->hw_ops;

	hw_ops->config_rx_mode(pdata);
}

static const struct net_device_ops fxgmac_netdev_ops = {
	.ndo_open		= fxgmac_open,
	.ndo_stop		= fxgmac_close,
	.ndo_start_xmit		= fxgmac_xmit,
	.ndo_tx_timeout		= fxgmac_tx_timeout,
	.ndo_get_stats64	= fxgmac_get_stats64,
	.ndo_change_mtu		= fxgmac_change_mtu,
	.ndo_set_mac_address	= fxgmac_set_mac_address,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_vlan_rx_add_vid	= fxgmac_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= fxgmac_vlan_rx_kill_vid,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= fxgmac_poll_controller,
#endif
	.ndo_set_features	= fxgmac_set_features,
	.ndo_fix_features	= fxgmac_fix_features,
	.ndo_set_rx_mode	= fxgmac_set_rx_mode,
};

const struct net_device_ops *fxgmac_get_netdev_ops(void)
{
	return &fxgmac_netdev_ops;
}

static void fxgmac_rx_refresh(struct fxgmac_channel *channel)
{
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_ring *ring = channel->rx_ring;
	struct fxgmac_desc_data *desc_data;

	while (ring->dirty != ring->cur) {
		desc_data = FXGMAC_GET_DESC_DATA(ring, ring->dirty);

		/* Reset desc_data values */
		fxgmac_desc_data_unmap(pdata, desc_data);

		if (fxgmac_rx_buffe_map(pdata, ring, desc_data))
			break;

		fxgmac_desc_rx_reset(desc_data);
		ring->dirty =
			FXGMAC_GET_ENTRY(ring->dirty, ring->dma_desc_count);
	}

	/* Make sure everything is written before the register write */
	wmb();

	/* Update the Rx Tail Pointer Register with address of
	 * the last cleaned entry
	 */
	desc_data =
		FXGMAC_GET_DESC_DATA(ring,
				     (ring->dirty - 1) &
				     (ring->dma_desc_count - 1));
	wr32_mac(pdata, lower_32_bits(desc_data->dma_desc_addr),
		 FXGMAC_DMA_REG(channel, DMA_CH_RDTR_LO));
}

static struct sk_buff *fxgmac_create_skb(struct fxgmac_pdata *pdata,
					 struct napi_struct *napi,
					 struct fxgmac_desc_data *desc_data,
					 unsigned int len)
{
	unsigned int copy_len;
	struct sk_buff *skb;
	u8 *packet;

	skb = napi_alloc_skb(napi, desc_data->rx.hdr.dma_len);
	if (!skb)
		return NULL;

	/* Start with the header buffer which may contain just the header
	 * or the header plus data
	 */
	dma_sync_single_range_for_cpu(pdata->dev, desc_data->rx.hdr.dma_base,
				      desc_data->rx.hdr.dma_off,
				      desc_data->rx.hdr.dma_len,
				      DMA_FROM_DEVICE);

	packet = page_address(desc_data->rx.hdr.pa.pages) +
		 desc_data->rx.hdr.pa.pages_offset;
	copy_len = min(desc_data->rx.hdr.dma_len, len);
	skb_copy_to_linear_data(skb, packet, copy_len);
	skb_put(skb, copy_len);

	return skb;
}

static int fxgmac_tx_poll(struct fxgmac_channel *channel)
{
	struct fxgmac_pdata *pdata = channel->pdata;
	unsigned int cur, tx_packets = 0, tx_bytes = 0;
	struct fxgmac_ring *ring = channel->tx_ring;
	struct net_device *netdev = pdata->netdev;
	struct fxgmac_desc_data *desc_data;
	struct fxgmac_dma_desc *dma_desc;
	struct netdev_queue *txq;
	int processed = 0;

	/* Nothing to do if there isn't a Tx ring for this channel */
	if (!ring) {
		if (netif_msg_tx_done(pdata) &&
		    channel->queue_index < FXGMAC_TX_1_Q)
			yt_dbg(pdata, "%s, null point to ring %d\n", __func__,
			       channel->queue_index);
		return 0;
	}
	if (ring->cur != ring->dirty && (netif_msg_tx_done(pdata)))
		yt_dbg(pdata, "%s, ring_cur=%d,ring_dirty=%d,qIdx=%d\n",
		       __func__, ring->cur, ring->dirty, channel->queue_index);

	cur = ring->cur;

	/* Be sure we get ring->cur before accessing descriptor data */
	smp_rmb();

	txq = netdev_get_tx_queue(netdev, channel->queue_index);
	while (ring->dirty != cur) {
		desc_data = FXGMAC_GET_DESC_DATA(ring, ring->dirty);
		dma_desc = desc_data->dma_desc;

		if (!fxgmac_is_tx_complete(dma_desc))
			break;

		/* Make sure descriptor fields are read after reading
		 * the OWN bit
		 */
		dma_rmb();

		if (netif_msg_tx_done(pdata))
			fxgmac_dump_tx_desc(pdata, ring, ring->dirty, 1, 0);

		if (fxgmac_is_last_desc(dma_desc)) {
			tx_packets += desc_data->tx.packets;
			tx_bytes += desc_data->tx.bytes;
		}

		/* Free the SKB and reset the descriptor for re-use */
		fxgmac_desc_data_unmap(pdata, desc_data);
		fxgmac_desc_tx_reset(desc_data);

		processed++;
		ring->dirty =
			FXGMAC_GET_ENTRY(ring->dirty, ring->dma_desc_count);
	}

	if (!processed)
		return 0;

	netdev_tx_completed_queue(txq, tx_packets, tx_bytes);

	/* Make sure ownership is written to the descriptor */
	smp_wmb();
	if (ring->tx.queue_stopped == 1 &&
	    (fxgmac_desc_tx_avail(ring) > FXGMAC_TX_DESC_MIN_FREE)) {
		ring->tx.queue_stopped = 0;
		netif_tx_wake_queue(txq);
	}

	if (netif_msg_tx_done(pdata))
		yt_dbg(pdata, "%s, processed=%d\n", __func__, processed);

	return processed;
}

static int fxgmac_one_poll_tx(struct napi_struct *napi, int budget)
{
	struct fxgmac_channel *channel =
		container_of(napi, struct fxgmac_channel, napi_tx);
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_hw_ops *hw_ops;
	int ret;

	hw_ops = &pdata->hw_ops;
	ret = fxgmac_tx_poll(channel);
	if (napi_complete_done(napi, 0))
		hw_ops->enable_msix_one_irq(pdata, MSI_ID_TXQ0);

	return ret;
}

static int fxgmac_rx_poll(struct fxgmac_channel *channel, int budget)
{
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_ring *ring = channel->rx_ring;
	struct net_device *netdev = pdata->netdev;
	u32 context_next, context, incomplete;
	struct fxgmac_desc_data *desc_data;
	struct fxgmac_pkt_info *pkt_info;
	struct fxgmac_hw_ops *hw_ops;
	struct napi_struct *napi;
	u32 len, attr, max_len;
	int packet_count = 0;

	struct sk_buff *skb;

	/* Nothing to do if there isn't a Rx ring for this channel */
	if (!ring)
		return 0;

	incomplete = 0;
	context_next = 0;
	napi = (pdata->per_channel_irq) ? &channel->napi_rx : &pdata->napi;
	pkt_info = &ring->pkt_info;

	hw_ops = &pdata->hw_ops;

	while (packet_count < budget) {
		memset(pkt_info, 0, sizeof(*pkt_info));
		skb = NULL;
		len = 0;

read_again:
		desc_data = FXGMAC_GET_DESC_DATA(ring, ring->cur);

		if (fxgmac_desc_rx_dirty(ring) > FXGMAC_RX_DESC_MAX_DIRTY)
			fxgmac_rx_refresh(channel);

		if (hw_ops->dev_read(channel))
			break;

		ring->cur = FXGMAC_GET_ENTRY(ring->cur, ring->dma_desc_count);
		attr = pkt_info->attributes;
		incomplete = FXGMAC_GET_BITS(attr, RX_PKT_ATTR_INCOMPLETE_POS,
					     RX_PKT_ATTR_INCOMPLETE_LEN);
		context_next = FXGMAC_GET_BITS(attr,
					       RX_PKT_ATTR_CONTEXT_NEXT_POS,
					       RX_PKT_ATTR_CONTEXT_NEXT_LEN);
		context = FXGMAC_GET_BITS(attr, RX_PKT_ATTR_CONTEXT_POS,
					  RX_PKT_ATTR_CONTEXT_LEN);

		if (incomplete || context_next)
			goto read_again;

		if (pkt_info->errors) {
			yt_err(pdata, "error in received packet\n");
			dev_kfree_skb(skb);
			pdata->netdev->stats.rx_dropped++;
			goto next_packet;
		}

		if (!context) {
			len = desc_data->rx.len;
			if (len == 0) {
				if (net_ratelimit())
					yt_err(pdata,
					       "A packet of length 0 was received\n");
				pdata->netdev->stats.rx_length_errors++;
				pdata->netdev->stats.rx_dropped++;
				goto next_packet;
			}

			if (len && !skb) {
				skb = fxgmac_create_skb(pdata, napi, desc_data,
							len);
				if (unlikely(!skb)) {
					if (net_ratelimit())
						yt_err(pdata,
						       "create skb err\n");
					pdata->netdev->stats.rx_dropped++;
					goto next_packet;
				}
			}
			max_len = netdev->mtu + ETH_HLEN;
			if (!(netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
			    skb->protocol == htons(ETH_P_8021Q))
				max_len += VLAN_HLEN;

			if (len > max_len) {
				if (net_ratelimit())
					yt_err(pdata,
					       "len %d larger than max size %d\n",
					       len, max_len);
				pdata->netdev->stats.rx_length_errors++;
				pdata->netdev->stats.rx_dropped++;
				dev_kfree_skb(skb);
				goto next_packet;
			}
		}

		if (!skb) {
			pdata->netdev->stats.rx_dropped++;
			goto next_packet;
		}

		if (netif_msg_pktdata(pdata))
			fxgmac_dbg_pkt(pdata, skb, false);

		skb_checksum_none_assert(skb);
		if (netdev->features & NETIF_F_RXCSUM)
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		if (FXGMAC_GET_BITS(attr, RX_PKT_ATTR_VLAN_CTAG_POS,
				    RX_PKT_ATTR_VLAN_CTAG_LEN)) {
			__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q),
					       pkt_info->vlan_ctag);
			pdata->stats.rx_vlan_packets++;
		}

		if (FXGMAC_GET_BITS(attr, RX_PKT_ATTR_RSS_HASH_POS,
				    RX_PKT_ATTR_RSS_HASH_LEN))
			skb_set_hash(skb, pkt_info->rss_hash,
				     pkt_info->rss_hash_type);

		skb->dev = netdev;
		skb->protocol = eth_type_trans(skb, netdev);
		skb_record_rx_queue(skb, channel->queue_index);

		napi_gro_receive(napi, skb);

next_packet:
		packet_count++;
		pdata->netdev->stats.rx_packets++;
		pdata->netdev->stats.rx_bytes += len;
	}

	return packet_count;
}

static int fxgmac_one_poll_rx(struct napi_struct *napi, int budget)
{
	struct fxgmac_channel *channel =
		container_of(napi, struct fxgmac_channel, napi_rx);
	struct fxgmac_pdata *pdata = channel->pdata;
	struct fxgmac_hw_ops *hw_ops;
	int processed = 0;

	hw_ops = &pdata->hw_ops;
	processed = fxgmac_rx_poll(channel, budget);
	if (processed < budget) {
		if (napi_complete_done(napi, processed)) {
			hw_ops->enable_msix_one_irq(pdata,
						    channel->queue_index);
		}
	}

	return processed;
}

static int fxgmac_all_poll(struct napi_struct *napi, int budget)
{
	struct fxgmac_pdata *pdata =
		container_of(napi, struct fxgmac_pdata, napi);
	struct fxgmac_channel *channel;
	int processed;

	if (netif_msg_rx_status(pdata))
		yt_dbg(pdata, "%s, budget=%d\n", __func__, budget);

	processed = 0;
	do {
		channel = pdata->channel_head;
		/* Only support 1 tx channel, poll ch 0. */
		fxgmac_tx_poll(pdata->channel_head + 0);
		for (u32 i = 0; i < pdata->channel_count; i++, channel++)
			processed += fxgmac_rx_poll(channel, budget);
	} while (false);

	/* If we processed everything, we are done */
	if (processed < budget) {
		/* Turn off polling */
		if (napi_complete_done(napi, processed))
			pdata->hw_ops.enable_mgm_irq(pdata);
	}

	if ((processed) && (netif_msg_rx_status(pdata)))
		yt_dbg(pdata, "%s, received : %d\n", __func__, processed);

	return processed;
}

static void fxgmac_napi_enable(struct fxgmac_pdata *pdata)
{
	u32 i, *flags = &pdata->int_flags;
	struct fxgmac_channel *channel;
	u32 misc_napi, tx, rx;

	misc_napi = FIELD_GET(BIT(FXGMAC_FLAG_MISC_NAPI_POS), *flags);
	tx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_TX_NAPI_POS,
			     FXGMAC_FLAG_TX_NAPI_LEN);
	rx = FXGMAC_GET_BITS(*flags, FXGMAC_FLAG_RX_NAPI_POS,
			     FXGMAC_FLAG_RX_NAPI_LEN);

	if (!pdata->per_channel_irq) {
		i = FIELD_GET(BIT(FXGMAC_FLAG_LEGACY_NAPI_POS), *flags);
		if (i)
			return;

		netif_napi_add_weight(pdata->netdev, &pdata->napi,
				      fxgmac_all_poll,
				      NAPI_POLL_WEIGHT);

		napi_enable(&pdata->napi);
		fxgmac_set_bits(flags, FXGMAC_FLAG_LEGACY_NAPI_POS,
				FXGMAC_FLAG_LEGACY_NAPI_LEN,
				FXGMAC_NAPI_ENABLE);
		return;
	}

	channel = pdata->channel_head;

	for (i = 0; i < pdata->channel_count; i++, channel++) {
		if (!FXGMAC_GET_BITS(rx, i, FXGMAC_FLAG_PER_RX_NAPI_LEN)) {
			netif_napi_add_weight(pdata->netdev,
					      &channel->napi_rx,
					      fxgmac_one_poll_rx,
					      NAPI_POLL_WEIGHT);

			napi_enable(&channel->napi_rx);
			fxgmac_set_bits(flags, FXGMAC_FLAG_RX_NAPI_POS + i,
					FXGMAC_FLAG_PER_RX_NAPI_LEN,
					FXGMAC_NAPI_ENABLE);
		}

		if (FXGMAC_IS_CHANNEL_WITH_TX_IRQ(i) && !tx) {
			netif_napi_add_weight(pdata->netdev, &channel->napi_tx,
					      fxgmac_one_poll_tx,
					      NAPI_POLL_WEIGHT);
			napi_enable(&channel->napi_tx);
			fxgmac_set_bits(flags, FXGMAC_FLAG_TX_NAPI_POS,
					FXGMAC_FLAG_TX_NAPI_LEN,
					FXGMAC_NAPI_ENABLE);
		}
		if (netif_msg_drv(pdata))
			yt_dbg(pdata, "msix ch%d napi enabled done.\n", i);
	}

	/* Misc */
	if (!misc_napi) {
		netif_napi_add_weight(pdata->netdev, &pdata->napi_misc,
				      fxgmac_misc_poll, NAPI_POLL_WEIGHT);

		napi_enable(&pdata->napi_misc);
		fxgmac_set_bits(flags, FXGMAC_FLAG_MISC_NAPI_POS,
				FXGMAC_FLAG_MISC_NAPI_LEN, FXGMAC_NAPI_ENABLE);
	}
}
