// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd.
 *
 * Below is a simplified block diagram of YT6801 chip and its relevant
 * interfaces.
 *                      ||
 *  ********************++**********************
 *  *            | PCIE Endpoint |             *
 *  *            +---------------+             *
 *  *                | GMAC |                  *
 *  *                +--++--+                  *
 *  *                  |**|                    *
 *  *         GMII --> |**| <-- MDIO           *
 *  *                 +-++--+                  *
 *  *            | Integrated PHY |  YT8531S   *
 *  *                 +-++-+                   *
 *  ********************||******************* **
 */

#include <linux/module.h>
#include "yt6801_type.h"
#include "yt6801_desc.h"

#define PHY_WR_CONFIG(reg_offset)	(0x8000205 + ((reg_offset) * 0x10000))
static int fxgmac_phy_write_reg(struct fxgmac_pdata *priv, u32 reg_id, u32 data)
{
	u32 val;
	int ret;

	fxgmac_io_wr(priv, MAC_MDIO_DATA, data);
	fxgmac_io_wr(priv, MAC_MDIO_ADDR, PHY_WR_CONFIG(reg_id));
	ret = read_poll_timeout_atomic(fxgmac_io_rd, val,
				       !field_get(MAC_MDIO_ADDR_BUSY, val),
				       10, 250, false, priv, MAC_MDIO_ADDR);
	if (ret == -ETIMEDOUT)
		dev_err(priv->dev, "%s, id:%x ctrl:0x%08x, data:0x%08x\n",
			__func__, reg_id, PHY_WR_CONFIG(reg_id), data);

	return ret;
}

#define PHY_RD_CONFIG(reg_offset)	(0x800020d + ((reg_offset) * 0x10000))
static int fxgmac_phy_read_reg(struct fxgmac_pdata *priv, u32 reg_id)
{
	u32 val;
	int ret;

	fxgmac_io_wr(priv, MAC_MDIO_ADDR, PHY_RD_CONFIG(reg_id));
	ret = read_poll_timeout_atomic(fxgmac_io_rd, val,
				       !field_get(MAC_MDIO_ADDR_BUSY, val),
				       10, 250, false, priv, MAC_MDIO_ADDR);
	if (ret == -ETIMEDOUT) {
		dev_err(priv->dev, "%s, id:%x, ctrl:0x%08x, val:0x%08x.\n",
			__func__, reg_id, PHY_RD_CONFIG(reg_id), val);
		return ret;
	}

	return fxgmac_io_rd(priv, MAC_MDIO_DATA); /* Read data */
}

static int fxgmac_mdio_write_reg(struct mii_bus *mii_bus, int phyaddr,
				 int phyreg, u16 val)
{
	if (phyaddr > 0)
		return -ENODEV;

	return fxgmac_phy_write_reg(mii_bus->priv, phyreg, val);
}

static int fxgmac_mdio_read_reg(struct mii_bus *mii_bus, int phyaddr,
				int phyreg)
{
	if (phyaddr > 0)
		return -ENODEV;

	return fxgmac_phy_read_reg(mii_bus->priv, phyreg);
}

static int fxgmac_mdio_register(struct fxgmac_pdata *priv)
{
	struct pci_dev *pdev = to_pci_dev(priv->dev);
	struct phy_device *phydev;
	struct mii_bus *new_bus;
	int ret;

	new_bus = devm_mdiobus_alloc(&pdev->dev);
	if (!new_bus)
		return -ENOMEM;

	new_bus->name = "yt6801";
	new_bus->priv = priv;
	new_bus->parent = &pdev->dev;
	new_bus->read = fxgmac_mdio_read_reg;
	new_bus->write = fxgmac_mdio_write_reg;
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "yt6801-%x-%x",
		 pci_domain_nr(pdev->bus), pci_dev_id(pdev));

	ret = devm_mdiobus_register(&pdev->dev, new_bus);
	if (ret < 0)
		return ret;

	phydev = mdiobus_get_phy(new_bus, 0);
	if (!phydev)
		return -ENODEV;

	priv->phydev = phydev;
	return 0;
}

static void fxgmac_disable_mgm_irq(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, MGMT_INT_CTRL0, MGMT_INT_CTRL0_INT_MASK,
			  MGMT_INT_CTRL0_INT_MASK_MASK);
}

static void napi_disable_del(struct fxgmac_pdata *priv, struct napi_struct *n,
			     u32 flag)
{
	napi_disable(n);
	netif_napi_del(n);
	priv->int_flag &= ~flag;
}

static void fxgmac_napi_disable(struct fxgmac_pdata *priv)
{
	struct fxgmac_channel *channel = priv->channel_head;
	u32 rx_napi[] = {INT_FLAG_RX0_NAPI, INT_FLAG_RX1_NAPI,
			INT_FLAG_RX2_NAPI, INT_FLAG_RX3_NAPI};

	if (!priv->per_channel_irq) {
		if (!field_get(INT_FLAG_LEGACY_NAPI, priv->int_flag))
			return;

		napi_disable_del(priv, &priv->napi,
				 INT_FLAG_LEGACY_NAPI);
		return;
	}

	if (field_get(INT_FLAG_TX_NAPI, priv->int_flag))
		napi_disable_del(priv, &channel->napi_tx, INT_FLAG_TX_NAPI);

	for (u32 i = 0; i < priv->channel_count; i++, channel++)
		if (priv->int_flag & rx_napi[i])
			napi_disable_del(priv, &channel->napi_rx, rx_napi[i]);
}

static void fxgmac_free_irqs(struct fxgmac_pdata *priv)
{
	u32 rx_irq[] = {INT_FLAG_RX0_IRQ, INT_FLAG_RX1_IRQ,
			INT_FLAG_RX2_IRQ, INT_FLAG_RX3_IRQ};
	struct fxgmac_channel *channel = priv->channel_head;

	if (!field_get(INT_FLAG_MSIX, priv->int_flag) &&
	    field_get(INT_FLAG_LEGACY_IRQ, priv->int_flag)) {
		devm_free_irq(priv->dev, priv->dev_irq, priv);
		priv->int_flag &= ~INT_FLAG_LEGACY_IRQ;
	}

	if (!priv->per_channel_irq)
		return;

	if (field_get(INT_FLAG_TX_IRQ, priv->int_flag)) {
		priv->int_flag &= ~INT_FLAG_TX_IRQ;
		devm_free_irq(priv->dev, channel->dma_irq_tx, channel);
	}

	for (u32 i = 0; i < priv->channel_count; i++, channel++)
		if (priv->int_flag & rx_irq[i]) {
			priv->int_flag &= ~rx_irq[i];
			devm_free_irq(priv->dev, channel->dma_irq_rx, channel);
		}
}

static void fxgmac_free_tx_data(struct fxgmac_pdata *priv)
{
	struct fxgmac_channel *channel = priv->channel_head;
	struct fxgmac_ring *ring;

	for (u32 i = 0; i < priv->channel_count; i++, channel++) {
		ring = channel->tx_ring;
		if (!ring)
			break;

		for (u32 j = 0; j < ring->dma_desc_count; j++)
			fxgmac_desc_data_unmap(priv,
					       FXGMAC_GET_DESC_DATA(ring, j));
	}
}

static void fxgmac_free_rx_data(struct fxgmac_pdata *priv)
{
	struct fxgmac_channel *channel = priv->channel_head;
	struct fxgmac_ring *ring;

	for (u32 i = 0; i < priv->channel_count; i++, channel++) {
		ring = channel->rx_ring;
		if (!ring)
			break;

		for (u32 j = 0; j < ring->dma_desc_count; j++)
			fxgmac_desc_data_unmap(priv,
					       FXGMAC_GET_DESC_DATA(ring, j));
	}
}

static void fxgmac_prepare_tx_stop(struct fxgmac_pdata *priv,
				   struct fxgmac_channel *channel)
{
	unsigned long tx_timeout;
	unsigned int tx_status;

	/* The Tx engine cannot be stopped if it is actively processing
	 * descriptors. Wait for the Tx engine to enter the stopped or
	 * suspended state.
	 */
	tx_timeout = jiffies + (FXGMAC_DMA_STOP_TIMEOUT * HZ);

	while (time_before(jiffies, tx_timeout)) {
		tx_status = fxgmac_io_rd(priv, DMA_DSR0);
		tx_status = field_get(DMA_DSR0_TPS, tx_status);
		if (tx_status == DMA_TPS_STOPPED ||
		    tx_status == DMA_TPS_SUSPENDED)
			break;

		fsleep(500);
	}

	if (!time_before(jiffies, tx_timeout))
		dev_err(priv->dev, "timed out waiting for Tx DMA channel  stop\n");
}

static void fxgmac_disable_tx(struct fxgmac_pdata *priv)
{
	struct fxgmac_channel *channel = priv->channel_head;

	/* Prepare for Tx DMA channel stop */
	fxgmac_prepare_tx_stop(priv, channel);

	fxgmac_io_wr_bits(priv, MAC_CR, MAC_CR_TE, 0); /* Disable MAC Tx */

	/* Disable Tx queue */
	fxgmac_mtl_wr_bits(priv, 0, MTL_Q_TQOMR, MTL_Q_TQOMR_TXQEN,
			   MTL_Q_DISABLED);

	/* Disable Tx DMA channel */
	fxgmac_dma_wr_bits(channel, DMA_CH_TCR, DMA_CH_TCR_ST, 0);
}

static void fxgmac_prepare_rx_stop(struct fxgmac_pdata *priv,
				   unsigned int queue)
{
	unsigned int rx_status, rx_q, rx_q_sts;
	unsigned long rx_timeout;

	/* The Rx engine cannot be stopped if it is actively processing
	 * packets. Wait for the Rx queue to empty the Rx fifo.
	 */
	rx_timeout = jiffies + (FXGMAC_DMA_STOP_TIMEOUT * HZ);

	while (time_before(jiffies, rx_timeout)) {
		rx_status = fxgmac_mtl_io_rd(priv, queue, MTL_Q_RQDR);
		rx_q = field_get(MTL_Q_RQDR_PRXQ, rx_status);
		rx_q_sts = field_get(MTL_Q_RQDR_RXQSTS, rx_status);
		if (rx_q == 0 && rx_q_sts == 0)
			break;

		fsleep(500);
	}

	if (!time_before(jiffies, rx_timeout))
		dev_err(priv->dev, "timed out waiting for Rx queue %u to empty\n",
			queue);
}

static void fxgmac_disable_rx(struct fxgmac_pdata *priv)
{
	struct fxgmac_channel *channel = priv->channel_head;
	u32 i;

	/* Disable MAC Rx */
	fxgmac_io_wr_bits(priv, MAC_CR,  MAC_CR_CST, 0);
	fxgmac_io_wr_bits(priv, MAC_CR,  MAC_CR_ACS, 0);
	fxgmac_io_wr_bits(priv, MAC_CR,  MAC_CR_RE, 0);

	/* Prepare for Rx DMA channel stop */
	for (i = 0; i < priv->rx_q_count; i++)
		fxgmac_prepare_rx_stop(priv, i);

	fxgmac_io_wr(priv, MAC_RQC0R, 0); /* Disable each Rx queue */

	/* Disable each Rx DMA channel */
	for (i = 0; i < priv->channel_count; i++, channel++)
		fxgmac_dma_wr_bits(channel, DMA_CH_RCR, DMA_CH_RCR_SR, 0);
}

/**
 * fxgmac_set_oob_wol - disable or enable oob wol crtl function
 * @priv: driver private struct
 * @en: 1 or 0
 *
 * Description:  After enable OOB_WOL from efuse, mac will loopcheck phy status,
 *   and lead to panic sometimes. So we should disable it from powerup,
 *   enable it from power down.
 */
static void fxgmac_set_oob_wol(struct fxgmac_pdata *priv, unsigned int en)
{
	/* en = 1 is disable */
	fxgmac_io_wr_bits(priv, OOB_WOL_CTRL, OOB_WOL_CTRL_DIS, !en);
}

static void fxgmac_config_powerup(struct fxgmac_pdata *priv)
{
	fxgmac_set_oob_wol(priv, 0);
	/* GAMC power up */
	fxgmac_io_wr_bits(priv, MAC_PMT_STA, MAC_PMT_STA_PWRDWN, 0);
}

static void fxgmac_pre_powerdown(struct fxgmac_pdata *priv)
{
	fxgmac_set_oob_wol(priv, 1);
	fsleep(2000);
}
static void fxgmac_phy_release(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, EPHY_CTRL, EPHY_CTRL_RESET, 1);
	fsleep(100);

static void fxgmac_phy_reset(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, EPHY_CTRL, EPHY_CTRL_RESET, 0);
	fsleep(1500);
}

static void fxgmac_disable_msix_irqs(struct fxgmac_pdata *priv)
{
	for (u32 intid = 0; intid < MSIX_TBL_MAX_NUM; intid++)
		fxgmac_disable_msix_one_irq(priv, intid);
}

static void fxgmac_stop(struct fxgmac_pdata *priv)
{
	struct net_device *ndev = priv->ndev;
	struct netdev_queue *txq;

	if (priv->dev_state != FXGMAC_DEV_START)
		return;

	priv->dev_state = FXGMAC_DEV_STOP;

	if (priv->per_channel_irq)
		fxgmac_disable_msix_irqs(priv);
	else
		fxgmac_disable_mgm_irq(priv);

	netif_carrier_off(ndev);
	netif_tx_stop_all_queues(ndev);
	fxgmac_disable_tx(priv);
	fxgmac_disable_rx(priv);
	fxgmac_free_irqs(priv);
	fxgmac_napi_disable(priv);
	phy_stop(priv->phydev);

	txq = netdev_get_tx_queue(ndev, priv->channel_head->queue_index);
	netdev_tx_reset_queue(txq);
}

static void fxgmac_config_powerdown(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, MAC_CR, MAC_CR_RE, 1); /* Enable MAC Rx */
	fxgmac_io_wr_bits(priv, MAC_CR, MAC_CR_TE, 1); /* Enable MAC TX */

	/* Set GAMC power down */
	fxgmac_io_wr_bits(priv, MAC_PMT_STA, MAC_PMT_STA_PWRDWN, 1);
}

static int fxgmac_net_powerdown(struct fxgmac_pdata *priv)
{
	struct net_device *ndev = priv->ndev;

	/* Signal that we are down to the interrupt handler */
	if (__test_and_set_bit(FXGMAC_POWER_STATE_DOWN, &priv->power_state))
		return 0; /* do nothing if already down */

	__clear_bit(FXGMAC_POWER_STATE_UP, &priv->power_state);
	netif_tx_stop_all_queues(ndev); /* Shut off incoming Tx traffic */

	/* Call carrier off first to avoid false dev_watchdog timeouts */
	netif_carrier_off(ndev);
	netif_tx_disable(ndev);
	fxgmac_disable_rx(priv);

	/* Synchronize_rcu() needed for pending XDP buffers to drain */
	synchronize_rcu();

	fxgmac_stop(priv);
	fxgmac_pre_powerdown(priv);

	if (!test_bit(FXGMAC_POWER_STATE_DOWN, &priv->power_state))
		dev_err(priv->dev, "fxgmac powerstate is %lu when config powe down.\n",
			priv->power_state);

	/* Set mac to lowpower mode */
	fxgmac_config_powerdown(priv);
	fxgmac_free_tx_data(priv);
	fxgmac_free_rx_data(priv);

	return 0;
}

static void fxgmac_init_interrupt_scheme(struct fxgmac_pdata *priv)
{
	struct pci_dev *pdev = to_pci_dev(priv->dev);
	int req_vectors = FXGMAC_MAX_DMA_CHANNELS;

	/* Since we have FXGMAC_MAX_DMA_CHANNELS channels, we must ensure the
	 * number of cpu core is ok. otherwise, just roll back to legacy.
	 */
	if (num_online_cpus() < FXGMAC_MAX_DMA_CHANNELS - 1)
		goto enable_msi_interrupt;

	priv->msix_entries =
		kcalloc(req_vectors, sizeof(struct msix_entry), GFP_KERNEL);
	if (!priv->msix_entries)
		goto enable_msi_interrupt;

	for (u32 i = 0; i < req_vectors; i++)
		priv->msix_entries[i].entry = i;

	if (pci_enable_msix_exact(pdev, priv->msix_entries, req_vectors) < 0) {
		/* Roll back to msi */
		kfree(priv->msix_entries);
		priv->msix_entries = NULL;
		dev_err(priv->dev, "Enable MSIx failed, clear msix entries.\n");
		goto enable_msi_interrupt;
	}

	priv->int_flag &= ~INT_FLAG_INTERRUPT;
	priv->int_flag |= INT_FLAG_MSIX;
	priv->per_channel_irq = 1;
	return;

enable_msi_interrupt:
	priv->int_flag &= ~INT_FLAG_INTERRUPT;
	if (pci_enable_msi(pdev) < 0) {
		priv->int_flag |= INT_FLAG_LEGACY;
		dev_err(priv->dev, "rollback to LEGACY.\n");
	} else {
		priv->int_flag |= INT_FLAG_MSI;
		dev_err(priv->dev, "rollback to MSI.\n");
		priv->dev_irq = pdev->irq;
	}
}

static int fxgmac_drv_probe(struct device *dev, struct fxgmac_resources *res)
{
	struct fxgmac_pdata *priv;
	struct net_device *ndev;
	int ret;

	ndev = alloc_etherdev_mq(sizeof(struct fxgmac_pdata),
				 FXGMAC_MAX_DMA_RX_CHANNELS);
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	priv = netdev_priv(ndev);

	priv->dev = dev;
	priv->ndev = ndev;
	priv->dev_irq = res->irq;
	priv->hw_addr = res->addr;
	priv->msg_enable = NETIF_MSG_DRV;
	priv->dev_state = FXGMAC_DEV_PROBE;

	/* Default to legacy interrupt */
	priv->int_flag &= ~INT_FLAG_INTERRUPT;
	priv->int_flag |= INT_FLAG_LEGACY;

	pci_set_drvdata(to_pci_dev(priv->dev), priv);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		fxgmac_init_interrupt_scheme(priv);

	ret = fxgmac_init(priv, true);
	if (ret < 0) {
		dev_err(dev, "fxgmac init failed:%d\n", ret);
		goto err_free_netdev;
	}

	fxgmac_phy_reset(priv);
	fxgmac_phy_release(priv);
	ret = fxgmac_mdio_register(priv);
	if (ret < 0) {
		dev_err(dev, "Register fxgmac mdio failed:%d\n", ret);
		goto err_free_netdev;
	}

	netif_carrier_off(ndev);
	ret = register_netdev(ndev);
	if (ret) {
		dev_err(dev, "Register ndev failed:%d\n", ret);
		goto err_free_netdev;
	}

	return 0;

err_free_netdev:
	free_netdev(ndev);
	return ret;
}

static int fxgmac_probe(struct pci_dev *pcidev, const struct pci_device_id *id)
{
	struct device *dev = &pcidev->dev;
	struct fxgmac_resources res;
	int i, ret;

	ret = pcim_enable_device(pcidev);
	if (ret) {
		dev_err(dev, "%s pcim_enable_device err:%d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (pci_resource_len(pcidev, i) == 0)
			continue;

		ret = pcim_iomap_regions(pcidev, BIT(i), FXGMAC_DRV_NAME);
		if (ret) {
			dev_err(dev, "%s, pcim_iomap_regions err:%d\n",
				__func__, ret);
			return ret;
		}
		break;
	}

	pci_set_master(pcidev);

	memset(&res, 0, sizeof(res));
	res.irq = pcidev->irq;
	res.addr = pcim_iomap_table(pcidev)[i];

	return fxgmac_drv_probe(&pcidev->dev, &res);
}

static void fxgmac_remove(struct pci_dev *pcidev)
{
	struct fxgmac_pdata *priv = dev_get_drvdata(&pcidev->dev);
	struct net_device *ndev = priv->ndev;

	unregister_netdev(ndev);
	fxgmac_phy_reset(priv);
	free_netdev(ndev);

	if (IS_ENABLED(CONFIG_PCI_MSI) &&
	    FIELD_GET(INT_FLAG_MSIX, priv->int_flag)) {
		pci_disable_msix(pcidev);
		kfree(priv->msix_entries);
		priv->msix_entries = NULL;
	}
}

static void __fxgmac_shutdown(struct pci_dev *pcidev)
{
	struct fxgmac_pdata *priv = dev_get_drvdata(&pcidev->dev);
	struct net_device *ndev = priv->ndev;

	fxgmac_net_powerdown(priv);
	netif_device_detach(ndev);
}

static void fxgmac_shutdown(struct pci_dev *pcidev)
{
	rtnl_lock();
	 __fxgmac_shutdown(pcidev);
	if (system_state == SYSTEM_POWER_OFF) {
		pci_wake_from_d3(pcidev, false);
		pci_set_power_state(pcidev, PCI_D3hot);
	}
	rtnl_unlock();
}
#define MOTORCOMM_PCI_ID			0x1f0a
#define YT6801_PCI_DEVICE_ID			0x6801

static const struct pci_device_id fxgmac_pci_tbl[] = {
	{ PCI_DEVICE(MOTORCOMM_PCI_ID, YT6801_PCI_DEVICE_ID) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, fxgmac_pci_tbl);

static struct pci_driver fxgmac_pci_driver = {
	.name		= FXGMAC_DRV_NAME,
	.id_table	= fxgmac_pci_tbl,
	.probe		= fxgmac_probe,
	.remove		= fxgmac_remove,
	.shutdown	= fxgmac_shutdown,
};

module_pci_driver(fxgmac_pci_driver);

MODULE_AUTHOR("Motorcomm Electronic Tech. Co., Ltd.");
MODULE_DESCRIPTION(FXGMAC_DRV_DESC);
MODULE_LICENSE("GPL");
