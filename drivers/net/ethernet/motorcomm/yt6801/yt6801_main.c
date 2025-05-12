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

static void fxgmac_phy_release(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, EPHY_CTRL, EPHY_CTRL_RESET, 1);
	fsleep(100);

static void fxgmac_phy_reset(struct fxgmac_pdata *priv)
{
	fxgmac_io_wr_bits(priv, EPHY_CTRL, EPHY_CTRL_RESET, 0);
	fsleep(1500);
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
};

module_pci_driver(fxgmac_pci_driver);

MODULE_AUTHOR("Motorcomm Electronic Tech. Co., Ltd.");
MODULE_DESCRIPTION(FXGMAC_DRV_DESC);
MODULE_LICENSE("GPL");
