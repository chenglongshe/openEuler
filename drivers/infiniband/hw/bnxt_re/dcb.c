// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2022, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Inc. and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: Enables netlink Data center bridging (DCB)
 */

#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/rtnetlink.h>
#include <net/dcbnl.h>

#include "bnxt_re.h"
#include "qplib_sp.h"

#ifdef CONFIG_BNXT_DCB
u8 bnxt_re_get_priority_mask(struct bnxt_re_dev *rdev, u8 selector)
{
	struct net_device *netdev;
	struct dcb_app app;
	u8 prio_map = 0, tmp_map = 0;

	netdev = rdev->en_dev->net;
	memset(&app, 0, sizeof(app));
	if (selector & IEEE_8021QAZ_APP_SEL_ETHERTYPE) {
		app.selector = IEEE_8021QAZ_APP_SEL_ETHERTYPE;
		app.protocol = BNXT_RE_ROCE_V1_ETH_TYPE;
		tmp_map = dcb_ieee_getapp_mask(netdev, &app);
		prio_map = tmp_map;
	}

	if (selector & IEEE_8021QAZ_APP_SEL_DGRAM) {
		app.selector = IEEE_8021QAZ_APP_SEL_DGRAM;
		app.protocol = BNXT_RE_ROCE_V2_PORT_NO;
		tmp_map = dcb_ieee_getapp_mask(netdev, &app);
		prio_map |= tmp_map;
	}

	return prio_map;
}

static int __bnxt_re_del_app(struct net_device *dev,
			     u8 sel, u8 prio, u16 protocol)
{
	struct dcb_app app;

	app.selector = sel;
	app.priority = prio;
	app.protocol = protocol;
	return dev->dcbnl_ops->ieee_delapp(dev, &app);
}

static int bnxt_re_del_app(struct bnxt_re_dev *rdev,
			   struct net_device *dev)
{
	if (is_qport_service_type_supported(rdev))
		__bnxt_re_del_app(dev, IEEE_8021QAZ_APP_SEL_DSCP,
				  BNXT_RE_DEFAULT_CNP_PRI,
				  BNXT_RE_DEFAULT_CNP_DSCP);


	__bnxt_re_del_app(dev, IEEE_8021QAZ_APP_SEL_DSCP,
			  BNXT_RE_DEFAULT_ROCE_PRI,
			  BNXT_RE_DEFAULT_ROCE_DSCP);
	__bnxt_re_del_app(dev, IEEE_8021QAZ_APP_SEL_DGRAM,
			  BNXT_RE_DEFAULT_ROCE_PRI,
			  BNXT_RE_ROCE_V2_PORT_NO);
	return 0;
}

static int __bnxt_re_set_app(struct net_device *dev,
			     u8 sel, u8 prio, u16 protocol)
{
	struct dcb_app app;

	app.selector = sel;
	app.priority = prio;
	app.protocol = protocol;
	return dev->dcbnl_ops->ieee_setapp(dev, &app);
}

static int bnxt_re_set_default_app(struct bnxt_re_dev *rdev,
				   struct net_device *dev)
{
	__bnxt_re_set_app(dev, IEEE_8021QAZ_APP_SEL_DGRAM,
			  BNXT_RE_DEFAULT_ROCE_PRI,
			  BNXT_RE_ROCE_V2_PORT_NO);
	__bnxt_re_set_app(dev, IEEE_8021QAZ_APP_SEL_DSCP,
			  BNXT_RE_DEFAULT_ROCE_PRI,
			  BNXT_RE_DEFAULT_ROCE_DSCP);

	if (is_qport_service_type_supported(rdev))
		__bnxt_re_set_app(dev, IEEE_8021QAZ_APP_SEL_DSCP,
				  BNXT_RE_DEFAULT_CNP_PRI,
				  BNXT_RE_DEFAULT_CNP_DSCP);
	return 0;
}

void bnxt_re_clear_dcb(struct bnxt_re_dev *rdev,
		       struct net_device *dev,
		       struct bnxt_re_tc_rec *tc_rec)
{
	struct bnxt_qplib_cc_param *cc_param = &rdev->cc_param;
	struct ieee_ets ets = {};
	struct ieee_pfc pfc = {};
	u8 roce_prio, cnp_prio;

	if (!dev->dcbnl_ops)
		return;

	cnp_prio = cc_param->alt_vlan_pcp;
	roce_prio = cc_param->roce_pri;

	if (dev->dcbnl_ops->ieee_getets)
		dev->dcbnl_ops->ieee_getets(dev, &ets);
	if (dev->dcbnl_ops->ieee_getpfc)
		dev->dcbnl_ops->ieee_getpfc(dev, &pfc);

	if (dev->dcbnl_ops->ieee_delapp)
		bnxt_re_del_app(rdev, dev);

	if (dev->dcbnl_ops->ieee_setpfc) {
		if (pfc.pfc_en &  (1 << BNXT_RE_DEFAULT_ROCE_PRI)) {
			pfc.pfc_en &= ~(1 << BNXT_RE_DEFAULT_ROCE_PRI);
			rtnl_lock();
			dev->dcbnl_ops->ieee_setpfc(dev, &pfc);
			rtnl_unlock();
		}
	}

	if (dev->dcbnl_ops->ieee_setets) {
		ets.tc_tx_bw[tc_rec->tc_roce] = 0;
		ets.tc_tsa[tc_rec->tc_roce] = IEEE_8021QAZ_TSA_STRICT;
		ets.prio_tc[roce_prio] = 0;
		if (is_qport_service_type_supported(rdev))
			ets.prio_tc[cnp_prio] = 0;

		rtnl_lock();
		dev->dcbnl_ops->ieee_setets(dev, &ets);
		rtnl_unlock();
		if (!is_qport_service_type_supported(rdev))
			(void)bnxt_re_setup_cnp_cos(rdev, true);
	}
}

int bnxt_re_setup_dcb(struct bnxt_re_dev *rdev,
		      struct net_device *dev,
		      struct bnxt_re_tc_rec *tc_rec,
		      u16 port_id)
{
	struct ieee_ets ets = {};
	struct ieee_pfc pfc = {};
	int rc;

	if (!dev->dcbnl_ops)
		return -EOPNOTSUPP;

	rc =  bnxt_re_query_hwrm_qportcfg(rdev, tc_rec, port_id);
	if (rc) {
		dev_err(rdev_to_dev(rdev), "Failed to query port config rc:%d",
			rc);
		return rc;
	}

	if (dev->dcbnl_ops->ieee_getets) {
		rc = dev->dcbnl_ops->ieee_getets(dev, &ets);
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Failed to getets rc:%d",
				rc);
			return rc;
		}
	}

	if (dev->dcbnl_ops->ieee_getpfc) {
		rc = dev->dcbnl_ops->ieee_getpfc(dev, &pfc);
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Failed to getpfc rc:%d",
				rc);
			return rc;
		}
	}

	if (dev->dcbnl_ops->ieee_setets) {
		ets.tc_tx_bw[0] = BNXT_RE_DEFAULT_L2_BW;
		ets.tc_tx_bw[tc_rec->tc_roce] = BNXT_RE_DEFAULT_ROCE_BW;

		ets.tc_tsa[0] = IEEE_8021QAZ_TSA_ETS;
		ets.tc_tsa[tc_rec->tc_roce] = IEEE_8021QAZ_TSA_ETS;

		ets.prio_tc[BNXT_RE_DEFAULT_ROCE_PRI] = tc_rec->tc_roce;
		if (is_qport_service_type_supported(rdev))
			ets.prio_tc[BNXT_RE_DEFAULT_CNP_PRI] = tc_rec->tc_cnp;

		rtnl_lock();
		rc = dev->dcbnl_ops->ieee_setets(dev, &ets);
		rtnl_unlock();
		if (rc) {
			if (rc != -EBUSY)
				dev_err(rdev_to_dev(rdev), "Fail to setets rc:%d", rc);
			return rc;
		}
		if (!is_qport_service_type_supported(rdev)) {
			/* Setup CNP COS queue using an HWRM for older HWRM */
			rc = bnxt_re_setup_cnp_cos(rdev, false);
			if (rc) {
				dev_err(rdev_to_dev(rdev),
					"Failed to set cnp cos rc:%d", rc);
				goto clear;
			}
		}
	}

	if (dev->dcbnl_ops->ieee_setpfc) {
		/* Default RoCE priority to be enabled = 0x3 */
		pfc.pfc_en = 1 << BNXT_RE_DEFAULT_ROCE_PRI;
		rtnl_lock();
		rc = dev->dcbnl_ops->ieee_setpfc(dev, &pfc);
		rtnl_unlock();
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Fail to setpfc rc:%d", rc);
			goto clear;
		}
	}

	if (dev->dcbnl_ops->ieee_setapp) {
		rc = bnxt_re_set_default_app(rdev, dev);
		if (rc) {
			dev_err(rdev_to_dev(rdev), "Fail to setapp tlvs rc:%d",
				rc);
			goto clear;
		}
	}
	return 0;
clear:
	bnxt_re_clear_dcb(rdev, rdev->en_dev->net, tc_rec);
	return rc;
}
#else
u8 bnxt_re_get_priority_mask(struct bnxt_re_dev *rdev, u8 selector)
{
	return 0;
}

int bnxt_re_setup_dcb(struct bnxt_re_dev *rdev,
		      struct net_device *dev,
		      struct bnxt_re_tc_rec *tc_rec,
		      u16 port_id)
{
	dev_warn(rdev_to_dev(rdev), "CONFIG_DCB is not enabled in Linux\n");
	return 0;
}

void bnxt_re_clear_dcb(struct bnxt_re_dev *rdev,
		       struct net_device *dev,
		       struct bnxt_re_tc_rec *tc_rec)
{
}
#endif /* CONFIG_BNXT_DCB */

