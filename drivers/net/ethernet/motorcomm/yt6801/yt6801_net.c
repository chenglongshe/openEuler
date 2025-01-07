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

static void fxgmac_napi_enable(struct fxgmac_pdata *pdata);

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
		yt_dbg(pdata, "%s, powerstate :%lu.\n", __func__,
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

static const struct net_device_ops fxgmac_netdev_ops = {
	.ndo_open		= fxgmac_open,
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
