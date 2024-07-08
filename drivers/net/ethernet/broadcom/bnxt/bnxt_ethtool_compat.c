// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2021 Broacom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */
#include "bnxt_ethtool.c"

#ifndef HAVE_ETHTOOL_LINK_KSETTINGS
int bnxt_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct bnxt *bp = netdev_priv(dev);
	struct ethtool_link_ksettings ks;
	int rc;

	memset(&ks, 0, sizeof(ks));
	rc = bnxt_get_link_ksettings(dev, &ks);
	if (rc)
		return rc;

	cmd->supported = ks.link_modes.supported[0];
	cmd->advertising = ks.link_modes.advertising[0];
	cmd->lp_advertising = ks.link_modes.lp_advertising[0];
	ethtool_cmd_speed_set(cmd, ks.base.speed);
	cmd->duplex = ks.base.duplex;
	cmd->autoneg = ks.base.autoneg;
	cmd->port = ks.base.port;
	cmd->phy_address = ks.base.phy_address;
	if (bp->link_info.transceiver ==
	    PORT_PHY_QCFG_RESP_XCVR_PKG_TYPE_XCVR_INTERNAL)
		cmd->transceiver = XCVR_INTERNAL;
	else
		cmd->transceiver = XCVR_EXTERNAL;

	return 0;
}

static void bnxt_fw_to_ethtool_support_spds(struct bnxt_link_info *link_info,
					    struct ethtool_link_ksettings *ks)
{
	u16 fw_speeds = link_info->support_speeds;
	u32 supported;

	supported = _bnxt_fw_to_ethtool_adv_spds(fw_speeds, 0);
	ks->link_modes.supported[0] = supported | SUPPORTED_Pause |
				      SUPPORTED_Asym_Pause;
}

int bnxt_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct bnxt *bp = netdev_priv(dev);
	struct ethtool_link_ksettings ks;

	memset(&ks, 0, sizeof(ks));
	if (cmd->autoneg == AUTONEG_ENABLE) {
		bnxt_fw_to_ethtool_support_spds(&bp->link_info, &ks);

		if (!ks.link_modes.supported) {
			netdev_err(dev, "Autoneg not supported\n");
			return -EINVAL;
		}
		if (cmd->advertising & ~(ks.link_modes.supported[0] |
					 ADVERTISED_Autoneg |
					 ADVERTISED_TP | ADVERTISED_FIBRE)) {
			netdev_err(dev, "Unsupported advertising mask (adv: 0x%x)\n",
				   cmd->advertising);
			return -EINVAL;
		}
	} else {
		/* If received a request for an unknown duplex, assume full*/
		if (cmd->duplex == DUPLEX_UNKNOWN)
			cmd->duplex = DUPLEX_FULL;
	}

	ks.link_modes.advertising[0] = cmd->advertising;
	ks.base.speed = ethtool_cmd_speed(cmd);
	ks.base.duplex = cmd->duplex;
	ks.base.autoneg = cmd->autoneg;
	return bnxt_set_link_ksettings(dev, &ks);
}
#endif

#ifndef HAVE_ETHTOOL_PARAMS_FROM_LINK_MODE
#define ETHTOOL_LINK_MODE(speed, type, duplex)				\
	ETHTOOL_LINK_MODE_ ## speed ## base ## type ## _ ## duplex ## _BIT

#include "bnxt_compat_link_modes.c"

void
ethtool_params_from_link_mode(struct ethtool_link_ksettings *link_ksettings,
			      enum ethtool_link_mode_bit_indices link_mode)
{
	const struct link_mode_info *link_info;

	if (WARN_ON_ONCE(link_mode >= ARRAY_SIZE(link_mode_params)))
		return;

	link_info = &link_mode_params[link_mode];
	link_ksettings->base.speed = link_info->speed;
#ifdef HAVE_ETHTOOL_LANES
	link_ksettings->lanes = link_info->lanes;
#endif
	link_ksettings->base.duplex = link_info->duplex;
#ifdef HAVE_ETHTOOL_LINK_MODE
	link_ksettings->link_mode = link_mode;
#endif
}
#endif
