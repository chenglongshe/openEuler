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
		yt_dbg(pdata, "%s, powerstate :%ld.\n", __func__,
		       pdata->powerstate);

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
