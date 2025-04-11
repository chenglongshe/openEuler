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

/****************  Other configuration register. *********************/
#define GLOBAL_CTRL0				0x1000

#define EPHY_CTRL				0x1004
#define EPHY_CTRL_RESET				BIT(0)
#define EPHY_CTRL_STA_LINKUP			BIT(1)
#define EPHY_CTRL_STA_DUPLEX			BIT(2)
#define EPHY_CTRL_STA_SPEED			GENMASK(4, 3)

#define OOB_WOL_CTRL				0x1010
#define OOB_WOL_CTRL_DIS			BIT(0)

/* MAC management registers */
#define MGMT_INT_CTRL0				0x1100
#define MGMT_INT_CTRL0_INT_STATUS		GENMASK(15, 0)
#define  MGMT_INT_CTRL0_INT_STATUS_RX		0x000f
#define  MGMT_INT_CTRL0_INT_STATUS_TX		0x0010
#define  MGMT_INT_CTRL0_INT_STATUS_MISC		0x0020
#define  MGMT_INT_CTRL0_INT_STATUS_RXTX		0x0030
#define MGMT_INT_CTRL0_INT_MASK			GENMASK(31, 16)
#define  MGMT_INT_CTRL0_INT_MASK_RXCH		0x000f
#define  MGMT_INT_CTRL0_INT_MASK_TXCH		0x0010
#define  MGMT_INT_CTRL0_INT_MASK_MISC		0x0020
#define  MGMT_INT_CTRL0_INT_MASK_EX_PMT		0xf7ff
#define  MGMT_INT_CTRL0_INT_MASK_DISABLE	0xf000
#define  MGMT_INT_CTRL0_INT_MASK_MASK		0xffff

/* Interrupt Moderation */
#define INT_MOD					0x1108
#define INT_MOD_RX				GENMASK(11, 0)
#define  INT_MOD_200_US				200
#define INT_MOD_TX				GENMASK(27, 16)

/* LTR_CTRL3, LTR latency message, only for System IDLE Start. */
#define LTR_IDLE_ENTER				0x113c
#define LTR_IDLE_ENTER_ENTER			GENMASK(9, 0)
#define  LTR_IDLE_ENTER_900_US			900
#define LTR_IDLE_ENTER_SCALE			GENMASK(14, 10)
#define  LTR_IDLE_ENTER_SCALE_1_NS		0
#define  LTR_IDLE_ENTER_SCALE_32_NS		1
#define  LTR_IDLE_ENTER_SCALE_1024_NS		2
#define  LTR_IDLE_ENTER_SCALE_32768_NS		3
#define  LTR_IDLE_ENTER_SCALE_1048576_NS	4
#define  LTR_IDLE_ENTER_SCALE_33554432_NS	5
#define LTR_IDLE_ENTER_REQUIRE			BIT(15)

/* LTR_CTRL4, LTR latency message, only for System IDLE End. */
#define LTR_IDLE_EXIT				0x1140
#define LTR_IDLE_EXIT_EXIT			GENMASK(9, 0)
#define  LTR_IDLE_EXIT_171_US			171
#define LTR_IDLE_EXIT_SCALE			GENMASK(14, 10)
#define LTR_IDLE_EXIT_REQUIRE			BIT(15)

#define MSIX_TBL_MASK				0x120c

/* msi table */
#define MSI_ID_RXQ0				0
#define MSI_ID_RXQ1				1
#define MSI_ID_RXQ2				2
#define MSI_ID_RXQ3				3
#define MSI_ID_TXQ0				4
#define MSIX_TBL_MAX_NUM			5

#define MSI_PBA					0x1300

#define EFUSE_OP_CTRL_0				0x1500
#define EFUSE_OP_MODE				GENMASK(1, 0)
#define  EFUSE_OP_MODE_ROW_WRITE		0x0
#define  EFUSE_OP_MODE_ROW_READ			0x1
#define  EFUSE_OP_MODE_AUTO_LOAD		0x2
#define  EFUSE_OP_MODE_READ_BLANK		0x3
#define EFUSE_OP_START				BIT(2)
#define EFUSE_OP_ADDR				GENMASK(15, 8)
#define EFUSE_OP_WR_DATA			GENMASK(23, 16)

#define EFUSE_OP_CTRL_1				0x1504
#define EFUSE_OP_DONE				BIT(1)
#define EFUSE_OP_PGM_PASS			BIT(2)
#define EFUSE_OP_BIST_ERR_CNT			GENMASK(15, 8)
#define EFUSE_OP_BIST_ERR_ADDR			GENMASK(23, 16)
#define EFUSE_OP_RD_DATA			GENMASK(31, 24)

/* MAC addr can be configured through effuse */
#define MACA0LR_FROM_EFUSE			0x1520
#define MACA0HR_FROM_EFUSE			0x1524

#define SYS_RESET				0x152c
#define SYS_RESET_RESET				BIT(31)

#define PCIE_SERDES_PLL				0x199c
#define PCIE_SERDES_PLL_AUTOOFF			BIT(0)

/****************  GMAC register. *********************/
#define MAC_CR				0x2000
#define MAC_CR_RE			BIT(0)
#define MAC_CR_TE			BIT(1)
#define MAC_CR_LM			BIT(12)
#define MAC_CR_DM			BIT(13)
#define MAC_CR_FES			BIT(14)
#define MAC_CR_PS			BIT(15)
#define MAC_CR_JE			BIT(16)
#define MAC_CR_ACS			BIT(20)
#define MAC_CR_CST			BIT(21)
#define MAC_CR_IPC			BIT(27)
#define MAC_CR_ARPEN			BIT(31)

#define MAC_RQC0R			0x20a0
#define MAC_RQC1R			0x20a4
#define MAC_RQC2R			0x20a8
#define MAC_RQC2_INC			4
#define MAC_RQC2_Q_PER_REG		4

#define MAC_ISR				0x20b0
#define MAC_ISR_PHYIF_STA		BIT(0)
#define MAC_ISR_AN_SR			GENMASK(3, 1)
#define MAC_ISR_PMT_STA			BIT(4)
#define MAC_ISR_LPI_STA			BIT(5)
#define MAC_ISR_MMC_STA			BIT(8)
#define MAC_ISR_RX_MMC_STA		BIT(9)
#define MAC_ISR_TX_MMC_STA		BIT(10)
#define MAC_ISR_IPC_RXINT		BIT(11)
#define MAC_ISR_TSIS			BIT(12)
#define MAC_ISR_TX_RX_STA		GENMASK(14, 13)
#define MAC_ISR_GPIO_SR			GENMASK(25, 15)

#define MAC_IER				0x20b4
#define MAC_IER_TSIE			BIT(12)

#define MAC_TX_RX_STA			0x20b8

#define MAC_PMT_STA			0x20c0
#define MAC_PMT_STA_PWRDWN		BIT(0)
#define MAC_PMT_STA_MGKPKTEN		BIT(1)
#define MAC_PMT_STA_RWKPKTEN		BIT(2)
#define MAC_PMT_STA_MGKPRCVD		BIT(5)
#define MAC_PMT_STA_RWKPRCVD		BIT(6)
#define MAC_PMT_STA_GLBLUCAST		BIT(9)
#define MAC_PMT_STA_RWKPTR		GENMASK(27, 24)
#define MAC_PMT_STA_RWKFILTERST		BIT(31)

#define MAC_RWK_PAC			0x20c4
#define MAC_LPI_STA			0x20d0
#define MAC_LPI_CONTROL			0x20d4
#define MAC_LPI_TIMER			0x20d8
#define MAC_MS_TIC_COUNTER		0x20dc
#define MAC_AN_CR			0x20e0
#define MAC_AN_SR			0x20e4
#define MAC_AN_ADV			0x20e8
#define MAC_AN_LPA			0x20ec
#define MAC_AN_EXP			0x20f0
#define MAC_PHYIF_STA			0x20f8
#define MAC_VR				0x2110
#define MAC_DBG_STA			0x2114

#define MAC_HWF0R			0x211c
#define MAC_HWF0R_VLHASH		BIT(4)
#define MAC_HWF0R_SMASEL		BIT(5)
#define MAC_HWF0R_RWKSEL		BIT(6)
#define MAC_HWF0R_MGKSEL		BIT(7)
#define MAC_HWF0R_MMCSEL		BIT(8)
#define MAC_HWF0R_ARPOFFSEL		BIT(9)
#define MAC_HWF0R_TSSEL			BIT(12)
#define MAC_HWF0R_EEESEL		BIT(13)
#define MAC_HWF0R_TXCOESEL		BIT(14)
#define MAC_HWF0R_RXCOESEL		BIT(16)
#define MAC_HWF0R_ADDMACADRSEL		GENMASK(22, 18)
#define MAC_HWF0R_TSSTSSEL		GENMASK(26, 25)
#define MAC_HWF0R_SAVLANINS		BIT(27)
#define MAC_HWF0R_ACTPHYIFSEL		GENMASK(30, 28)

#define MAC_HWF1R			0x2120
#define MAC_HWF1R_RXFIFOSIZE		GENMASK(4, 0)
#define MAC_HWF1R_TXFIFOSIZE		GENMASK(10, 6)
#define MAC_HWF1R_ADVTHWORD		BIT(13)
#define MAC_HWF1R_ADDR64		GENMASK(15, 14)
#define MAC_HWF1R_DCBEN			BIT(16)
#define MAC_HWF1R_SPHEN			BIT(17)
#define MAC_HWF1R_TSOEN			BIT(18)
#define MAC_HWF1R_DBGMEMA		BIT(19)
#define MAC_HWF1R_AVSEL			BIT(20)
#define MAC_HWF1R_RAVSEL		BIT(21)
#define MAC_HWF1R_HASHTBLSZ		GENMASK(25, 24)
#define MAC_HWF1R_L3L4FNUM		GENMASK(30, 27)

#define MAC_HWF2R			0x2124
#define MAC_HWF2R_RXQCNT		GENMASK(3, 0)
#define MAC_HWF2R_TXQCNT		GENMASK(9, 6)
#define MAC_HWF2R_RXCHCNT		GENMASK(15, 12)
#define MAC_HWF2R_TXCHCNT		GENMASK(21, 18)
#define MAC_HWF2R_PPSOUTNUM		GENMASK(26, 24)
#define MAC_HWF2R_AUXSNAPNUM		GENMASK(30, 28)

#define MAC_HWF3R			0x2128

#define MAC_MDIO_ADDR			0x2200
#define MAC_MDIO_ADDR_BUSY		BIT(0)
#define MAC_MDIO_ADDR_GOC		GENMASK(3, 2)

#define MAC_MDIO_DATA			0x2204
#define MAC_MDIO_DATA_GD		GENMASK(15, 0)
#define MAC_MDIO_DATA_RA		GENMASK(31, 16)

/* MTL queue registers */
#define MTL_Q_BASE			0x2d00
#define MTL_Q_INC			0x40

#define MTL_Q_TQOMR			0x00
#define MTL_Q_TQOMR_FTQ			BIT(0)
#define MTL_Q_TQOMR_TSF			BIT(1)
#define MTL_Q_TQOMR_TXQEN		GENMASK(3, 2)
#define MTL_Q_DISABLED			0x00
#define MTL_Q_EN_IF_AV			0x01
#define MTL_Q_ENABLED			0x02

#define MTL_Q_IR			0x2c /* Interrupt control status */
#define MTL_Q_RQDR			0x38
#define MTL_Q_RQDR_RXQSTS		GENMASK(5, 4)
#define MTL_Q_RQDR_PRXQ			GENMASK(29, 16)

/* DMA registers */
#define DMA_MR					0x3000
#define DMA_MR_SWR				BIT(0)
#define DMA_MR_TXPR				BIT(11)
#define DMA_MR_INTM				GENMASK(17, 16)
#define DMA_MR_QUREAD				BIT(19)
#define DMA_MR_TNDF				GENMASK(21, 20)
#define DMA_MR_RNDF				GENMASK(23, 22)

#define DMA_DSRX_INC				4
#define DMA_DSR0				0x300c
#define DMA_DSR0_TPS				GENMASK(15, 12)
#define  DMA_TPS_STOPPED			0x00
#define  DMA_TPS_SUSPENDED			0x06

/* DMA channel registers */
#define DMA_CH_BASE			0x3100
#define DMA_CH_INC			0x80

#define DMA_CH_TCR			0x04
#define DMA_CH_TCR_ST			BIT(0)
#define DMA_CH_TCR_OSP			BIT(4)
#define DMA_CH_TCR_TSE			BIT(12)
#define DMA_CH_TCR_PBL			GENMASK(21, 16)
#define  DMA_CH_PBL_1			1
#define  DMA_CH_PBL_2			2
#define  DMA_CH_PBL_4			4
#define  DMA_CH_PBL_8			8
#define  DMA_CH_PBL_16			16
#define  DMA_CH_PBL_32			32
#define  DMA_CH_PBL_64			64
#define  DMA_CH_PBL_128			128
#define  DMA_CH_PBL_256			256

#define DMA_CH_RCR			0x08
#define DMA_CH_RCR_SR			BIT(0)
#define DMA_CH_RCR_RBSZ			GENMASK(14, 1)
#define DMA_CH_RCR_PBL			GENMASK(21, 16)

#define DMA_CH_IER			0x34
#define DMA_CH_IER_TIE			BIT(0)
#define DMA_CH_IER_TXSE			BIT(1)
#define DMA_CH_IER_TBUE			BIT(2)
#define DMA_CH_IER_RIE			BIT(6)
#define DMA_CH_IER_RBUE			BIT(7)
#define DMA_CH_IER_RSE			BIT(8)
#define DMA_CH_IER_FBEE			BIT(12)
#define DMA_CH_IER_AIE			BIT(14)
#define DMA_CH_IER_NIE			BIT(15)

#define DMA_CH_SR			0x60
#define DMA_CH_SR_TI			BIT(0)
#define DMA_CH_SR_TPS			BIT(1)
#define DMA_CH_SR_TBU			BIT(2)
#define DMA_CH_SR_RI			BIT(6)
#define DMA_CH_SR_RBU			BIT(7)
#define DMA_CH_SR_RPS			BIT(8)
#define DMA_CH_SR_FBE			BIT(12)

struct fxgmac_ring_buf {
	struct sk_buff *skb;
	dma_addr_t skb_dma;
	unsigned int skb_len;
};

/* Common Tx and Rx DMA hardware descriptor */
struct fxgmac_dma_desc {
	__le32 desc0;
	__le32 desc1;
	__le32 desc2;
	__le32 desc3;
};

/* Page allocation related values */
struct fxgmac_page_alloc {
	struct page *pages;
	unsigned int pages_len;
	unsigned int pages_offset;
	dma_addr_t pages_dma;
};

/* Ring entry buffer data */
struct fxgmac_buffer_data {
	struct fxgmac_page_alloc pa;
	struct fxgmac_page_alloc pa_unmap;

	dma_addr_t dma_base;
	unsigned long dma_off;
	unsigned int dma_len;
};

struct fxgmac_tx_desc_data {
	unsigned int packets;		/* BQL packet count */
	unsigned int bytes;		/* BQL byte count */
};

struct fxgmac_rx_desc_data {
	struct fxgmac_buffer_data hdr;	/* Header locations */
	struct fxgmac_buffer_data buf;	/* Payload locations */
	unsigned short hdr_len;		/* Length of received header */
	unsigned short len;		/* Length of received packet */
};

struct fxgmac_desc_data {
	struct fxgmac_dma_desc *dma_desc;  /* Virtual address of descriptor */
	dma_addr_t dma_desc_addr;          /* DMA address of descriptor */
	struct sk_buff *skb;               /* Virtual address of SKB */
	dma_addr_t skb_dma;                /* DMA address of SKB data */
	unsigned int skb_dma_len;          /* Length of SKB DMA area */

	/* Tx/Rx -related data */
	struct fxgmac_tx_desc_data tx;
	struct fxgmac_rx_desc_data rx;

	unsigned int mapped_as_page;
};

struct fxgmac_ring {
	struct fxgmac_pkt_info pkt_info;  /* packet related information */

	/* Virtual/DMA addresses of DMA descriptor list */
	struct fxgmac_dma_desc *dma_desc_head;
	dma_addr_t dma_desc_head_addr;
	unsigned int dma_desc_count;

	/* Array of descriptor data corresponding the DMA descriptor
	 * (always use the FXGMAC_GET_DESC_DATA macro to access this data)
	 */
	struct fxgmac_desc_data *desc_data_head;

	/* Page allocation for RX buffers */
	struct fxgmac_page_alloc rx_hdr_pa;
	struct fxgmac_page_alloc rx_buf_pa;

	/* Ring index values
	 * cur  - Tx: index of descriptor to be used for current transfer
	 *        Rx: index of descriptor to check for packet availability
	 * dirty - Tx: index of descriptor to check for transfer complete
	 *         Rx: index of descriptor to check for buffer reallocation
	 */
	unsigned int cur;
	unsigned int dirty;

	struct {
		unsigned int xmit_more;
		unsigned int queue_stopped;
		unsigned short cur_mss;
		unsigned short cur_vlan_ctag;
	} tx;
} ____cacheline_aligned;

struct fxgmac_channel {
	char name[16];

	/* Address of private data area for device */
	struct fxgmac_pdata *priv;

	/* Queue index and base address of queue's DMA registers */
	unsigned int queue_index;

	/* Per channel interrupt irq number */
	u32 dma_irq_rx;
	char dma_irq_rx_name[IFNAMSIZ + 32];
	u32 dma_irq_tx;
	char dma_irq_tx_name[IFNAMSIZ + 32];

	/* ndev related settings */
	struct napi_struct napi_tx;
	struct napi_struct napi_rx;

	void __iomem *dma_regs;
	struct fxgmac_ring *tx_ring;
	struct fxgmac_ring *rx_ring;
} ____cacheline_aligned;

/* This structure contains flags that indicate what hardware features
 * or configurations are present in the device.
 */
struct fxgmac_hw_features {
	unsigned int version;		/* HW Version */

	/* HW Feature Register0 */
	unsigned int phyifsel;		/* PHY interface support */
	unsigned int vlhash;		/* VLAN Hash Filter */
	unsigned int sma;		/* SMA(MDIO) Interface */
	unsigned int rwk;		/* PMT remote wake-up packet */
	unsigned int mgk;		/* PMT magic packet */
	unsigned int mmc;		/* RMON module */
	unsigned int aoe;		/* ARP Offload */
	unsigned int ts;		/* IEEE 1588-2008 Advanced Timestamp */
	unsigned int eee;		/* Energy Efficient Ethernet */
	unsigned int tx_coe;		/* Tx Checksum Offload */
	unsigned int rx_coe;		/* Rx Checksum Offload */
	unsigned int addn_mac;		/* Additional MAC Addresses */
	unsigned int ts_src;		/* Timestamp Source */
	unsigned int sa_vlan_ins;	/* Source Address or VLAN Insertion */

	/* HW Feature Register1 */
	unsigned int rx_fifo_size;	/* MTL Receive FIFO Size */
	unsigned int tx_fifo_size;	/* MTL Transmit FIFO Size */
	unsigned int adv_ts_hi;		/* Advance Timestamping High Word */
	unsigned int dma_width;		/* DMA width */
	unsigned int dcb;		/* DCB Feature */
	unsigned int sph;		/* Split Header Feature */
	unsigned int tso;		/* TCP Segmentation Offload */
	unsigned int dma_debug;		/* DMA Debug Registers */
	unsigned int rss;		/* Receive Side Scaling */
	unsigned int tc_cnt;		/* Number of Traffic Classes */
	unsigned int avsel;		/* AV Feature Enable */
	unsigned int ravsel;		/* Rx Side Only AV Feature Enable */
	unsigned int hash_table_size;	/* Hash Table Size */
	unsigned int l3l4_filter_num;	/* Number of L3-L4 Filters */

	/* HW Feature Register2 */
	unsigned int rx_q_cnt;		/* Number of MTL Receive Queues */
	unsigned int tx_q_cnt;		/* Number of MTL Transmit Queues */
	unsigned int rx_ch_cnt;		/* Number of DMA Receive Channels */
	unsigned int tx_ch_cnt;		/* Number of DMA Transmit Channels */
	unsigned int pps_out_num;	/* Number of PPS outputs */
	unsigned int aux_snap_num;	/* Number of Aux snapshot inputs */

	u32 hwfr3;			/* HW Feature Register3 */
};

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

	struct fxgmac_hw_features hw_feat;	/* Hardware features */
	void __iomem *hw_addr;			/* Registers base */

	/* Rings for Tx/Rx on a DMA channel */
	struct fxgmac_channel *channel_head;
	unsigned int channel_count;
	unsigned int rx_ring_count;
	unsigned int rx_desc_count;
	unsigned int rx_q_count;
#define FXGMAC_TX_1_RING	1
#define FXGMAC_TX_1_Q		1
	unsigned int tx_desc_count;

	unsigned long sysclk_rate;		/* Device clocks */
	unsigned int pblx8;			/* Tx/Rx common settings */

	/* Tx settings */
	unsigned int tx_sf_mode;
	unsigned int tx_threshold;
	unsigned int tx_pbl;
	unsigned int tx_osp_mode;

	/* Rx settings */
	unsigned int rx_sf_mode;
	unsigned int rx_threshold;
	unsigned int rx_pbl;

	/* Tx coalescing settings */
	unsigned int tx_usecs;
	unsigned int tx_frames;

	/* Rx coalescing settings */
	unsigned int rx_riwt;
	unsigned int rx_usecs;
	unsigned int rx_frames;

	/* Flow control settings */
	unsigned int tx_pause;
	unsigned int rx_pause;

	unsigned int rx_buf_size;	/* Current Rx buffer size */

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

	/* ndev related settings */
	unsigned char mac_addr[ETH_ALEN];

	int mac_speed;
	int mac_duplex;

	u32 msg_enable;
	u32 reg_nonstick[(MSI_PBA - GLOBAL_CTRL0) >> 2];

	struct work_struct restart_work;
	enum fxgmac_dev_state dev_state;
#define FXGMAC_POWER_STATE_DOWN			0
#define FXGMAC_POWER_STATE_UP			1
	unsigned long power_state;
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

static inline u32 fxgmac_mtl_io_rd(struct fxgmac_pdata *priv, u8 n, u32 reg)
{
	return fxgmac_io_rd(priv, reg + n * MTL_Q_INC);
}

static inline u32
fxgmac_mtl_rd_bits(struct fxgmac_pdata *priv, u8 n, u32 reg, u32 mask)
{
	return fxgmac_io_rd_bits(priv,  reg + n * MTL_Q_INC, mask);
}

static inline void
fxgmac_mtl_io_wr(struct fxgmac_pdata *priv, u8 n, u32 reg, u32 set)
{
	return fxgmac_io_wr(priv,  reg + n * MTL_Q_INC, set);
}

static inline void
fxgmac_mtl_wr_bits(struct fxgmac_pdata *priv, u8 n, u32 reg, u32 mask, u32 set)
{
	return fxgmac_io_wr_bits(priv,  reg + n * MTL_Q_INC, mask, set);
}

static inline u32 fxgmac_dma_io_rd(struct fxgmac_channel *channel, u32 reg)
{
	return ioread32(channel->dma_regs + reg);
}

static inline u32
fxgmac_dma_rd_bits(struct fxgmac_channel *channel, u32 reg, u32 mask)
{
	u32 cfg = fxgmac_dma_io_rd(channel, reg);

	return FIELD_GET(mask, cfg);
}

static inline void
fxgmac_dma_io_wr(struct fxgmac_channel *channel, u32 reg, u32 set)
{
	iowrite32(set, channel->dma_regs + reg);
}

static inline void
fxgmac_dma_wr_bits(struct fxgmac_channel *channel, u32 reg, u32 mask, u32 set)
{
	u32 cfg = fxgmac_dma_io_rd(channel, reg);

	cfg &= ~mask;
	cfg |= FIELD_PREP(mask, set);
	fxgmac_dma_io_wr(channel, reg, cfg);
}

static inline u32 fxgmac_desc_rd_bits(__le32 desc, u32 mask)
{
	return FIELD_GET(mask, le32_to_cpu(desc));
}

static inline void fxgmac_desc_wr_bits(__le32 *desc, u32 mask, u32 set)
{
	u32 cfg = le32_to_cpu(*desc);

	cfg &= ~mask;
	cfg |= FIELD_PREP(mask, set);
	*desc = cpu_to_le32(cfg);
}

#endif /* YT6801_TYPE_H */
