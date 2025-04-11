/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#ifndef YT6801_TYPE_H
#define YT6801_TYPE_H

#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/pci.h>

#define FXGMAC_DRV_NAME		"yt6801"
#define FXGMAC_DRV_DESC		"Motorcomm Gigabit Ethernet Driver"

#define FXGMAC_RX_BUF_ALIGN	64
#define FXGMAC_TX_MAX_BUF_SIZE	(0x3fff & ~(FXGMAC_RX_BUF_ALIGN - 1))
#define FXGMAC_RX_MIN_BUF_SIZE	(ETH_FRAME_LEN + ETH_FCS_LEN + VLAN_HLEN)

/* Descriptors required for maximum contiguous TSO/GSO packet */
#define FXGMAC_TX_MAX_SPLIT	((GSO_MAX_SIZE / FXGMAC_TX_MAX_BUF_SIZE) + 1)

/* Maximum possible descriptors needed for a SKB */
#define FXGMAC_TX_MAX_DESC_NR	(MAX_SKB_FRAGS + FXGMAC_TX_MAX_SPLIT + 2)

#define FXGMAC_DMA_STOP_TIMEOUT		5
#define FXGMAC_JUMBO_PACKET_MTU		9014
#define FXGMAC_MAX_DMA_RX_CHANNELS	4
#define FXGMAC_MAX_DMA_TX_CHANNELS	1
#define FXGMAC_MAX_DMA_CHANNELS                                           \
	(FXGMAC_MAX_DMA_RX_CHANNELS + FXGMAC_MAX_DMA_TX_CHANNELS)

#define EPHY_CTRL				0x1004
#define EPHY_CTRL_RESET				BIT(0)
#define EPHY_CTRL_STA_LINKUP			BIT(1)
#define EPHY_CTRL_STA_DUPLEX			BIT(2)
#define EPHY_CTRL_STA_SPEED			GENMASK(4, 3)

struct fxgmac_resources {
	void __iomem *addr;
	int irq;
};

enum fxgmac_dev_state {
	FXGMAC_DEV_OPEN		= 0x0,
	FXGMAC_DEV_CLOSE	= 0x1,
	FXGMAC_DEV_STOP		= 0x2,
	FXGMAC_DEV_START	= 0x3,
	FXGMAC_DEV_SUSPEND	= 0x4,
	FXGMAC_DEV_RESUME	= 0x5,
	FXGMAC_DEV_PROBE	= 0xFF,
};

struct fxgmac_pdata {
	struct net_device *ndev;
	struct device *dev;
	struct phy_device *phydev;

	void __iomem *hw_addr;			/* Registers base */

	/* Device interrupt */
	int dev_irq;
	unsigned int per_channel_irq;
	u32 channel_irq[FXGMAC_MAX_DMA_CHANNELS];
	struct msix_entry *msix_entries;
#define INT_FLAG_INTERRUPT		GENMASK(4, 0)
#define INT_FLAG_MSI			BIT(1)
#define INT_FLAG_MSIX			BIT(3)
#define INT_FLAG_LEGACY			BIT(4)
#define INT_FLAG_RX0_NAPI		BIT(18)
#define INT_FLAG_RX1_NAPI		BIT(19)
#define INT_FLAG_RX2_NAPI		BIT(20)
#define INT_FLAG_RX3_NAPI		BIT(21)
#define INT_FLAG_RX0_IRQ		BIT(22)
#define INT_FLAG_RX1_IRQ		BIT(23)
#define INT_FLAG_RX2_IRQ		BIT(24)
#define INT_FLAG_RX3_IRQ		BIT(25)
#define INT_FLAG_TX_NAPI		BIT(26)
#define INT_FLAG_TX_IRQ			BIT(27)
#define INT_FLAG_LEGACY_NAPI		BIT(30)
#define INT_FLAG_LEGACY_IRQ		BIT(31)
	u32 int_flag;		/* interrupt flag */

	u32 msg_enable;
	enum fxgmac_dev_state dev_state;
};

static inline u32 fxgmac_io_rd(struct fxgmac_pdata *priv, u32 reg)
{
	return ioread32(priv->hw_addr + reg);
}

static inline u32
fxgmac_io_rd_bits(struct fxgmac_pdata *priv, u32 reg, u32 mask)
{
	u32 cfg = fxgmac_io_rd(priv, reg);

	return FIELD_GET(mask, cfg);
}

static inline void fxgmac_io_wr(struct fxgmac_pdata *priv, u32 reg, u32 set)
{
	iowrite32(set, priv->hw_addr + reg);
}

static inline void
fxgmac_io_wr_bits(struct fxgmac_pdata *priv, u32 reg, u32 mask, u32 set)
{
	u32 cfg = fxgmac_io_rd(priv, reg);

	cfg &= ~mask;
	cfg |= FIELD_PREP(mask, set);
	fxgmac_io_wr(priv, reg, cfg);
}

#endif /* YT6801_TYPE_H */
