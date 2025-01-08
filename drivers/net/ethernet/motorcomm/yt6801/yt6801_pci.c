// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#include <linux/kernel.h>
#include <linux/module.h>

#ifdef CONFIG_PCI_MSI
#include <linux/pci.h>
#endif

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
	struct fxgmac_pdata *pdata = dev_get_drvdata(&pcidev->dev);
	struct net_device *netdev = pdata->netdev;
	struct device *dev = &pcidev->dev;

	unregister_netdev(netdev);
	pdata->hw_ops.reset_phy(pdata);
	free_netdev(netdev);

#ifdef CONFIG_PCI_MSI
	if (FIELD_GET(FXGMAC_FLAG_MSIX_ENABLED, pdata->int_flags)) {
		pci_disable_msix(pcidev);
		kfree(pdata->msix_entries);
		pdata->msix_entries = NULL;
	}
#endif

	dev_dbg(dev, "%s has been removed\n", netdev->name);
}

static int __fxgmac_shutdown(struct pci_dev *pcidev, bool *wake_en)
{
	struct fxgmac_pdata *pdata = dev_get_drvdata(&pcidev->dev);
	struct net_device *netdev = pdata->netdev;

	rtnl_lock();
	fxgmac_net_powerdown(pdata, !!pdata->wol);
	if (wake_en)
		*wake_en = !!pdata->wol;

	netif_device_detach(netdev);
	rtnl_unlock();

	return 0;
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

MODULE_DESCRIPTION(FXGMAC_DRV_DESC);
MODULE_VERSION(FXGMAC_DRV_VERSION);
MODULE_AUTHOR("Motorcomm Electronic Tech. Co., Ltd.");
MODULE_LICENSE("GPL");
