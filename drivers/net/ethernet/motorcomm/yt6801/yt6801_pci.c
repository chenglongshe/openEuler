// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#include <linux/kernel.h>
#include <linux/module.h>

#ifdef CONFIG_PCI_MSI
#include <linux/pci.h>
#endif

#include "yt6801_net.h"

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

static void fxgmac_shutdown(struct pci_dev *pcidev)
{
	struct fxgmac_pdata *pdata = dev_get_drvdata(&pcidev->dev);
	struct device *dev = &pcidev->dev;
	bool wake;
	int ret;

	mutex_lock(&pdata->mutex);
	ret = __fxgmac_shutdown(pcidev, &wake);
	if (ret < 0)
		goto unlock;

	if (system_state == SYSTEM_POWER_OFF) {
		pci_wake_from_d3(pcidev, wake);
		pci_set_power_state(pcidev, PCI_D3hot);
	}
unlock:
	mutex_unlock(&pdata->mutex);

	dev_dbg(dev, "%s, system power off=%d\n", __func__,
		(system_state == SYSTEM_POWER_OFF) ? 1 : 0);
}

static int fxgmac_suspend(struct device *device)
{
	struct fxgmac_pdata *pdata = dev_get_drvdata(device);
	struct net_device *netdev = pdata->netdev;
	int ret = 0;

	mutex_lock(&pdata->mutex);
	if (pdata->dev_state != FXGMAC_DEV_START)
		goto unlock;

	if (netif_running(netdev)) {
		ret = __fxgmac_shutdown(to_pci_dev(device), NULL);
		if (ret < 0)
			goto unlock;
	}

	pdata->dev_state = FXGMAC_DEV_SUSPEND;
unlock:
	mutex_unlock(&pdata->mutex);

	return ret;
}

static int fxgmac_resume(struct device *device)
{
	struct fxgmac_pdata *pdata = dev_get_drvdata(device);
	struct net_device *netdev = pdata->netdev;
	int ret = 0;

	mutex_lock(&pdata->mutex);
	if (pdata->dev_state != FXGMAC_DEV_SUSPEND)
		goto unlock;

	pdata->dev_state = FXGMAC_DEV_RESUME;
	__clear_bit(FXGMAC_POWER_STATE_DOWN, &pdata->powerstate);

	rtnl_lock();
	if (netif_running(netdev)) {
		ret = fxgmac_net_powerup(pdata);
		if (ret < 0) {
			dev_err(device, "%s, fxgmac_net_powerup err:%d\n",
				__func__, ret);
			goto unlock;
		}
	}

	netif_device_attach(netdev);
	rtnl_unlock();

	dev_dbg(device, "%s ok\n", __func__);
unlock:
	mutex_unlock(&pdata->mutex);

	return ret;
}

#define MOTORCOMM_PCI_ID			0x1f0a
#define YT6801_PCI_DEVICE_ID			0x6801

static const struct pci_device_id fxgmac_pci_tbl[] = {
	{ PCI_DEVICE(MOTORCOMM_PCI_ID, YT6801_PCI_DEVICE_ID) },
	{ 0 }
};

MODULE_DEVICE_TABLE(pci, fxgmac_pci_tbl);

static const struct dev_pm_ops fxgmac_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(fxgmac_suspend, fxgmac_resume)
};

static struct pci_driver fxgmac_pci_driver = {
	.name		= FXGMAC_DRV_NAME,
	.id_table	= fxgmac_pci_tbl,
	.probe		= fxgmac_probe,
	.remove		= fxgmac_remove,
	.driver.pm	= pm_ptr(&fxgmac_pm_ops),
	.shutdown	= fxgmac_shutdown,
};

module_pci_driver(fxgmac_pci_driver);

MODULE_DESCRIPTION(FXGMAC_DRV_DESC);
MODULE_VERSION(FXGMAC_DRV_VERSION);
MODULE_AUTHOR("Motorcomm Electronic Tech. Co., Ltd.");
MODULE_LICENSE("GPL");
