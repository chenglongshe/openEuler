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

static const struct net_device_ops fxgmac_netdev_ops = {
	.ndo_open		= fxgmac_open,
};

const struct net_device_ops *fxgmac_get_netdev_ops(void)
{
	return &fxgmac_netdev_ops;

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
