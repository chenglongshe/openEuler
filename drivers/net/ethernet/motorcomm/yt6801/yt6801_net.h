/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (c) 2022 - 2024 Motorcomm Electronic Technology Co.,Ltd. */

#ifndef YT6801_NET_H
#define YT6801_NET_H

#include "yt6801.h"

/* flags for ipv6 NS offload address, local link or Global unicast */
#define FXGMAC_NS_IFA_LOCAL_LINK	1
#define FXGMAC_NS_IFA_GLOBAL_UNICAST	2

unsigned int fxgmac_get_netdev_ip4addr(struct fxgmac_pdata *pdata);
unsigned char *fxgmac_get_netdev_ip6addr(struct fxgmac_pdata *pdata,
					 unsigned char *ipval,
					 unsigned char *ip6addr_solicited,
					 unsigned int ifa_flag);
int fxgmac_start(struct fxgmac_pdata *pdata);
void fxgmac_stop(struct fxgmac_pdata *pdata);
void fxgmac_restart(struct fxgmac_pdata *pdata);
void fxgmac_dbg_pkt(struct fxgmac_pdata *pdata, struct sk_buff *skb,
		    bool tx_rx);
void fxgmac_tx_start_xmit(struct fxgmac_channel *channel,
			  struct fxgmac_ring *ring);
void fxgmac_free_tx_data(struct fxgmac_pdata *pdata);
void fxgmac_free_rx_data(struct fxgmac_pdata *pdata);
int fxgmac_config_wol(struct fxgmac_pdata *pdata, bool en);
int fxgmac_net_powerup(struct fxgmac_pdata *pdata);
int fxgmac_net_powerdown(struct fxgmac_pdata *pdata, bool wake_en);
int fxgmac_drv_probe(struct device *dev, struct fxgmac_resources *res);

#endif /* YT6801_NET_H */
