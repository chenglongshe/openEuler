/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#ifndef YT6801_H
#define YT6801_H

#include <linux/dma-mapping.h>
#include <linux/timecounter.h>
#include <linux/pm_wakeup.h>
#include <linux/workqueue.h>
#include <linux/crc32poly.h>
#include <linux/if_vlan.h>
#include <linux/bitrev.h>
#include <linux/bitops.h>
#include <linux/mdio.h>
#include <linux/phy.h>

#ifdef CONFIG_PCI_MSI
#include <linux/pci.h>
#endif

#include "yt6801_type.h"

#define FXGMAC_DRV_VERSION		"1.0.29"
#define FXGMAC_DRV_NAME			"yt6801"
#define FXGMAC_DRV_DESC			"Motorcomm Gigabit Ethernet Driver"

#define FXGMAC_RX_BUF_ALIGN		64
#define FXGMAC_TX_MAX_BUF_SIZE		(0x3fff & ~(FXGMAC_RX_BUF_ALIGN - 1))
#define FXGMAC_RX_MIN_BUF_SIZE		(ETH_FRAME_LEN + ETH_FCS_LEN + VLAN_HLEN)

/* Descriptors required for maximum contiguous TSO/GSO packet */
#define FXGMAC_TX_MAX_SPLIT		((GSO_MAX_SIZE / FXGMAC_TX_MAX_BUF_SIZE) + 1)

/* Maximum possible descriptors needed for a SKB */
#define FXGMAC_TX_MAX_DESC_NR	(MAX_SKB_FRAGS + FXGMAC_TX_MAX_SPLIT + 2)

#define FXGMAC_DMA_STOP_TIMEOUT			5

/* Receive Side Scaling */
#define FXGMAC_RSS_HASH_KEY_SIZE		40
#define FXGMAC_RSS_MAX_TABLE_SIZE		128
#define FXGMAC_RSS_LOOKUP_TABLE_TYPE		0
#define FXGMAC_RSS_HASH_KEY_TYPE		1

#define FXGMAC_JUMBO_PACKET_MTU			9014
#define FXGMAC_DATA_WIDTH			128

#define FXGMAC_MAX_DMA_RX_CHANNELS		4
#define FXGMAC_MAX_DMA_TX_CHANNELS		1
#define FXGMAC_MAX_DMA_CHANNELS                                                \
	(FXGMAC_MAX_DMA_RX_CHANNELS + FXGMAC_MAX_DMA_TX_CHANNELS)

#define FXGMAC_PHY_INT_NUM	1
#define FXGMAC_MSIX_INT_NUMS	(FXGMAC_MAX_DMA_CHANNELS + FXGMAC_PHY_INT_NUM)

/* WOL pattern settings */
#define MAX_PATTERN_SIZE		128	/* pattern length */
#define MAX_PATTERN_COUNT		16	/* pattern count */

struct wol_bitmap_pattern {
	u32 flags;
	u32 pattern_size;
	u32 mask_size;
	u8 mask_info[MAX_PATTERN_SIZE / 8];
	u8 pattern_info[MAX_PATTERN_SIZE];
	u8 pattern_offset;
	u16 pattern_crc;
};

enum _wake_reason {
	WAKE_REASON_NONE = 0,
	WAKE_REASON_MAGIC,
	WAKE_REASON_PATTERNMATCH,
	WAKE_REASON_LINK,
	WAKE_REASON_TCPSYNV4,
	WAKE_REASON_TCPSYNV6,
	/* For wake up method like Link-change, GMAC cannot
	 * identify and need more checking.
	 */
	WAKE_REASON_TBD,
	WAKE_REASON_HW_ERR,
};

enum fxgmac_int {
	FXGMAC_INT_DMA_CH_SR_TI,
	FXGMAC_INT_DMA_CH_SR_TPS,
	FXGMAC_INT_DMA_CH_SR_TBU,
	FXGMAC_INT_DMA_CH_SR_RI,
	FXGMAC_INT_DMA_CH_SR_RBU,
	FXGMAC_INT_DMA_CH_SR_RPS,
	FXGMAC_INT_DMA_CH_SR_TI_RI,
	FXGMAC_INT_DMA_CH_SR_FBE,
	FXGMAC_INT_DMA_ALL,
};

struct fxgmac_stats {
	/* MMC TX counters */
	u64 txoctetcount_gb;
	u64 txframecount_gb;
	u64 txbroadcastframes_g;
	u64 txmulticastframes_g;
	u64 tx64octets_gb;
	u64 tx65to127octets_gb;
	u64 tx128to255octets_gb;
	u64 tx256to511octets_gb;
	u64 tx512to1023octets_gb;
	u64 tx1024tomaxoctets_gb;
	u64 txunicastframes_gb;
	u64 txmulticastframes_gb;
	u64 txbroadcastframes_gb;
	u64 txunderflowerror;
	u64 txsinglecollision_g;
	u64 txmultiplecollision_g;
	u64 txdeferredframes;
	u64 txlatecollisionframes;
	u64 txexcessivecollisionframes;
	u64 txcarriererrorframes;
	u64 txoctetcount_g;
	u64 txframecount_g;
	u64 txexcessivedeferralerror;
	u64 txpauseframes;
	u64 txvlanframes_g;
	u64 txoversize_g;

	/* MMC RX counters */
	u64 rxframecount_gb;
	u64 rxoctetcount_gb;
	u64 rxoctetcount_g;
	u64 rxbroadcastframes_g;
	u64 rxmulticastframes_g;
	u64 rxcrcerror;
	u64 rxalignerror;
	u64 rxrunterror;
	u64 rxjabbererror;
	u64 rxundersize_g;
	u64 rxoversize_g;
	u64 rx64octets_gb;
	u64 rx65to127octets_gb;
	u64 rx128to255octets_gb;
	u64 rx256to511octets_gb;
	u64 rx512to1023octets_gb;
	u64 rx1024tomaxoctets_gb;
	u64 rxunicastframes_g;
	u64 rxlengtherror;
	u64 rxoutofrangetype;
	u64 rxpauseframes;
	u64 rxfifooverflow;
	u64 rxvlanframes_gb;
	u64 rxwatchdogerror;
	u64 rxreceiveerrorframe;
	u64 rxcontrolframe_g;

	/* Extra counters */
	u64 tx_tso_packets;
	u64 rx_split_header_packets;
	u64 tx_process_stopped;
	u64 rx_process_stopped;
	u64 tx_buffer_unavailable;
	u64 rx_buffer_unavailable;
	u64 fatal_bus_error;
	u64 tx_vlan_packets;
	u64 rx_vlan_packets;
	u64 napi_poll_isr;
	u64 napi_poll_txtimer;
	u64 cnt_alive_txtimer;
	u64 ephy_poll_timer_cnt;
	u64 mgmt_int_isr;
};

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

/* Tx-related desc data */
struct fxgmac_tx_desc_data {
	unsigned int packets;		/* BQL packet count */
	unsigned int bytes;		/* BQL byte count */
};

/* Rx-related desc data */
struct fxgmac_rx_desc_data {
	struct fxgmac_buffer_data hdr;	/* Header locations */
	struct fxgmac_buffer_data buf;	/* Payload locations */
	unsigned short hdr_len;		/* Length of received header */
	unsigned short len;		/* Length of received packet */
};

struct fxgmac_pkt_info {
	struct sk_buff *skb;
	unsigned int attributes;
	unsigned int errors;

	/* descriptors needed for this packet */
	unsigned int desc_count;
	unsigned int length;
	unsigned int tx_packets;
	unsigned int tx_bytes;

	unsigned int header_len;
	unsigned int tcp_header_len;
	unsigned int tcp_payload_len;
	unsigned short mss;
	unsigned short vlan_ctag;

	u64 rx_tstamp;
	u32 rss_hash;
	enum pkt_hash_types rss_hash_type;
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
	struct fxgmac_pkt_info pkt_info;  /* Per packet related information */

	/* Virtual/DMA addresses of DMA descriptor list and the total count */
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
	 *  cur   - Tx: index of descriptor to be used for current transfer
	 *          Rx: index of descriptor to check for packet availability
	 *  dirty - Tx: index of descriptor to check for transfer complete
	 *          Rx: index of descriptor to check for buffer reallocation
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
	struct fxgmac_pdata *pdata;

	/* Queue index and base address of queue's DMA registers */
	unsigned int queue_index;

	/* Per channel interrupt irq number */
	u32 dma_irq_rx;
	char dma_irq_rx_name[IFNAMSIZ + 32];
	u32 dma_irq_tx;
	char dma_irq_tx_name[IFNAMSIZ + 32];

	/* Netdev related settings */
	struct napi_struct napi_tx;
	struct napi_struct napi_rx;

	unsigned int saved_ier;
	unsigned int tx_timer_active;
	struct timer_list tx_timer;

	u32 dma_reg_offset;
	struct fxgmac_ring *tx_ring;
	struct fxgmac_ring *rx_ring;
} ____cacheline_aligned;

struct fxgmac_hw_ops {
	void (*pcie_init)(struct fxgmac_pdata *pdata);
	int (*init)(struct fxgmac_pdata *pdata);
	void (*exit)(struct fxgmac_pdata *pdata);
	void (*save_nonstick_reg)(struct fxgmac_pdata *pdata);
	void (*restore_nonstick_reg)(struct fxgmac_pdata *pdata);

	void (*enable_tx)(struct fxgmac_pdata *pdata);
	void (*disable_tx)(struct fxgmac_pdata *pdata);
	void (*enable_rx)(struct fxgmac_pdata *pdata);
	void (*disable_rx)(struct fxgmac_pdata *pdata);
	int (*dev_read)(struct fxgmac_channel *channel);
	void (*dev_xmit)(struct fxgmac_channel *channel);

	void (*enable_channel_irq)(struct fxgmac_channel *channel,
				   enum fxgmac_int int_id);
	void (*disable_channel_irq)(struct fxgmac_channel *channel,
				    enum fxgmac_int int_id);
	void (*set_interrupt_moderation)(struct fxgmac_pdata *pdata);
	void (*enable_msix_one_irq)(struct fxgmac_pdata *pdata, u32 intid);
	void (*disable_msix_one_irq)(struct fxgmac_pdata *pdata, u32 intid);
	void (*enable_mgm_irq)(struct fxgmac_pdata *pdata);
	void (*disable_mgm_irq)(struct fxgmac_pdata *pdata);

	void (*dismiss_all_int)(struct fxgmac_pdata *pdata);
	void (*clear_misc_int_status)(struct fxgmac_pdata *pdata);

	void (*set_mac_address)(struct fxgmac_pdata *pdata, u8 *addr);
	void (*set_mac_hash)(struct fxgmac_pdata *pdata);
	void (*config_rx_mode)(struct fxgmac_pdata *pdata);
	void (*enable_rx_csum)(struct fxgmac_pdata *pdata);
	void (*disable_rx_csum)(struct fxgmac_pdata *pdata);
	void (*config_tso)(struct fxgmac_pdata *pdata);
	u32 (*calculate_max_checksum_size)(struct fxgmac_pdata *pdata);
	void (*config_mac_speed)(struct fxgmac_pdata *pdata);

	/* PHY Control */
	void (*reset_phy)(struct fxgmac_pdata *pdata);
	void (*release_phy)(struct fxgmac_pdata *pdata);
	int (*write_phy_reg)(struct fxgmac_pdata *pdata, u32 val, u32 data);
	int (*read_phy_reg)(struct fxgmac_pdata *pdata, u32 val);

	/* Vlan related config */
	void (*enable_rx_vlan_stripping)(struct fxgmac_pdata *pdata);
	void (*disable_rx_vlan_stripping)(struct fxgmac_pdata *pdata);
	void (*enable_rx_vlan_filtering)(struct fxgmac_pdata *pdata);
	void (*disable_rx_vlan_filtering)(struct fxgmac_pdata *pdata);
	void (*update_vlan_hash_table)(struct fxgmac_pdata *pdata);

	/* RX coalescing */
	void (*config_rx_coalesce)(struct fxgmac_pdata *pdata);
	unsigned int (*usec_to_riwt)(struct fxgmac_pdata *pdata,
				     unsigned int usec);

	/* MMC statistics */
	void (*read_mmc_stats)(struct fxgmac_pdata *pdata);

	/* Receive Side Scaling */
	void (*enable_rss)(struct fxgmac_pdata *pdata);
	void (*disable_rss)(struct fxgmac_pdata *pdata);
	u32 (*get_rss_options)(struct fxgmac_pdata *pdata);
	void (*set_rss_options)(struct fxgmac_pdata *pdata);
	void (*set_rss_hash_key)(struct fxgmac_pdata *pdata, const u8 *key);
	void (*write_rss_lookup_table)(struct fxgmac_pdata *pdata);

	/* Wake */
	void (*enable_wake_pattern)(struct fxgmac_pdata *pdata);
	void (*disable_wake_pattern)(struct fxgmac_pdata *pdata);
	void (*disable_arp_offload)(struct fxgmac_pdata *pdata);
	void (*disable_wake_magic_pattern)(struct fxgmac_pdata *pdata);
	int (*set_wake_pattern)(struct fxgmac_pdata *pdata,
				struct wol_bitmap_pattern *wol_pattern,
				u32 pattern_cnt);
	void (*enable_wake_magic_pattern)(struct fxgmac_pdata *pdata);
	void (*enable_wake_link_change)(struct fxgmac_pdata *pdata);
	void (*disable_wake_link_change)(struct fxgmac_pdata *pdata);

	/* Power management */
	int (*pre_power_down)(struct fxgmac_pdata *pdata);
	void (*config_power_down)(struct fxgmac_pdata *pdata, bool wake_en);
	void (*config_power_up)(struct fxgmac_pdata *pdata);
};

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

enum fxgmac_task_flag {
	FXGMAC_TASK_FLAG_DOWN = 0,
	FXGMAC_TASK_FLAG_RESET,
	FXGMAC_TASK_FLAG_LINK_CHG,
	FXGMAC_TASK_FLAG_MAX
};

struct fxgmac_pdata {
	struct net_device *netdev;
	struct device *dev;
	struct phy_device *phydev;

	struct fxgmac_hw_features hw_feat;	/* Hardware features */
	struct fxgmac_hw_ops hw_ops;
	void __iomem *hw_addr;			/* Registers base */
	struct fxgmac_stats stats;		/* Device statistics */

	/* Rings for Tx/Rx on a DMA channel */
	struct fxgmac_channel *channel_head;
	unsigned int channel_count;
	unsigned int rx_ring_count;
	unsigned int rx_desc_count;
	unsigned int rx_q_count;
#define FXGMAC_TX_1_RING	1
#define FXGMAC_TX_1_Q	1
	unsigned int tx_desc_count;

	unsigned long sysclk_rate;		/* Device clocks */
	unsigned int pblx8;			/* Tx/Rx common settings */
	unsigned int crc_check;

	/* Tx settings */
	unsigned int tx_sf_mode;
	unsigned int tx_threshold;
	unsigned int tx_pbl;
	unsigned int tx_osp_mode;
	unsigned int tx_hang_restart_queuing;

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

	unsigned int rx_buf_size;		/* Current Rx buffer size */

	/* Flow control settings */
	unsigned int tx_pause;
	unsigned int rx_pause;

	/* Jumbo frames */
	unsigned int mtu;
	unsigned int jumbo;

	/* vlan */
	unsigned int vlan;
	unsigned int vlan_exist;
	unsigned int vlan_filter;
	unsigned int vlan_strip;

	/* Device interrupt */
	int dev_irq;
	unsigned int per_channel_irq;
	u32 channel_irq[FXGMAC_MAX_DMA_CHANNELS];
	int misc_irq;
	char misc_irq_name[IFNAMSIZ + 32];
	struct msix_entry *msix_entries;
#define FXGMAC_FLAG_INTERRUPT_POS		0
#define FXGMAC_FLAG_INTERRUPT_LEN		5
#define FXGMAC_FLAG_MSI_CAPABLE			BIT(0)
#define FXGMAC_FLAG_MSI_ENABLED			BIT(1)
#define FXGMAC_FLAG_MSIX_CAPABLE		BIT(2)
#define FXGMAC_FLAG_MSIX_ENABLED		BIT(3)
#define FXGMAC_FLAG_LEGACY_ENABLED		BIT(4)
#define FXGMAC_FLAG_RX_NAPI_POS			18
#define FXGMAC_FLAG_RX_NAPI_LEN			4
#define FXGMAC_FLAG_PER_RX_NAPI_LEN		1
#define FXGMAC_FLAG_RX_IRQ_POS			22
#define FXGMAC_FLAG_RX_IRQ_LEN			4
#define FXGMAC_FLAG_PER_RX_IRQ_LEN		1
#define FXGMAC_FLAG_TX_NAPI_POS			26
#define FXGMAC_FLAG_TX_NAPI_LEN			1
#define FXGMAC_FLAG_TX_IRQ_POS			27
#define FXGMAC_FLAG_TX_IRQ_LEN			1
#define FXGMAC_FLAG_MISC_NAPI_POS		28
#define FXGMAC_FLAG_MISC_NAPI_LEN		1
#define FXGMAC_FLAG_MISC_IRQ_POS		29
#define FXGMAC_FLAG_MISC_IRQ_LEN		1
#define FXGMAC_FLAG_LEGACY_NAPI_POS		30
#define FXGMAC_FLAG_LEGACY_NAPI_LEN		1
#define FXGMAC_FLAG_LEGACY_IRQ_POS		31
#define FXGMAC_FLAG_LEGACY_IRQ_LEN		1
	u32 int_flags;

	/* Interrupt Moderation */
	unsigned int intr_mod;
	unsigned int intr_mod_timer;

	/* Netdev related settings */
	unsigned char mac_addr[ETH_ALEN];
	netdev_features_t netdev_features;
	struct napi_struct napi;
	struct napi_struct napi_misc;

	/* Filtering support */
	unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];

	/* Receive Side Scaling settings */
	unsigned int rss;
	u8 rss_key[FXGMAC_RSS_HASH_KEY_SIZE];
	u32 rss_table[FXGMAC_RSS_MAX_TABLE_SIZE];
#define FXGMAC_RSS_IP4TE		BIT(0)
#define FXGMAC_RSS_TCP4TE		BIT(1)
#define FXGMAC_RSS_UDP4TE		BIT(2)
#define FXGMAC_RSS_IP6TE		BIT(3)
#define FXGMAC_RSS_TCP6TE		BIT(4)
#define FXGMAC_RSS_UDP6TE		BIT(5)
	u32 rss_options;

	int phy_speed;
	int phy_duplex;
	int phy_autoeng;
	bool phy_link;

	u32 msg_enable;
	u32 reg_nonstick[(MSI_PBA - GLOBAL_CTRL0) >> 2];

	u32 wol;
	struct wol_bitmap_pattern pattern[MAX_PATTERN_COUNT];

	struct work_struct restart_work;
	DECLARE_BITMAP(task_flags, FXGMAC_TASK_FLAG_MAX);

	enum fxgmac_dev_state dev_state;
#define FXGMAC_POWER_STATE_DOWN			0
#define FXGMAC_POWER_STATE_UP			1
	unsigned long powerstate;
	struct mutex mutex; /* Driver lock */

	char drv_name[32];
	char drv_ver[32];
};

void fxgmac_hw_ops_init(struct fxgmac_hw_ops *hw_ops);
int fxgmac_phy_irq_enable(struct fxgmac_pdata *pdata, bool clear_phy_interrupt);
const struct ethtool_ops *fxgmac_get_ethtool_ops(void);

#endif /* YT6801_H */
