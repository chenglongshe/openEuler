// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2023, Broadcom. All rights reserved.  The term
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
 *
 * Description: Enables configfs interface
 */

#include "configfs.h"
#include "bnxt.h"

static const char *mode_name[] = {"DCQCN-D", "TCP", "Invalid"};
static const char *mode_name_p5[] = {"DCQCN-D", "DCQCN-P", "Invalid"};

static const char *_get_mode_str(u8 mode, bool is_p5)
{
	return is_p5 ?  mode_name_p5[mode] : mode_name[mode];
}

static struct bnxt_re_dev *__get_rdev_from_name(const char *name)
{
	struct bnxt_re_dev *rdev;
	u8 found = false;

	mutex_lock(&bnxt_re_mutex);
	list_for_each_entry(rdev, &bnxt_re_dev_list, list) {
		if (!strcmp(name, rdev->ibdev.name)) {
			found = true;
			break;
		}
	}
	mutex_unlock(&bnxt_re_mutex);

	return found ? rdev : ERR_PTR(-ENODEV);
}

static struct bnxt_re_dev * bnxt_re_get_valid_rdev(struct bnxt_re_cc_group *ccgrp)
{
	struct bnxt_re_dev *rdev = NULL;

	if (ccgrp->portgrp && ccgrp->portgrp->devgrp)
		rdev = __get_rdev_from_name(ccgrp->portgrp->devgrp->name);

	if (!rdev || (PTR_ERR(rdev) == -ENODEV))
	{
		pr_err("bnxt_re: %s : Invalid rdev received rdev = %p\n",
		       __func__, ccgrp->rdev);
		return NULL;
	}


	if (ccgrp->rdev != rdev)
		ccgrp->rdev = rdev;

	return rdev;
}

static int bnxt_re_is_dscp_mapping_set(u32 mask)
{
	return (mask &
		(CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TOS_DSCP |
		 CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_TOS_DSCP));
}

static int bnxt_re_is_pri_mapping_set(struct bnxt_re_dev *rdev)
{
	return rdev->cc_param.cur_mask & BNXT_QPLIB_CC_PARAM_MASK_ROCE_PRI;
}

static int bnxt_re_init_d2p_map(struct bnxt_re_dev *rdev,
				struct bnxt_re_dscp2pri *d2p)
{
	u32 cc_mask;
	int mapcnt = 0;

	cc_mask = rdev->cc_param.mask;

	if (!bnxt_re_is_dscp_mapping_set(rdev->cc_param.mask))
		goto bail;

	if (cc_mask & CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TOS_DSCP ||
	    cc_mask & BNXT_QPLIB_CC_PARAM_MASK_ROCE_PRI) {
		d2p->dscp = rdev->cc_param.tos_dscp;
		d2p->pri = rdev->cc_param.roce_pri;
		d2p->mask = 0x3F;
		mapcnt++;
		d2p++;
	}

	if (cc_mask & CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_TOS_DSCP ||
	    cc_mask & CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_VLAN_PCP) {
		d2p->dscp = rdev->cc_param.alt_tos_dscp;
		d2p->pri = rdev->cc_param.alt_vlan_pcp;
		d2p->mask = 0x3F;
		mapcnt++;
	}
bail:

	return mapcnt;
}

static int __bnxt_re_clear_dscp(struct bnxt_re_dev *rdev, u16 portid)
{
	struct bnxt_re_dscp2pri d2p[8] = {};
	u16 count = 8;
	int rc = 0;
	u16 i;

	/* Get older values to be reseted. Set mask to 0 */
	rc = bnxt_re_query_hwrm_dscp2pri(rdev, d2p, &count, portid);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to query dscp on pci function %d\n",
			bnxt_re_dev_pcifn_id(rdev));
		goto bail;
	}
	if (!count)
		goto bail;

	/* Clear mask of all d2p mapping in HW */
	for (i = 0; i < count;  i++)
		d2p[i].mask = 0;

	rc = bnxt_re_set_hwrm_dscp2pri(rdev, d2p, count, portid);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to clear dscp on pci function %d\n",
			bnxt_re_dev_pcifn_id(rdev));
		goto bail;
	}
bail:
	return rc;
}

int bnxt_re_clear_dscp(struct bnxt_re_dev *rdev)
{
	int rc = 0;
	u16 portid;

	/*
	 * Target ID to be specified.
	 * 0xFFFF - if issued for the same function
	 * function_id - if issued for another function
	 */
	portid = 0xFFFF;
	rc = __bnxt_re_clear_dscp(rdev, portid);
	if (rc)
		goto bail;

	if (rdev->binfo) {
		/* Function id of the second function to be
		 * specified.
		 */
		portid = (PCI_FUNC(rdev->binfo->pdev2->devfn) + 1);
		rc = __bnxt_re_clear_dscp(rdev, portid);
		if (rc)
			goto bail;
	}
bail:
	return rc;
}

static int __bnxt_re_setup_dscp(struct bnxt_re_dev *rdev, u16 portid)
{
	struct bnxt_re_dscp2pri d2p[2] = {};
	int rc = 0, mapcnt = 0;

	/*Init dscp to pri map */
	mapcnt = bnxt_re_init_d2p_map(rdev, d2p);
	if (!mapcnt)
		goto bail;
	rc = bnxt_re_set_hwrm_dscp2pri(rdev, d2p, mapcnt, portid);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to updated dscp on pci function %d\n",
			bnxt_re_dev_pcifn_id(rdev));
		goto bail;
	}
bail:
	return rc;
}

int bnxt_re_setup_dscp(struct bnxt_re_dev *rdev)
{
	int rc = 0;
	u16 portid;

	/*
	 * Target ID to be specified.
	 * 0xFFFF - if issued for the same function
	 * function_id - if issued for another function
	 */
	portid = 0xFFFF;
	rc = __bnxt_re_setup_dscp(rdev, portid);
	if (rc)
		goto bail;

	if (rdev->binfo) {
		/* Function id of the second function to be
		 * specified.
		 */
		portid = (PCI_FUNC(rdev->binfo->pdev2->devfn) + 1);
		rc = __bnxt_re_setup_dscp(rdev, portid);
		if (rc)
			goto bail;
	}
bail:
	return rc;
}

static struct bnxt_re_cc_group * __get_cc_group(struct config_item *item)
{
	struct config_group *group = container_of(item, struct config_group,
						  cg_item);
	struct bnxt_re_cc_group *ccgrp =
			container_of(group, struct bnxt_re_cc_group, group);
        return ccgrp;
}

static bool _is_cc_gen1_plus(struct bnxt_re_dev *rdev)
{
	u16 cc_gen;

	cc_gen = rdev->dev_attr->dev_cap_flags &
		 CREQ_QUERY_FUNC_RESP_SB_CC_GENERATION_MASK;
	return cc_gen >= CREQ_QUERY_FUNC_RESP_SB_CC_GENERATION_CC_GEN1;
}

static int print_cc_gen1_adv(struct bnxt_qplib_cc_param_ext *cc_ext, char *buf)
{
	int bytes = 0;

	bytes += sprintf(buf+bytes,"extended inactivity threshold\t\t: %#x\n",
			 cc_ext->inact_th_hi);
	bytes += sprintf(buf+bytes,"minimum time between cnps\t\t: %#x usec\n",
			 cc_ext->min_delta_cnp);
	bytes += sprintf(buf+bytes,"initial congestion probability\t\t: %#x\n",
			 cc_ext->init_cp);
	bytes += sprintf(buf + bytes, "target rate update mode\t\t\t: %d\n",
			 cc_ext->tr_update_mode);
	bytes += sprintf(buf+bytes,"target rate update cycle\t\t: %#x\n",
			 cc_ext->tr_update_cyls);
	bytes += sprintf(buf+bytes,"fast recovery rtt\t\t\t: %#x rtts\n",
			 cc_ext->fr_rtt);
	bytes += sprintf(buf+bytes,"active increase time quanta\t\t: %#x\n",
			 cc_ext->ai_rate_incr);
	bytes += sprintf(buf+bytes,"reduc. relax rtt threshold\t\t: %#x rtts\n",
			 cc_ext->rr_rtt_th);
	bytes += sprintf(buf+bytes,"additional relax cr rtt \t\t: %#x rtts\n",
			 cc_ext->ar_cr_th);
	bytes += sprintf(buf+bytes,"minimum current rate threshold\t\t: %#x\n",
			 cc_ext->cr_min_th);
	bytes += sprintf(buf+bytes,"bandwidth weight\t\t\t: %#x\n",
			 cc_ext->bw_avg_weight);
	bytes += sprintf(buf+bytes,"actual current rate factor\t\t: %#x\n",
			 cc_ext->cr_factor);
	bytes += sprintf(buf+bytes,"current rate level to max cp\t\t: %#x\n",
			 cc_ext->cr_th_max_cp);
	bytes += sprintf(buf+bytes,"cp bias state\t\t\t\t: %s\n",
			 cc_ext->cp_bias_en ? "Enabled" : "Disabled");
	bytes += sprintf(buf+bytes,"log of cr fraction added to cp\t\t: %#x\n",
			 cc_ext->cp_bias);
	bytes += sprintf(buf+bytes,"cr threshold to reset cc\t\t: %#x\n",
			 cc_ext->cc_cr_reset_th);
	bytes += sprintf(buf+bytes,"target rate lower bound\t\t\t: %#x\n",
			 cc_ext->tr_lb);
	bytes += sprintf(buf+bytes,"current rate probability factor\t\t: %#x\n",
			 cc_ext->cr_prob_fac);
	bytes += sprintf(buf+bytes,"target rate probability factor\t\t: %#x\n",
			 cc_ext->tr_prob_fac);
	bytes += sprintf(buf+bytes,"current rate fairness threshold\t\t: %#x\n",
			 cc_ext->fair_cr_th);
	bytes += sprintf(buf+bytes,"reduction divider\t\t\t: %#x\n",
			 cc_ext->red_div);
	bytes += sprintf(buf+bytes,"rate reduction threshold\t\t: %#x cnps\n",
			 cc_ext->cnp_ratio_th);
	bytes += sprintf(buf+bytes,"extended no congestion rtts\t\t: %#x rtt\n",
			 cc_ext->ai_ext_rtt);
	bytes += sprintf(buf+bytes,"log of cp to cr ratio\t\t\t: %#x\n",
			 cc_ext->exp_crcp_ratio);
	bytes += sprintf(buf+bytes,"use lower rate table entries\t\t: %s\n",
			 cc_ext->low_rate_en ? "Enabled" : "Disabled");
	bytes += sprintf(buf+bytes,"rtts to start cp track cr\t\t: %#x rtt\n",
			 cc_ext->cpcr_update_th);
	bytes += sprintf(buf+bytes,"first threshold to rise ai\t\t: %#x rtt\n",
			 cc_ext->ai_rtt_th1);
	bytes += sprintf(buf+bytes,"second threshold to rise ai\t\t: %#x rtt\n",
			 cc_ext->ai_rtt_th2);
	bytes += sprintf(buf+bytes,"actual rate base reduction threshold\t: %#x rtt\n",
			 cc_ext->cf_rtt_th);
	bytes += sprintf(buf+bytes,"first severe cong. cr threshold\t\t: %#x\n",
			 cc_ext->sc_cr_th1);
	bytes += sprintf(buf+bytes,"second severe cong. cr threshold\t: %#x\n",
			 cc_ext->sc_cr_th2);
	bytes += sprintf(buf+bytes,"cc ack bytes\t\t\t\t: %#x\n",
			 cc_ext->cc_ack_bytes);
	bytes += sprintf(buf+bytes,"reduce to init rtts threshold\t\t: %#x rtt\n",
			 cc_ext->reduce_cf_rtt_th);
	return bytes;
}

static int print_cc_gen1(struct bnxt_qplib_cc_param_ext *cc_ext, char *buf,
			 u8 show_adv_cc)
{
	int bytes = 0;

	bytes += sprintf(buf+bytes,"cnp header ecn status\t\t\t: %s\n",
			 !cc_ext->cnp_ecn ? "Not-ECT" :
			 (cc_ext->cnp_ecn == 0x01) ? "ECT(1)" :
			 "ECT(0)");
	bytes += sprintf(buf+bytes,"rtt jitter\t\t\t\t: %s\n",
			 cc_ext->rtt_jitter_en ? "Enabled" : "Disabled");
	bytes += sprintf(buf+bytes,"link bytes per usec\t\t\t: %#x byte/usec\n",
			 cc_ext->bytes_per_usec);
	bytes += sprintf(buf+bytes,"current rate width\t\t\t: %#x bits\n",
			 cc_ext->cr_width);
	bytes += sprintf(buf+bytes,"minimum quota period\t\t\t: %#x\n",
			 cc_ext->min_quota);
	bytes += sprintf(buf+bytes,"maximum quota period\t\t\t: %#x\n",
			 cc_ext->max_quota);
	bytes += sprintf(buf+bytes,"absolute maximum quota period\t\t: %#x\n",
			 cc_ext->abs_max_quota);
	bytes += sprintf(buf+bytes,"64B transmitted in one rtt\t\t: %#x\n",
			 cc_ext->l64B_per_rtt);
	/* Print advanced parameters */
	if (show_adv_cc)
		bytes += print_cc_gen1_adv(cc_ext, (buf+bytes));
	return bytes;
}

static int __get_d2p_index(struct bnxt_re_dev *rdev,
			   struct bnxt_re_dscp2pri *d2p,
			   u32 num_d2ps,
			   u32 *roce_index,
			   u32 *cnp_index)
{
	int i;

	if (!num_d2ps || !d2p || !roce_index || !cnp_index)
		return -EINVAL;

	for (i=0; i < num_d2ps; i++) {
		if (rdev->cc_param.roce_pri == d2p[i].pri)
			*roce_index = i;
		if (rdev->cc_param.alt_vlan_pcp == d2p[i].pri)
			*cnp_index = i;
	}

	return 0;
}

#define SLAVE_STR(slave) (slave ? "slave " : "")
static int  __print_pri_dscp_values_from_query
				(struct bnxt_re_dev *rdev, char *buf,
				 int bytes, struct bnxt_re_dscp2pri *d2p,
				 u32 roce_index, u32 cnp_index, bool slave,
				 struct bnxt_qplib_cc_param *cc_param)
{
	bytes += sprintf(buf + bytes, "%sroce prio\t\t\t\t: %d\n",
			 SLAVE_STR(slave), d2p[roce_index].pri);
	/* If CC is disabled, skip displaying the following values */
	if (!bnxt_re_is_dscp_mapping_set(rdev->cc_param.cur_mask))
		return bytes;

	bytes += sprintf(buf + bytes, "%sroce dscp\t\t\t\t: %d\n",
			 SLAVE_STR(slave), d2p[roce_index].dscp);
	if (!cc_param->enable)
		return bytes;
	bytes += sprintf(buf + bytes, "%scnp prio\t\t\t\t: %d\n",
			 SLAVE_STR(slave), d2p[cnp_index].pri);
	bytes += sprintf(buf + bytes, "%scnp dscp\t\t\t\t: %d\n",
			 SLAVE_STR(slave), d2p[cnp_index].dscp);
	return bytes;
}

static int __print_pri_dscp_values_from_async_event
			(struct bnxt_re_dev *rdev,
			 bool slave, char *buf,
			 struct bnxt_re_tc_rec *tc_rec)
{
	struct bnxt_qplib_cc_param *cc_param = &rdev->cc_param;
	int bytes = 0;

	bytes += sprintf(buf + bytes, "%sroce prio\t\t\t\t: %d\n",
			 SLAVE_STR(slave), tc_rec->roce_prio);
	bytes += sprintf(buf + bytes, "%sroce dscp\t\t\t\t: %d\n",
			 SLAVE_STR(slave), tc_rec->roce_dscp);
	if (!cc_param->enable)
		return bytes;
	bytes += sprintf(buf + bytes, "%scnp prio\t\t\t\t: %d\n",
			 SLAVE_STR(slave), tc_rec->cnp_prio);
	bytes += sprintf(buf + bytes, "%scnp dscp\t\t\t\t: %d\n",
			 SLAVE_STR(slave), tc_rec->cnp_dscp);
	return bytes;
}

int bnxt_re_get_print_dscp_pri_mapping(struct bnxt_re_dev *rdev,
				       char *buf,
				       struct bnxt_qplib_cc_param *ccparam)
{
	struct bnxt_re_dscp2pri base_d2p[8] = {};
	u32 roce_index = 0, cnp_index = 0;
	struct bnxt_re_tc_rec *tc_rec;
	u16 portid = 0xFFFF;
	u16 count = 8;
	int bytes = 0;
	u8 prio_map;
	int rc;

	if (is_qport_service_type_supported(rdev)) {
		tc_rec = &rdev->tc_rec[0];
		if (bnxt_re_get_pri_dscp_settings(rdev, -1, tc_rec))
			goto out;
		bytes += __print_pri_dscp_values_from_async_event
				(rdev, false, buf + bytes, tc_rec);
		if (rdev->binfo) {
			tc_rec = &rdev->tc_rec[1];
			if (bnxt_re_get_pri_dscp_settings(rdev, 2, tc_rec))
				goto out;
			bytes += __print_pri_dscp_values_from_async_event
					(rdev, true, buf + bytes, tc_rec);
		}
	} else {
		if (bnxt_re_is_dscp_mapping_set(rdev->cc_param.cur_mask)) {
			rc = bnxt_re_query_hwrm_dscp2pri(rdev, base_d2p,
							 &count, portid);
			if (rc) {
				dev_err(rdev_to_dev(rdev),
					"Query DSCP paras failed on fn %d\n",
					bnxt_re_dev_pcifn_id(rdev));
				bytes = rc;
				goto out;
			}
		} else {
			if (bnxt_re_is_pri_mapping_set(rdev)) {
				prio_map = bnxt_re_get_priority_mask
					(rdev, (IEEE_8021QAZ_APP_SEL_ETHERTYPE |
						IEEE_8021QAZ_APP_SEL_DGRAM));
				if (prio_map & (1 << rdev->cc_param.roce_pri))
					base_d2p[0].pri = rdev->cc_param.roce_pri;
			}
		}

		if (!__get_d2p_index(rdev, base_d2p, 2, &roce_index, &cnp_index))
			bytes = __print_pri_dscp_values_from_query
					(rdev, buf, bytes,
					 base_d2p, roce_index,
					 cnp_index, false,
					 ccparam);
		if (rdev->binfo) {
			struct bnxt_re_dscp2pri slave_d2p[8] = {};
			u16 count = 8;

			portid = (PCI_FUNC(rdev->binfo->pdev2->devfn) + 1);
			if (bnxt_re_is_dscp_mapping_set(rdev->cc_param.cur_mask)) {
				rc = bnxt_re_query_hwrm_dscp2pri
					(rdev, slave_d2p, &count, portid);
				if (rc) {
					dev_err(rdev_to_dev(rdev),
						"Query DSCP paras failed on fn %d\n",
						PCI_FUNC(rdev->binfo->pdev2->devfn));
					bytes = rc;
					goto out;
				}
			} else {
				if (bnxt_re_is_pri_mapping_set(rdev)) {
					prio_map = bnxt_re_get_priority_mask
						(rdev, (IEEE_8021QAZ_APP_SEL_ETHERTYPE |
							IEEE_8021QAZ_APP_SEL_DGRAM));
					if (prio_map & (1 << rdev->cc_param.roce_pri))
						slave_d2p[0].pri =
							rdev->cc_param.roce_pri;
				}
			}
			roce_index = 0;
			cnp_index = 0;
			if (!__get_d2p_index(rdev, slave_d2p, 2,
					     &roce_index, &cnp_index))
				bytes = __print_pri_dscp_values_from_query
						(rdev, buf, bytes,
						 slave_d2p, roce_index,
						 cnp_index, true,
						 ccparam);
		}
	}
out:
	return bytes;
}

static int __print_pri_dscp_values(struct bnxt_re_dev *rdev,
				   bool slave, char *buf,
				   struct bnxt_re_tc_rec *tc_rec)
{
	int bytes = 0;

	if (tc_rec->prio_valid & 1 << ROCE_PRIO_VALID) {
		bytes += sprintf(buf + bytes, "%sroce prio\t\t\t\t: %d\n",
				SLAVE_STR(slave), tc_rec->roce_prio);
		bytes += sprintf(buf + bytes, "%sroce dscp\t\t\t\t: %d\n",
				SLAVE_STR(slave), tc_rec->roce_dscp);
	}
	if (!tc_rec->ecn_enabled)
		return bytes;

	if (!rdev->is_virtfn && (tc_rec->prio_valid & 1 << CNP_PRIO_VALID)) {
		bytes += sprintf(buf + bytes, "%scnp prio\t\t\t\t: %d\n",
				SLAVE_STR(slave), tc_rec->cnp_prio);
		bytes += sprintf(buf + bytes, "%scnp dscp\t\t\t\t: %d\n",
				SLAVE_STR(slave), tc_rec->cnp_dscp);
	}
	return bytes;
}

int bnxt_re_get_print_dscp_pri(struct bnxt_re_dev *rdev, char *buf,
			       struct bnxt_qplib_cc_param *ccparam,
			       bool slave)
{
	struct bnxt_re_tc_rec *tc_rec;
	int rc = 0, bytes = 0;

	if (slave)
		tc_rec = &rdev->tc_rec[1];
	else
		tc_rec = &rdev->tc_rec[0];

	tc_rec->roce_dscp = ccparam->tos_dscp;
	tc_rec->cnp_dscp = ccparam->alt_tos_dscp;
	tc_rec->ecn_enabled = ccparam->enable;

	rc = bnxt_re_hwrm_pri2cos_qcfg(rdev, tc_rec, (slave ? 2 : -1));
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to query pri2cos settings on pci function %d\n",
			bnxt_re_dev_pcifn_id(rdev));
		goto end;
	}

	bytes += __print_pri_dscp_values(rdev, slave, buf + bytes, tc_rec);
end:
	return bytes;
}

static ssize_t apply_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_cc_param ccparam = {0};
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;
	int rc = 0, bytes = 0;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	drv_mode = &rdev->chip_ctx->modes;
	rc = bnxt_qplib_query_cc_param(&rdev->qplib_res, &ccparam);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to query CC parameters\n");
		bytes = rc;
		goto out;
	}


	bytes += sprintf(buf+bytes, "ecn status\t\t\t\t: %s\n",
			 ccparam.enable ? "Enabled" : "Disabled");
	bytes += sprintf(buf+bytes, "ecn marking\t\t\t\t: %s\n",
			 !ccparam.tos_ecn ? "Not-ECT" :
			 (ccparam.tos_ecn == 0x01) ? "ECT(1)" :
			  "ECT(0)");
	bytes += sprintf(buf+bytes,"congestion control mode\t\t\t: %s\n",
			 _get_mode_str(ccparam.cc_mode, _is_cc_gen1_plus(rdev)));
	bytes += sprintf(buf+bytes, "send priority vlan (VLAN 0)\t\t: %s\n",
					 rdev->qplib_res.prio ? "Enabled" : "Disabled");

	bytes += sprintf(buf+bytes, "running avg. weight(g)\t\t\t: %u\n",
			 ccparam.g);
	bytes += sprintf(buf+bytes,"inactivity threshold\t\t\t: %u usec\n",
			 ccparam.inact_th);
	bytes += sprintf(buf+bytes,"initial current rate\t\t\t: %#x\n",
			 ccparam.init_cr);
	bytes += sprintf(buf+bytes, "initial target rate\t\t\t: %#x\n",
			 ccparam.init_tr);

	if (drv_mode->cc_pr_mode) {
		bytes += sprintf(buf+bytes, "round trip time\t\t\t\t: %u usec\n",
				ccparam.rtt);
		if (!_is_chip_gen_p5_p7(rdev->chip_ctx)) {
			bytes += sprintf(buf+bytes,
					 "phases in fast recovery\t\t\t: %u\n",
					 ccparam.nph_per_state);
			bytes += sprintf(buf+bytes,
					 "quanta in recovery phase\t\t: %u\n",
					 ccparam.time_pph);
			bytes += sprintf(buf+bytes,
					 "packets in recovery phase\t\t: %u\n",
					 ccparam.pkts_pph);
		}

		if (ccparam.cc_mode == 1 && !_is_cc_gen1_plus(rdev)) {
			bytes += sprintf(buf+bytes,
					 "tcp congestion probability\t\t: %#x\n",
					 ccparam.tcp_cp);
		}
	}

	if (_is_chip_gen_p5_p7(rdev->chip_ctx))
		bytes += print_cc_gen1(&ccparam.cc_ext, (buf + bytes),
				       drv_mode->cc_pr_mode);

	bytes += bnxt_re_get_print_dscp_pri(rdev, buf + bytes, &ccparam, false);

	if (rdev->binfo)
		bytes += bnxt_re_get_print_dscp_pri(rdev, buf + bytes, &ccparam, true);

out:
	return bytes;
}

static int bnxt_re_program_cnp_dcb_values(struct bnxt_re_dev *rdev)
{
	int rc;

	rc = bnxt_re_setup_cnp_cos(rdev, !is_cc_enabled(rdev));
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to setup cnp cos\n");
		goto exit;
	}

	/* Clear the previous dscp table */
	rc = bnxt_re_clear_dscp(rdev);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to clear the dscp - pri table\n");
		goto exit;
	}
	if (!is_cc_enabled(rdev)) {
		/*
		 * Reset the CNP pri and dscp masks if
		 * dscp is not programmed
		 */
		rdev->cc_param.mask &=
			(~CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_VLAN_PCP);
		rdev->cc_param.mask &=
			(~CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_TOS_DSCP);
		rdev->cc_param.alt_tos_dscp = 0;
		rdev->cc_param.alt_vlan_pcp = 0;
	}
	/* Setup cnp and roce dscp */
	rc = bnxt_re_setup_dscp(rdev);
	if (rc) {
		dev_err(rdev_to_dev(rdev),
			"Failed to setup the dscp - pri table\n");
		goto exit;
	}
	return 0;

exit:
	return rc;
}

static ssize_t apply_store(struct config_item *item, const char *buf,
			   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;
	u8 prio_map;
	int rc = 0;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);

	mutex_lock(&rdev->cc_lock);
	if (val == BNXT_RE_MODIFY_CC) {
		/* Update current priority setting */
		prio_map = bnxt_re_get_priority_mask(rdev,
				(IEEE_8021QAZ_APP_SEL_ETHERTYPE |
				 IEEE_8021QAZ_APP_SEL_DGRAM));
		if (rdev->cur_prio_map != prio_map)
			rdev->cur_prio_map = prio_map;
		/* For VLAN transmission disablement */
		if (rdev->cc_param.mask &
		    BNXT_QPLIB_CC_PARAM_MASK_VLAN_TX_DISABLE) {
			rdev->cc_param.mask &=
				~BNXT_QPLIB_CC_PARAM_MASK_VLAN_TX_DISABLE;
			rc = bnxt_re_prio_vlan_tx_update(rdev);
			if (rc)
				dev_err(rdev_to_dev(rdev),
					"Failed to disable VLAN tx\n");
		}

		if (rdev->cc_param.mask || rdev->cc_param.cc_ext.ext_mask ||
		    rdev->cc_param.cc_ext2.ext2_mask) {
			if (!is_qport_service_type_supported(rdev)) {
				rc = bnxt_re_program_cnp_dcb_values(rdev);
				if (rc) {
					dev_err(rdev_to_dev(rdev),
						"Failed to set cnp values\n");
					goto exit;
				}
			}
			rc = bnxt_qplib_modify_cc(&rdev->qplib_res,
					&rdev->cc_param);
			if (rc)
				dev_err(rdev_to_dev(rdev),
					"Failed to apply cc settings\n");
		}
	}
exit:
	/* Reset the cc param */
	rdev->cc_param.cur_mask = rdev->cc_param.mask;
	rdev->cc_param.mask = 0;
	mutex_unlock(&rdev->cc_lock);
	return rc ? -EINVAL : strnlen(buf, count);
}
CONFIGFS_ATTR(, apply);

static ssize_t advanced_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	drv_mode = &rdev->chip_ctx->modes;
	return sprintf(buf, "%#x\n", drv_mode->cc_pr_mode);
}

#define BNXT_RE_CONFIGFS_HIDE_ADV_CC_PARAMS 0x0
#define BNXT_RE_CONFIGFS_SHOW_ADV_CC_PARAMS 0x1
static ssize_t advanced_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;
	int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	drv_mode = &rdev->chip_ctx->modes;
	sscanf(buf, "%x\n", &val);
	if (val > 0)
		drv_mode->cc_pr_mode = BNXT_RE_CONFIGFS_SHOW_ADV_CC_PARAMS;
	else
		drv_mode->cc_pr_mode = BNXT_RE_CONFIGFS_HIDE_ADV_CC_PARAMS;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, advanced);

static ssize_t cnp_dscp_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.alt_tos_dscp);
}

static ssize_t cnp_dscp_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_tc_rec *tc_rec;
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	if (val > 0x3F)
		return -EINVAL;

	mutex_lock(&rdev->cc_lock);

	tc_rec = &rdev->tc_rec[0];
	if (bnxt_re_get_pri_dscp_settings(rdev, -1, tc_rec))
		goto fail;

	/*
	 * When use_profile_type on qportcfg_output is set (indicates
	 * service_profile will carry either lossy/lossless type),
	 * Validate the DSCP and reject if it is not configured
	 * for CNP Traffic
	 */
	if (is_qport_service_type_supported(rdev) &&
	    (!(tc_rec->cnp_dscp_bv & (1ul << val))))
		goto fail;

	rdev->cc_param.prev_alt_tos_dscp = rdev->cc_param.alt_tos_dscp;
	rdev->cc_param.alt_tos_dscp = val;
	rdev->cc_param.cnp_dscp_user = val;
	rdev->cc_param.cnp_dscp_user |= BNXT_QPLIB_USER_DSCP_VALID;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_TOS_DSCP;
	mutex_unlock(&rdev->cc_lock);

	return strnlen(buf, count);
fail:
	mutex_unlock(&rdev->cc_lock);
	return -EINVAL;
}

CONFIGFS_ATTR(, cnp_dscp);

static ssize_t cnp_prio_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.alt_vlan_pcp);
}

static ssize_t cnp_prio_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	if (is_qport_service_type_supported(rdev))
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (rdev->cc_param.alt_vlan_pcp > 7)
		return -EINVAL;
	rdev->cc_param.prev_alt_vlan_pcp = rdev->cc_param.alt_vlan_pcp;
	rdev->cc_param.alt_vlan_pcp = val & 0x07;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ALT_VLAN_PCP;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cnp_prio);

static ssize_t cc_mode_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.cc_mode);
}

static ssize_t cc_mode_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (val > 1)
		return -EINVAL;
	rdev->cc_param.cc_mode = val;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_CC_MODE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cc_mode);

static ssize_t dcn_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_cc_param_ext2 *cc_ext2;
	struct bnxt_re_dev *rdev;
	ssize_t cnt = 0;
	int i;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	cc_ext2 = &rdev->cc_param.cc_ext2;
	cnt += sprintf(buf + cnt, "index   ql_thr(kB)   cr     tr cnp_inc upd_imm\n");
	for (i = 7; i >= 0; i--)
		cnt += sprintf(buf + cnt, "%5d %8u   %6lu %6lu %7d %7d\n",
			       i, cc_ext2->dcn_qlevel_tbl_thr[i],
			       DCN_GET_CR(cc_ext2->dcn_qlevel_tbl_act[i]),
			       DCN_GET_TR(cc_ext2->dcn_qlevel_tbl_act[i]),
			       DCN_GET_INC_CNP(cc_ext2->dcn_qlevel_tbl_act[i]),
			       DCN_GET_UPD_IMM(cc_ext2->dcn_qlevel_tbl_act[i]));
	cnt += sprintf(buf + cnt, "Change pending apply:\n");
	if (cc_ext2->ext2_mask)
		cnt += sprintf(buf + cnt, "%5d %8u   %6u %6u %7d %7d\n",
			       cc_ext2->idx, cc_ext2->thr, cc_ext2->cr,
			       cc_ext2->tr, cc_ext2->cnp_inc, cc_ext2->upd_imm);
	else
		cnt += sprintf(buf + cnt, "    None\n");

	return cnt;
}

static ssize_t dcn_store(struct config_item *item, const char *buf,
			 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	int idx, ql_thr, cr, tr, cnp_inc, upd_imm;
	struct bnxt_qplib_cc_param_ext2 *cc_ext2;
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	if (sscanf(buf, "%d %d %d %d %d %d\n",
		   &idx, &ql_thr, &cr, &tr, &cnp_inc, &upd_imm) != 6)
		return -EINVAL;

	/* Input values range check */
	if (idx < 0 || idx > 7)
		return -EINVAL;
	if (ql_thr < 0 || ql_thr > 0xFFFF)
		return -EINVAL;
	if (cr < 0 || cr > (MODIFY_DCN_QT_ACT_CR_MASK >> MODIFY_DCN_QT_ACT_CR_SFT))
		return -EINVAL;
	if (tr < 0 || tr > (MODIFY_DCN_QT_ACT_TR_MASK >> MODIFY_DCN_QT_ACT_TR_SFT))
		return -EINVAL;
	if (cnp_inc < 0 || cnp_inc > 1)
		return -EINVAL;
	if (upd_imm < 0 || upd_imm > 1)
		return -EINVAL;

	/* Store the values, pending to apply */
	cc_ext2 = &rdev->cc_param.cc_ext2;
	cc_ext2->idx = idx;
	cc_ext2->ext2_mask = 0;
	cc_ext2->thr = ql_thr;
	if (cc_ext2->thr != cc_ext2->dcn_qlevel_tbl_thr[idx])
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_THR;
	cc_ext2->cr = cr;
	if (cc_ext2->cr != DCN_GET_CR(cc_ext2->dcn_qlevel_tbl_act[idx]))
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_CR;
	cc_ext2->tr = tr;
	if (cc_ext2->tr != DCN_GET_TR(cc_ext2->dcn_qlevel_tbl_act[idx]))
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_TR;
	cc_ext2->cnp_inc = cnp_inc;
	if (cc_ext2->cnp_inc != DCN_GET_INC_CNP(cc_ext2->dcn_qlevel_tbl_act[idx]))
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_INC_CNP;
	cc_ext2->upd_imm = upd_imm;
	if (cc_ext2->upd_imm != DCN_GET_UPD_IMM(cc_ext2->dcn_qlevel_tbl_act[idx]))
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_UPD_IMM;
	if (cc_ext2->ext2_mask)
		cc_ext2->ext2_mask |= MODIFY_MASK_DCN_QLEVEL_TBL_IDX;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, dcn);

static ssize_t ecn_enable_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.enable);
}

static ssize_t ecn_enable_store(struct config_item *item, const char *buf,
			    size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.enable = val & 0xFF;
	rdev->cc_param.admin_enable = rdev->cc_param.enable;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_ENABLE_CC;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, ecn_enable);

static ssize_t g_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.g);
}

static ssize_t g_store(struct config_item *item, const char *buf,
		       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.g = val & 0xFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_G;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, g);

static ssize_t init_cr_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.init_cr);
}

static ssize_t init_cr_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.init_cr = val & 0xFFFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_INIT_CR;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, init_cr);

static ssize_t inact_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.inact_th);
}

static ssize_t inact_th_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.inact_th = val & 0xFFFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_INACTIVITY_CP;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, inact_th);

static ssize_t init_tr_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.init_tr);
}

static ssize_t init_tr_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.init_tr = val & 0xFFFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_INIT_TR;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, init_tr);

static ssize_t nph_per_state_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.nph_per_state);
}

static ssize_t nph_per_state_store(struct config_item *item, const char *buf,
			   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.nph_per_state = val & 0xFF;
	rdev->cc_param.mask |=
		CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_NUMPHASEPERSTATE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, nph_per_state);

static ssize_t time_pph_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.time_pph);
}

static ssize_t time_pph_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (val < 1 || val > 0xF)
		return -EINVAL;
	rdev->cc_param.time_pph = val & 0xFF;
	rdev->cc_param.mask |=
		CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TIME_PER_PHASE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, time_pph);

static ssize_t pkts_pph_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.pkts_pph);
}

static ssize_t pkts_pph_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (val < 1 || val > 0xFF)
		return -EINVAL;
	rdev->cc_param.pkts_pph = val & 0xFF;
	rdev->cc_param.mask |=
		CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_PKTS_PER_PHASE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, pkts_pph);

static ssize_t rtt_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.rtt);
}

static ssize_t rtt_store(struct config_item *item, const char *buf,
			 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.rtt = val & 0xFFFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_RTT;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, rtt);

static ssize_t tcp_cp_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.tcp_cp);
}

static ssize_t tcp_cp_store(struct config_item *item, const char *buf,
			    size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	rdev->cc_param.tcp_cp = val & 0xFFFF;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TCP_CP;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, tcp_cp);

static ssize_t roce_dscp_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.tos_dscp);
}

static ssize_t roce_dscp_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_tc_rec *tc_rec;
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (val > 0x3F)
		return -EINVAL;

	mutex_lock(&rdev->cc_lock);

	tc_rec = &rdev->tc_rec[0];
	if (bnxt_re_get_pri_dscp_settings(rdev, -1, tc_rec))
		goto fail;

	/*
	 * When use_profile_type on qportcfg_output is set (indicates
	 * service_profile will carry either lossy/lossless type),
	 * Validate the DSCP and reject if it is not configured
	 * for RoCE Traffic
	 */
	if (is_qport_service_type_supported(rdev) &&
	    (!(tc_rec->roce_dscp_bv & (1ul << val))))
		goto fail;

	rdev->cc_param.prev_tos_dscp = rdev->cc_param.tos_dscp;
	rdev->cc_param.tos_dscp = val;
	rdev->cc_param.roce_dscp_user = val;
	rdev->cc_param.roce_dscp_user |= BNXT_QPLIB_USER_DSCP_VALID;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TOS_DSCP;
	mutex_unlock(&rdev->cc_lock);

	return strnlen(buf, count);
fail:
	mutex_unlock(&rdev->cc_lock);
	return -EINVAL;
}
CONFIGFS_ATTR(, roce_dscp);

static ssize_t roce_prio_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.roce_pri);
}

static ssize_t roce_prio_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	if (is_qport_service_type_supported(rdev))
		return -EINVAL;

	sscanf(buf, "%x\n", &val);
	if (rdev->cc_param.roce_pri > 7)
		return -EINVAL;
	rdev->cc_param.prev_roce_pri = rdev->cc_param.roce_pri;
	rdev->cc_param.roce_pri = val & 0x07;
	rdev->cc_param.mask |= BNXT_QPLIB_CC_PARAM_MASK_ROCE_PRI;
        return strnlen(buf, count);
}
CONFIGFS_ATTR(, roce_prio);

static ssize_t ecn_marking_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	return sprintf(buf,"%#x\n", rdev->cc_param.tos_ecn);
}

static ssize_t ecn_marking_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	sscanf(buf, "%x", &val);
	if (val >= 0x03)
		return -EINVAL;
	rdev->cc_param.tos_ecn = val & 0x3;
	rdev->cc_param.mask |= CMDQ_MODIFY_ROCE_CC_MODIFY_MASK_TOS_ECN;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, ecn_marking);

static ssize_t disable_prio_vlan_tx_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.disable_prio_vlan_tx);
}

static ssize_t disable_prio_vlan_tx_store(struct config_item *item, const char *buf,
				     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.disable_prio_vlan_tx = val & 0x1;
	rdev->cc_param.mask |= BNXT_QPLIB_CC_PARAM_MASK_VLAN_TX_DISABLE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, disable_prio_vlan_tx);

static ssize_t inact_th_hi_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.inact_th_hi);
}

static ssize_t inact_th_hi_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.inact_th_hi = val & 0xFFFF;
	/* rdev->cc_param.cc_ext.ext_mask |= ;*/

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, inact_th_hi);

static ssize_t min_time_bet_cnp_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.min_delta_cnp);
}

static ssize_t min_time_bet_cnp_store(struct config_item *item,
				      const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.min_delta_cnp = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_MIN_TIME_BETWEEN_CNPS;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, min_time_bet_cnp);

static ssize_t init_cp_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.init_cp);
}

static ssize_t init_cp_store(struct config_item *item,
			     const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.init_cp = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_INIT_CP;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, init_cp);

static ssize_t tr_update_mode_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.tr_update_mode);
}

static ssize_t tr_update_mode_store(struct config_item *item,
				    const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.tr_update_mode = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_TR_UPDATE_MODE;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, tr_update_mode);

static ssize_t tr_update_cyls_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.tr_update_cyls);
}

static ssize_t tr_update_cyls_store(struct config_item *item,
				    const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.tr_update_cyls = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_TR_UPDATE_CYCLES;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, tr_update_cyls);

static ssize_t fr_num_rtts_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.fr_rtt);
}

static ssize_t fr_num_rtts_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.fr_rtt = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_FR_NUM_RTTS;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, fr_num_rtts);

static ssize_t ai_rate_incr_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.ai_rate_incr);
}

static ssize_t ai_rate_incr_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.ai_rate_incr = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_AI_RATE_INCREASE;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, ai_rate_incr);

static ssize_t red_rel_rtts_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.rr_rtt_th);
}

static ssize_t red_rel_rtts_th_store(struct config_item *item,
				     const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.rr_rtt_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
	CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_REDUCTION_RELAX_RTTS_TH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, red_rel_rtts_th);

static ssize_t act_rel_cr_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.ar_cr_th);
}

static ssize_t act_rel_cr_th_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.ar_cr_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
	CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_ADDITIONAL_RELAX_CR_TH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, act_rel_cr_th);

static ssize_t cr_min_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cr_min_th);
}

static ssize_t cr_min_th_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cr_min_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CR_MIN_TH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cr_min_th);

static ssize_t bw_avg_weight_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.bw_avg_weight);
}

static ssize_t bw_avg_weight_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.bw_avg_weight = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_BW_AVG_WEIGHT;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, bw_avg_weight);

static ssize_t act_cr_factor_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cr_factor);
}

static ssize_t act_cr_factor_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cr_factor = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_ACTUAL_CR_FACTOR;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, act_cr_factor);

static ssize_t max_cp_cr_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cr_th_max_cp);
}

static ssize_t max_cp_cr_th_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cr_th_max_cp = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_MAX_CP_CR_TH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, max_cp_cr_th);

static ssize_t cp_bias_en_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cp_bias_en);
}

static ssize_t cp_bias_en_store(struct config_item *item, const char *buf,
				size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cp_bias_en = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CP_BIAS_EN;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cp_bias_en);

static ssize_t cp_bias_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cp_bias);
}

static ssize_t cp_bias_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cp_bias = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CP_BIAS;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cp_bias);

static ssize_t cnp_ecn_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cnp_ecn);
}

static ssize_t cnp_ecn_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cnp_ecn = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CNP_ECN;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cnp_ecn);

static ssize_t rtt_jitter_en_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.rtt_jitter_en);
}

static ssize_t rtt_jitter_en_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.rtt_jitter_en = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_RTT_JITTER_EN;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, rtt_jitter_en);

static ssize_t lbytes_per_usec_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.bytes_per_usec);
}

static ssize_t lbytes_per_usec_store(struct config_item *item, const char *buf,
				     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.bytes_per_usec = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_LINK_BYTES_PER_USEC;
	return strnlen(buf, count);
}

CONFIGFS_ATTR(, lbytes_per_usec);

static ssize_t reset_cc_cr_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cc_cr_reset_th);
}

static ssize_t reset_cc_cr_th_store(struct config_item *item, const char *buf,
				    size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cc_cr_reset_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_RESET_CC_CR_TH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, reset_cc_cr_th);

static ssize_t cr_width_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cr_width);
}

static ssize_t cr_width_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cr_width = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CR_WIDTH;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cr_width);

static ssize_t min_quota_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.min_quota);
}

static ssize_t min_quota_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.min_quota = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_QUOTA_PERIOD_MIN;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, min_quota);

static ssize_t max_quota_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.max_quota);
}

static ssize_t max_quota_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.max_quota = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_QUOTA_PERIOD_MAX;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, max_quota);

static ssize_t abs_max_quota_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.abs_max_quota);
}

static ssize_t abs_max_quota_store(struct config_item *item, const char *buf,
				   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.abs_max_quota = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_QUOTA_PERIOD_ABS_MAX;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, abs_max_quota);

static ssize_t tr_lb_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.tr_lb);
}

static ssize_t tr_lb_store(struct config_item *item, const char *buf,
			   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.tr_lb = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_TR_LOWER_BOUND;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, tr_lb);

static ssize_t cr_prob_fac_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cr_prob_fac);
}

static ssize_t cr_prob_fac_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cr_prob_fac = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CR_PROB_FACTOR;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cr_prob_fac);

static ssize_t tr_prob_fac_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.tr_prob_fac);
}

static ssize_t tr_prob_fac_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.tr_prob_fac = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_TR_PROB_FACTOR;
	return strnlen(buf, count);
}
CONFIGFS_ATTR(, tr_prob_fac);

static ssize_t fair_cr_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.fair_cr_th);
}

static ssize_t fair_cr_th_store(struct config_item *item, const char *buf,
				size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.fair_cr_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_FAIRNESS_CR_TH;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, fair_cr_th);

static ssize_t red_div_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.red_div);
}

static ssize_t red_div_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.red_div = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_RED_DIV;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, red_div);

static ssize_t cnp_ratio_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cnp_ratio_th);
}

static ssize_t cnp_ratio_th_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cnp_ratio_th = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CNP_RATIO_TH;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cnp_ratio_th);

static ssize_t exp_ai_rtts_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.ai_ext_rtt);
}

static ssize_t exp_ai_rtts_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.ai_ext_rtt = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_EXP_AI_RTTS;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, exp_ai_rtts);

static ssize_t exp_crcp_ratio_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.exp_crcp_ratio);
}

static ssize_t exp_crcp_ratio_store(struct config_item *item, const char *buf,
				    size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.exp_crcp_ratio = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_EXP_AI_CR_CP_RATIO;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, exp_crcp_ratio);

static ssize_t rt_en_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.low_rate_en);
}

static ssize_t rt_en_store(struct config_item *item, const char *buf,
			   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.low_rate_en = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_USE_RATE_TABLE;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, rt_en);

static ssize_t cp_exp_update_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cpcr_update_th);
}

static ssize_t cp_exp_update_th_store(struct config_item *item,
				      const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cpcr_update_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CP_EXP_UPDATE_TH;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cp_exp_update_th);

static ssize_t ai_rtt_th1_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.ai_rtt_th1);
}

static ssize_t ai_rtt_th1_store(struct config_item *item, const char *buf,
				size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.ai_rtt_th1 = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_HIGH_EXP_AI_RTTS_TH1;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, ai_rtt_th1);

static ssize_t ai_rtt_th2_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.ai_rtt_th2);
}

static ssize_t ai_rtt_th2_store(struct config_item *item, const char *buf,
				size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.ai_rtt_th2 = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_HIGH_EXP_AI_RTTS_TH2;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, ai_rtt_th2);

static ssize_t cf_rtt_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cf_rtt_th);
}

static ssize_t cf_rtt_th_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cf_rtt_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
	CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_ACTUAL_CR_CONG_FREE_RTTS_TH;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cf_rtt_th);

static ssize_t reduce_cf_rtt_th_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.reduce_cf_rtt_th);
}

static ssize_t reduce_cf_rtt_th_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.reduce_cf_rtt_th = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_REDUCE_INIT_CONG_FREE_RTTS_TH;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, reduce_cf_rtt_th);

static ssize_t sc_cr_th1_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.sc_cr_th1);
}

static ssize_t sc_cr_th1_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.sc_cr_th1 = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_SEVERE_CONG_CR_TH1;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, sc_cr_th1);

static ssize_t sc_cr_th2_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.sc_cr_th2);
}

static ssize_t sc_cr_th2_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.sc_cr_th2 = val & 0xFFFF;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_SEVERE_CONG_CR_TH2;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, sc_cr_th2);

static ssize_t l64B_per_rtt_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.l64B_per_rtt);
}

static ssize_t l64B_per_rtt_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.l64B_per_rtt = val;
	rdev->cc_param.cc_ext.ext_mask |=
		CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_LINK64B_PER_RTT;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, l64B_per_rtt);

static ssize_t cc_ack_bytes_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf,"%#x\n", rdev->cc_param.cc_ext.cc_ack_bytes);
}

static ssize_t cc_ack_bytes_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &val);
	rdev->cc_param.cc_ext.cc_ack_bytes = val & 0xFF;
	rdev->cc_param.cc_ext.ext_mask |=
	CMDQ_MODIFY_ROCE_CC_GEN1_TLV_MODIFY_MASK_CC_ACK_BYTES;

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, cc_ack_bytes);

static struct configfs_attribute *bnxt_re_cc_attrs[] = {
	CONFIGFS_ATTR_ADD(attr_advanced),
	CONFIGFS_ATTR_ADD(attr_apply),
	CONFIGFS_ATTR_ADD(attr_cnp_dscp),
	CONFIGFS_ATTR_ADD(attr_cnp_prio),
	CONFIGFS_ATTR_ADD(attr_cc_mode),
	CONFIGFS_ATTR_ADD(attr_ecn_enable),
	CONFIGFS_ATTR_ADD(attr_g),
	CONFIGFS_ATTR_ADD(attr_init_cr),
	CONFIGFS_ATTR_ADD(attr_inact_th),
	CONFIGFS_ATTR_ADD(attr_init_tr),
	CONFIGFS_ATTR_ADD(attr_nph_per_state),
	CONFIGFS_ATTR_ADD(attr_time_pph),
	CONFIGFS_ATTR_ADD(attr_pkts_pph),
	CONFIGFS_ATTR_ADD(attr_rtt),
	CONFIGFS_ATTR_ADD(attr_tcp_cp),
	CONFIGFS_ATTR_ADD(attr_roce_dscp),
	CONFIGFS_ATTR_ADD(attr_roce_prio),
	CONFIGFS_ATTR_ADD(attr_ecn_marking),
	CONFIGFS_ATTR_ADD(attr_disable_prio_vlan_tx),
	NULL,
};

static struct configfs_attribute *bnxt_re_cc_attrs_ext[] = {
	CONFIGFS_ATTR_ADD(attr_advanced),
	CONFIGFS_ATTR_ADD(attr_apply),
	CONFIGFS_ATTR_ADD(attr_cnp_dscp),
	CONFIGFS_ATTR_ADD(attr_cnp_prio),
	CONFIGFS_ATTR_ADD(attr_cc_mode),
	CONFIGFS_ATTR_ADD(attr_ecn_enable),
	CONFIGFS_ATTR_ADD(attr_g),
	CONFIGFS_ATTR_ADD(attr_init_cr),
	CONFIGFS_ATTR_ADD(attr_inact_th),
	CONFIGFS_ATTR_ADD(attr_init_tr),
	CONFIGFS_ATTR_ADD(attr_rtt),
	CONFIGFS_ATTR_ADD(attr_roce_dscp),
	CONFIGFS_ATTR_ADD(attr_roce_prio),
	CONFIGFS_ATTR_ADD(attr_ecn_marking),
	CONFIGFS_ATTR_ADD(attr_disable_prio_vlan_tx),
	CONFIGFS_ATTR_ADD(attr_inact_th_hi),
	CONFIGFS_ATTR_ADD(attr_min_time_bet_cnp),
	CONFIGFS_ATTR_ADD(attr_init_cp),
	CONFIGFS_ATTR_ADD(attr_tr_update_mode),
	CONFIGFS_ATTR_ADD(attr_tr_update_cyls),
	CONFIGFS_ATTR_ADD(attr_fr_num_rtts),
	CONFIGFS_ATTR_ADD(attr_ai_rate_incr),
	CONFIGFS_ATTR_ADD(attr_red_rel_rtts_th),
	CONFIGFS_ATTR_ADD(attr_act_rel_cr_th),
	CONFIGFS_ATTR_ADD(attr_cr_min_th),
	CONFIGFS_ATTR_ADD(attr_bw_avg_weight),
	CONFIGFS_ATTR_ADD(attr_act_cr_factor),
	CONFIGFS_ATTR_ADD(attr_max_cp_cr_th),
	CONFIGFS_ATTR_ADD(attr_cp_bias_en),
	CONFIGFS_ATTR_ADD(attr_cp_bias),
	CONFIGFS_ATTR_ADD(attr_cnp_ecn),
	CONFIGFS_ATTR_ADD(attr_rtt_jitter_en),
	CONFIGFS_ATTR_ADD(attr_lbytes_per_usec),
	CONFIGFS_ATTR_ADD(attr_reset_cc_cr_th),
	CONFIGFS_ATTR_ADD(attr_cr_width),
	CONFIGFS_ATTR_ADD(attr_min_quota),
	CONFIGFS_ATTR_ADD(attr_max_quota),
	CONFIGFS_ATTR_ADD(attr_abs_max_quota),
	CONFIGFS_ATTR_ADD(attr_tr_lb),
	CONFIGFS_ATTR_ADD(attr_cr_prob_fac),
	CONFIGFS_ATTR_ADD(attr_tr_prob_fac),
	CONFIGFS_ATTR_ADD(attr_fair_cr_th),
	CONFIGFS_ATTR_ADD(attr_red_div),
	CONFIGFS_ATTR_ADD(attr_cnp_ratio_th),
	CONFIGFS_ATTR_ADD(attr_exp_ai_rtts),
	CONFIGFS_ATTR_ADD(attr_exp_crcp_ratio),
	CONFIGFS_ATTR_ADD(attr_rt_en),
	CONFIGFS_ATTR_ADD(attr_cp_exp_update_th),
	CONFIGFS_ATTR_ADD(attr_ai_rtt_th1),
	CONFIGFS_ATTR_ADD(attr_ai_rtt_th2),
	CONFIGFS_ATTR_ADD(attr_cf_rtt_th),
	CONFIGFS_ATTR_ADD(attr_sc_cr_th1),
	CONFIGFS_ATTR_ADD(attr_sc_cr_th2),
	CONFIGFS_ATTR_ADD(attr_l64B_per_rtt),
	CONFIGFS_ATTR_ADD(attr_cc_ack_bytes),
	CONFIGFS_ATTR_ADD(attr_reduce_cf_rtt_th),
	NULL,
};

static struct configfs_attribute *bnxt_re_cc_attrs_ext2[] = {
	CONFIGFS_ATTR_ADD(attr_advanced),
	CONFIGFS_ATTR_ADD(attr_apply),
	CONFIGFS_ATTR_ADD(attr_cnp_dscp),
	CONFIGFS_ATTR_ADD(attr_cnp_prio),
	CONFIGFS_ATTR_ADD(attr_cc_mode),
	CONFIGFS_ATTR_ADD(attr_dcn),
	CONFIGFS_ATTR_ADD(attr_ecn_enable),
	CONFIGFS_ATTR_ADD(attr_g),
	CONFIGFS_ATTR_ADD(attr_init_cr),
	CONFIGFS_ATTR_ADD(attr_inact_th),
	CONFIGFS_ATTR_ADD(attr_init_tr),
	CONFIGFS_ATTR_ADD(attr_rtt),
	CONFIGFS_ATTR_ADD(attr_roce_dscp),
	CONFIGFS_ATTR_ADD(attr_roce_prio),
	CONFIGFS_ATTR_ADD(attr_ecn_marking),
	CONFIGFS_ATTR_ADD(attr_disable_prio_vlan_tx),
	CONFIGFS_ATTR_ADD(attr_inact_th_hi),
	CONFIGFS_ATTR_ADD(attr_min_time_bet_cnp),
	CONFIGFS_ATTR_ADD(attr_init_cp),
	CONFIGFS_ATTR_ADD(attr_tr_update_mode),
	CONFIGFS_ATTR_ADD(attr_tr_update_cyls),
	CONFIGFS_ATTR_ADD(attr_fr_num_rtts),
	CONFIGFS_ATTR_ADD(attr_ai_rate_incr),
	CONFIGFS_ATTR_ADD(attr_red_rel_rtts_th),
	CONFIGFS_ATTR_ADD(attr_act_rel_cr_th),
	CONFIGFS_ATTR_ADD(attr_cr_min_th),
	CONFIGFS_ATTR_ADD(attr_bw_avg_weight),
	CONFIGFS_ATTR_ADD(attr_act_cr_factor),
	CONFIGFS_ATTR_ADD(attr_max_cp_cr_th),
	CONFIGFS_ATTR_ADD(attr_cp_bias_en),
	CONFIGFS_ATTR_ADD(attr_cp_bias),
	CONFIGFS_ATTR_ADD(attr_cnp_ecn),
	CONFIGFS_ATTR_ADD(attr_rtt_jitter_en),
	CONFIGFS_ATTR_ADD(attr_lbytes_per_usec),
	CONFIGFS_ATTR_ADD(attr_reset_cc_cr_th),
	CONFIGFS_ATTR_ADD(attr_cr_width),
	CONFIGFS_ATTR_ADD(attr_min_quota),
	CONFIGFS_ATTR_ADD(attr_max_quota),
	CONFIGFS_ATTR_ADD(attr_abs_max_quota),
	CONFIGFS_ATTR_ADD(attr_tr_lb),
	CONFIGFS_ATTR_ADD(attr_cr_prob_fac),
	CONFIGFS_ATTR_ADD(attr_tr_prob_fac),
	CONFIGFS_ATTR_ADD(attr_fair_cr_th),
	CONFIGFS_ATTR_ADD(attr_red_div),
	CONFIGFS_ATTR_ADD(attr_cnp_ratio_th),
	CONFIGFS_ATTR_ADD(attr_exp_ai_rtts),
	CONFIGFS_ATTR_ADD(attr_exp_crcp_ratio),
	CONFIGFS_ATTR_ADD(attr_rt_en),
	CONFIGFS_ATTR_ADD(attr_cp_exp_update_th),
	CONFIGFS_ATTR_ADD(attr_ai_rtt_th1),
	CONFIGFS_ATTR_ADD(attr_ai_rtt_th2),
	CONFIGFS_ATTR_ADD(attr_cf_rtt_th),
	CONFIGFS_ATTR_ADD(attr_sc_cr_th1),
	CONFIGFS_ATTR_ADD(attr_sc_cr_th2),
	CONFIGFS_ATTR_ADD(attr_l64B_per_rtt),
	CONFIGFS_ATTR_ADD(attr_cc_ack_bytes),
	CONFIGFS_ATTR_ADD(attr_reduce_cf_rtt_th),
	NULL,
};

static struct bnxt_re_dev *cfgfs_update_auxbus_re(struct bnxt_re_dev *rdev,
						  u32 gsi_mode, u8 wqe_mode)
{
	struct bnxt_re_dev *new_rdev = NULL;
	struct net_device *netdev;
	struct bnxt_en_dev *en_dev;
	struct auxiliary_device *adev;
	int rc = 0;

	/* check again if context is changed by roce driver */
	if (!rdev)
		return NULL;

	mutex_lock(&bnxt_re_mutex);
	en_dev = rdev->en_dev;
	netdev = en_dev->net;
	adev   = rdev->adev;

	/* Remove and add the device.
	 * Before removing unregister with IB.
	 */
	bnxt_re_ib_uninit(rdev);
	bnxt_re_remove_device(rdev, BNXT_RE_COMPLETE_REMOVE, adev);
	rc = bnxt_re_add_device(&new_rdev, netdev, NULL, gsi_mode,
				BNXT_RE_COMPLETE_INIT, wqe_mode,
				adev);
	if (rc)
		goto clean_dev;
	_bnxt_re_ib_init(new_rdev);
	_bnxt_re_ib_init2(new_rdev);

	/* update the auxdev container */
	rdev = new_rdev;

	/* Don't crash for usermodes.
	 * Return gracefully they will retry
	 */
	if (rtnl_trylock()) {
		bnxt_re_get_link_speed(rdev);
		rtnl_unlock();
	} else {
		pr_err("Setting link speed failed, retry config again");
		goto clean_dev;
	}
	mutex_unlock(&bnxt_re_mutex);
	return rdev;
clean_dev:
	mutex_unlock(&bnxt_re_mutex);
	if (new_rdev) {
		bnxt_re_ib_uninit(rdev);
		bnxt_re_remove_device(rdev, BNXT_RE_COMPLETE_REMOVE, adev);
	}
	return NULL;
}

#ifdef HAVE_OLD_CONFIGFS_API
static ssize_t bnxt_re_ccgrp_attr_show(struct config_item *item,
				       struct configfs_attribute *attr,
				       char *page)
{
	struct configfs_attr *ccgrp_attr =
			container_of(attr, struct configfs_attr, attr);
	ssize_t rc = -EINVAL;

	if (!ccgrp_attr)
		goto out;

	if (ccgrp_attr->show)
		rc = ccgrp_attr->show(item, page);
out:
	return rc;
}

static ssize_t bnxt_re_ccgrp_attr_store(struct config_item *item,
					struct configfs_attribute *attr,
					const char *page, size_t count)
{
	struct configfs_attr *ccgrp_attr =
			container_of(attr, struct configfs_attr, attr);
	ssize_t rc = -EINVAL;

	if (!ccgrp_attr)
		goto out;
	if (ccgrp_attr->store)
		rc = ccgrp_attr->store(item, page, count);
out:
	return rc;
}

static struct configfs_item_operations bnxt_re_ccgrp_ops = {
	.show_attribute         = bnxt_re_ccgrp_attr_show,
	.store_attribute	= bnxt_re_ccgrp_attr_store,
};

#else
static struct configfs_item_operations bnxt_re_ccgrp_ops = {
};
#endif

static struct config_item_type bnxt_re_ccgrp_type = {
	.ct_attrs = bnxt_re_cc_attrs,
	.ct_item_ops = &bnxt_re_ccgrp_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item_type bnxt_re_ccgrp_type_ext = {
	.ct_attrs = bnxt_re_cc_attrs_ext,
	.ct_item_ops = &bnxt_re_ccgrp_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item_type bnxt_re_ccgrp_type_ext2 = {
	.ct_attrs = bnxt_re_cc_attrs_ext2,
	.ct_item_ops = &bnxt_re_ccgrp_ops,
	.ct_owner = THIS_MODULE,
};

static int make_bnxt_re_cc(struct bnxt_re_port_group *portgrp,
			   struct bnxt_re_dev *rdev, u32 gidx)
{
	struct config_item_type *grp_type;
	struct bnxt_re_cc_group *ccgrp;
	int rc;

	/*
	 * TODO: If there is confirmed use case that users need to read cc
	 * params from VF instance, we would enable cc node for VF with
	 * selected params.
	 */
	if (rdev->is_virtfn)
		return 0;

	ccgrp = kzalloc(sizeof(*ccgrp), GFP_KERNEL);
	if (!ccgrp) {
		rc = -ENOMEM;
		goto out;
	}

	ccgrp->rdev = rdev;
	grp_type = &bnxt_re_ccgrp_type;
	if (_is_chip_gen_p5_p7(rdev->chip_ctx)) {
		if (BNXT_RE_DCN_ENABLED(rdev->rcfw.res))
			grp_type = &bnxt_re_ccgrp_type_ext2;
		else
			grp_type = &bnxt_re_ccgrp_type_ext;
	}

	config_group_init_type_name(&ccgrp->group, "cc", grp_type);
#ifndef HAVE_CFGFS_ADD_DEF_GRP
	portgrp->nportgrp.default_groups = portgrp->default_grp;
	portgrp->default_grp[gidx] = &ccgrp->group;
	portgrp->default_grp[gidx + 1] = NULL;
#else
	configfs_add_default_group(&ccgrp->group, &portgrp->nportgrp);
#endif
	portgrp->ccgrp = ccgrp;
	ccgrp->portgrp = portgrp;

	return 0;
out:
	kfree(ccgrp);
	return rc;
}

static ssize_t min_tx_depth_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%u\n", rdev->min_tx_depth);
}

static ssize_t min_tx_depth_store(struct config_item *item, const char *buf,
				  size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;
	int rc;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	rc = sscanf(buf, "%u\n", &val);
	if (val > rdev->dev_attr->max_qp_wqes || rc <= 0) {
		dev_err(rdev_to_dev(rdev),
			"min_tx_depth %u cannot be greater than max_qp_wqes %u",
			val, rdev->dev_attr->max_qp_wqes);
		return -EINVAL;
	}

	rdev->min_tx_depth = val;

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, min_tx_depth);

static ssize_t stats_query_sec_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->stats.stats_query_sec);
}

static ssize_t stats_query_sec_store(struct config_item *item, const char *buf,
				     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	/* Valid values are 0 - 8 now. default value is 1
	 * 0 means disable periodic query.
	 * 1 means bnxt_re_worker queries every sec, 2 - every 2 sec and so on
	 */

	if (val > 8)
		return -EINVAL;

	rdev->stats.stats_query_sec = val;

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, stats_query_sec);

static ssize_t gsi_qp_mode_store(struct config_item *item,
				 const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev, *new_rdev = NULL;
	struct mutex *mutexp; /* Aquire subsys mutex */
	u32 gsi_mode;
	u8 wqe_mode;
	int rc;

	if (!ccgrp)
		return -EINVAL;

	/* Hold the subsytem lock to serialize */
	mutexp = &item->ci_group->cg_subsys->su_mutex;
	mutex_lock(mutexp);
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		goto ret_err;
	if (_is_chip_gen_p5_p7(rdev->chip_ctx))
		goto ret_err;
	sscanf(buf, "%x\n", (unsigned int *)&gsi_mode);
	if (!gsi_mode || gsi_mode > BNXT_RE_GSI_MODE_ROCE_V2_IPV6)
		goto ret_err;
	if (gsi_mode == rdev->gsi_ctx.gsi_qp_mode)
		goto done;
	wqe_mode = rdev->chip_ctx->modes.wqe_mode;

	if (rdev->binfo) {
		struct bnxt_re_bond_info binfo;
		struct netdev_bonding_info *nbinfo;

		memcpy(&binfo, rdev->binfo, sizeof(*(rdev->binfo)));
		nbinfo = &binfo.nbinfo;
		bnxt_re_destroy_lag(&rdev);
		bnxt_re_create_base_interface(&binfo, true);
		bnxt_re_create_base_interface(&binfo, false);

		/* TODO: Wait for sched count to become 0 on both rdevs */
		msleep(10000);

		/* Recreate lag. */
		rc = bnxt_re_create_lag(&nbinfo->master, &nbinfo->slave,
					nbinfo, binfo.slave2, &new_rdev,
					gsi_mode, wqe_mode);
		if (rc)
			dev_warn(rdev_to_dev(rdev), "%s: failed to create lag %d\n",
				 __func__, rc);
	} else {
		/* driver functions takes care of locking */
		new_rdev = cfgfs_update_auxbus_re(rdev, gsi_mode, wqe_mode);
		if (!new_rdev)
			goto ret_err;
	}

	if (new_rdev)
		ccgrp->rdev = new_rdev;
done:
	mutex_unlock(mutexp);
	return strnlen(buf, count);
ret_err:
	mutex_unlock(mutexp);
	return -EINVAL;
}

static const char *bnxt_re_mode_to_str [] = {
	"GSI Mode Invalid",
	"GSI Mode All",
	"GSI Mode RoCE_v1 Only",
	"GSI Mode RoCE_v2 IPv4 Only",
	"GSI Mode RoCE_v2 IPv6 Only",
	"GSI Mode UD"
};

static inline const char * mode_to_str(u8 gsi_mode)
{
	return bnxt_re_mode_to_str[gsi_mode];
}

static ssize_t gsi_qp_mode_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	int bytes = 0;
	u8 gsi_mode;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	/* Little too much? */
	gsi_mode = rdev->gsi_ctx.gsi_qp_mode;
	bytes += sprintf(buf + bytes, "%s (%#x): %s\n",
			 mode_to_str(BNXT_RE_GSI_MODE_ALL),
			 (int)BNXT_RE_GSI_MODE_ALL,
			 gsi_mode == BNXT_RE_GSI_MODE_ALL ?
			 "Enabled" : "Disabled");
	bytes += sprintf(buf + bytes, "%s (%#x): %s\n",
			 mode_to_str(BNXT_RE_GSI_MODE_ROCE_V1),
			 (int)BNXT_RE_GSI_MODE_ROCE_V1,
			 gsi_mode == BNXT_RE_GSI_MODE_ROCE_V1 ?
			 "Enabled" : "Disabled");
	bytes += sprintf(buf + bytes, "%s (%#x): %s\n",
			 mode_to_str(BNXT_RE_GSI_MODE_ROCE_V2_IPV4),
			 (int)BNXT_RE_GSI_MODE_ROCE_V2_IPV4,
			 gsi_mode == BNXT_RE_GSI_MODE_ROCE_V2_IPV4 ?
			 "Enabled" : "Disabled");
	bytes += sprintf(buf + bytes, "%s (%#x): %s\n",
			 mode_to_str(BNXT_RE_GSI_MODE_ROCE_V2_IPV6),
			 (int)BNXT_RE_GSI_MODE_ROCE_V2_IPV6,
			 gsi_mode == BNXT_RE_GSI_MODE_ROCE_V2_IPV6 ?
			 "Enabled" : "Disabled");
	bytes += sprintf(buf + bytes, "%s (%#x): %s\n",
			 mode_to_str(BNXT_RE_GSI_MODE_UD),
			 (int)BNXT_RE_GSI_MODE_UD,
			 gsi_mode == BNXT_RE_GSI_MODE_UD ?
			 "Enabled" : "Disabled");
	return bytes;
}
CONFIGFS_ATTR(, gsi_qp_mode);

static const char *bnxt_re_wqe_mode_to_str [] = {
	"STATIC", "VARIABLE"
};

static ssize_t wqe_mode_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	drv_mode = &rdev->chip_ctx->modes;
	return sprintf(buf, "sq wqe mode: %s (%#x)\n",
		       bnxt_re_wqe_mode_to_str[drv_mode->wqe_mode],
		       drv_mode->wqe_mode);
}

static ssize_t wqe_mode_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev, *new_rdev = NULL;
	struct bnxt_qplib_drv_modes *drv_mode;
	struct mutex *mutexp; /* subsys lock */
	int mode, rc;
	u8 gsi_mode;

	if (!ccgrp)
		return -EINVAL;
	/* Hold the subsys lock */
	mutexp = &item->ci_group->cg_subsys->su_mutex;
	mutex_lock(mutexp);
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		goto ret_err;
	rc = sscanf(buf, "%d\n", &mode);
	if (mode < 0 || mode > BNXT_QPLIB_WQE_MODE_VARIABLE || rc <= 0)
		goto ret_err;
	if (mode == BNXT_QPLIB_WQE_MODE_VARIABLE &&
	    !_is_chip_gen_p5_p7(rdev->chip_ctx))
		goto ret_err;

	drv_mode = &rdev->chip_ctx->modes;
	if (drv_mode->wqe_mode == mode)
		goto done;

	gsi_mode = rdev->gsi_ctx.gsi_qp_mode;
	new_rdev = cfgfs_update_auxbus_re(rdev, gsi_mode, mode);
	if (!new_rdev)
		goto ret_err;
	ccgrp->rdev = new_rdev;
done:
	mutex_unlock(mutexp);
	return strnlen(buf, count);
ret_err:
	mutex_unlock(mutexp);
	return -EINVAL;
}
CONFIGFS_ATTR(, wqe_mode);

static ssize_t acc_tx_path_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	drv_mode = &rdev->chip_ctx->modes;
	return sprintf(buf, "Accelerated transmit path: %s\n",
		       drv_mode->te_bypass ? "Enabled" : "Disabled");
}

static ssize_t acc_tx_path_store(struct config_item *item, const char *buf,
				 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_en_dev_info *en_info;
	struct bnxt_qplib_drv_modes *drv_mode;
	struct bnxt_re_dev *rdev;
	unsigned int mode;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	sscanf(buf, "%x\n", &mode);
	if (mode >= 2)
		return -EINVAL;
	if (mode) {
		if (!_is_chip_gen_p5_p7(rdev->chip_ctx))
			return -EINVAL;

		if (_is_chip_p7(rdev->chip_ctx) && BNXT_EN_HW_LAG(rdev->en_dev))
			return -EINVAL;
	}

	drv_mode = &rdev->chip_ctx->modes;
	drv_mode->te_bypass = mode;

	/* Update the container */
	en_info = auxiliary_get_drvdata(rdev->adev);
	if (en_info)
		en_info->te_bypass = (mode == 0x1);

	return strnlen(buf, count);
}
CONFIGFS_ATTR(, acc_tx_path);

static ssize_t en_qp_dbg_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->en_qp_dbg);
}

static ssize_t en_qp_dbg_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	if (rdev->en_qp_dbg && val == 0)
		bnxt_re_rem_dbg_files(rdev);
	else if (!rdev->en_qp_dbg && val)
		bnxt_re_add_dbg_files(rdev);

	rdev->en_qp_dbg = val;

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, en_qp_dbg);

static ssize_t user_dbr_drop_recov_timeout_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%d\n", rdev->user_dbr_drop_recov_timeout);
}

static ssize_t user_dbr_drop_recov_timeout_store(struct config_item *item, const char *buf,
						 size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%d\n", &val)  != 1)
		return -EINVAL;
	if ((val < BNXT_DBR_DROP_MIN_TIMEOUT) || (val > BNXT_DBR_DROP_MAX_TIMEOUT))
		return -EINVAL;

	rdev->user_dbr_drop_recov_timeout = val;

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, user_dbr_drop_recov_timeout);

static ssize_t user_dbr_drop_recov_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->user_dbr_drop_recov);
}

static ssize_t user_dbr_drop_recov_store(struct config_item *item, const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;
	int rc = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	if (!val) {
		/* Disable DBR drop recovery */
		if (!rdev->user_dbr_drop_recov) {
			dev_info(rdev_to_dev(rdev),
				 "User DBR drop recovery already disabled. Returning\n");
			goto exit;
		}
		rdev->user_dbr_drop_recov = false;
	} else {
		if (rdev->user_dbr_drop_recov) {
			dev_info(rdev_to_dev(rdev),
				 "User DBR drop recovery already enabled. Returning\n");
			goto exit;
		}

		if (!rdev->dbr_drop_recov) {
			dev_info(rdev_to_dev(rdev),
				 "Can not enable User DBR drop recovery as FW doesn't support\n");
			rdev->user_dbr_drop_recov = false;
			rc = -EINVAL;
			goto exit;
		}

		rdev->user_dbr_drop_recov = true;
	}
exit:
	return (rc ? -EINVAL : strnlen(buf, count));
}

CONFIGFS_ATTR(, user_dbr_drop_recov);

static ssize_t dbr_pacing_enable_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;

	if (!rdev->dbr_bar_addr)
		dev_info(rdev_to_dev(rdev),
			 "DBR pacing is not supported on this device\n");
	return sprintf(buf, "%#x\n", rdev->dbr_pacing);
}

static ssize_t dbr_pacing_enable_store(struct config_item *item, const char *buf,
				       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	struct bnxt_qplib_nq *nq;
	unsigned int val = 0;
	int rc = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > 1)
		return -EINVAL;

	if (!rdev->dbr_bar_addr) {
		dev_info(rdev_to_dev(rdev),
			 "DBR pacing is not supported on this device\n");
		return -EINVAL;
	}

	nq = &rdev->nqr->nq[0];
	if (!val) {
		/* Disable DBR Pacing */
		if (!rdev->dbr_pacing) {
			dev_info(rdev_to_dev(rdev),
				 "DBR pacing already disabled. Returning\n");
			goto exit;
		}
		if (!bnxt_qplib_dbr_pacing_ext_en(rdev->chip_ctx))
			rc = bnxt_re_disable_dbr_pacing(rdev);
	} else {
		if (rdev->dbr_pacing) {
			dev_info(rdev_to_dev(rdev),
				 "DBR pacing already enabled. Returning\n");
			goto exit;
		}
		if (!bnxt_qplib_dbr_pacing_ext_en(rdev->chip_ctx))
			rc = bnxt_re_enable_dbr_pacing(rdev);
		else
			bnxt_re_set_dbq_throttling_reg(rdev, nq->ring_id,
						       rdev->dbq_watermark);
	}
	rdev->dbr_pacing = !!val;
exit:
	return (rc ? -EINVAL : strnlen(buf, count));
}

CONFIGFS_ATTR(, dbr_pacing_enable);

static ssize_t dbr_pacing_dbq_watermark_show(struct config_item *item,
					     char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->dbq_watermark);
}

static ssize_t dbr_pacing_dbq_watermark_store(struct config_item *item,
					      const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	struct bnxt_qplib_nq *nq;
	unsigned int val = 0;
	int rc = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > BNXT_RE_PACING_DBQ_HIGH_WATERMARK)
		return -EINVAL;

	rdev->dbq_watermark = val;

	if (bnxt_qplib_dbr_pacing_ext_en(rdev->chip_ctx)) {
		nq = &rdev->nqr->nq[0];
		bnxt_re_set_dbq_throttling_reg(rdev, nq->ring_id, rdev->dbq_watermark);
	} else {
		if (bnxt_re_enable_dbr_pacing(rdev)) {
			dev_err(rdev_to_dev(rdev),
				"Failed to set dbr pacing config\n");
			rc = -EIO;
		}
	}
	return rc ? rc : strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_pacing_dbq_watermark);

static ssize_t dbr_pacing_time_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->dbq_pacing_time);
}

static ssize_t dbr_pacing_time_store(struct config_item *item, const char *buf,
				     size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;

	rdev->dbq_pacing_time = val;
	return strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_pacing_time);

static ssize_t dbr_pacing_primary_fn_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n",
		      bnxt_qplib_dbr_pacing_is_primary_pf(rdev->chip_ctx));
}

static ssize_t dbr_pacing_primary_fn_store(struct config_item *item, const char *buf,
					   size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;
	int rc = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;

	if (bnxt_qplib_dbr_pacing_ext_en(rdev->chip_ctx)) {
		dev_info(rdev_to_dev(rdev),
			 "FW is responsible for picking the primary function\n");
		return -EOPNOTSUPP;
	}
	if (!val) {
		if (bnxt_qplib_dbr_pacing_is_primary_pf(rdev->chip_ctx)) {
			rc = bnxt_re_disable_dbr_pacing(rdev);
			bnxt_qplib_dbr_pacing_set_primary_pf(rdev->chip_ctx, 0);
		}
	} else {
		rc = bnxt_re_enable_dbr_pacing(rdev);
		bnxt_qplib_dbr_pacing_set_primary_pf(rdev->chip_ctx, 1);
	}
	return rc ? rc : strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_pacing_primary_fn);

static ssize_t dbr_pacing_algo_threshold_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->pacing_algo_th);
}

static ssize_t dbr_pacing_algo_threshold_store(struct config_item *item,
					       const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > rdev->qplib_res.pacing_data->fifo_max_depth)
		return -EINVAL;

	rdev->pacing_algo_th = val;
	bnxt_re_set_def_pacing_threshold(rdev);

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_pacing_algo_threshold);

static ssize_t dbr_pacing_en_int_threshold_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->pacing_en_int_th);
}

static ssize_t dbr_pacing_en_int_threshold_store(struct config_item *item,
						 const char *buf, size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val)  != 1)
		return -EINVAL;
	if (val > rdev->qplib_res.pacing_data->fifo_max_depth)
		return -EINVAL;

	rdev->pacing_en_int_th = val;

	return strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_pacing_en_int_threshold);

static ssize_t dbr_def_do_pacing_show(struct config_item *item, char *buf)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;

	if (!ccgrp)
		return -EINVAL;

	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	return sprintf(buf, "%#x\n", rdev->dbr_def_do_pacing);
}

static ssize_t dbr_def_do_pacing_store(struct config_item *item, const char *buf,
				       size_t count)
{
	struct bnxt_re_cc_group *ccgrp = __get_cc_group(item);
	struct bnxt_re_dev *rdev;
	unsigned int val = 0;

	if (!ccgrp)
		return -EINVAL;
	rdev = bnxt_re_get_valid_rdev(ccgrp);
	if (!rdev)
		return -EINVAL;
	if (sscanf(buf, "%x\n", &val) != 1)
		return -EINVAL;
	if (val > BNXT_RE_MAX_DBR_DO_PACING)
		return -EINVAL;
	rdev->dbr_def_do_pacing = val;
	bnxt_re_set_def_do_pacing(rdev);
	return strnlen(buf, count);
}

CONFIGFS_ATTR(, dbr_def_do_pacing);

static struct configfs_attribute *bnxt_re_tun_attrs[] = {
	CONFIGFS_ATTR_ADD(attr_min_tx_depth),
	CONFIGFS_ATTR_ADD(attr_stats_query_sec),
	CONFIGFS_ATTR_ADD(attr_gsi_qp_mode),
	CONFIGFS_ATTR_ADD(attr_wqe_mode),
	CONFIGFS_ATTR_ADD(attr_acc_tx_path),
	CONFIGFS_ATTR_ADD(attr_en_qp_dbg),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_enable),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_dbq_watermark),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_time),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_primary_fn),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_algo_threshold),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_en_int_threshold),
	CONFIGFS_ATTR_ADD(attr_dbr_def_do_pacing),
	CONFIGFS_ATTR_ADD(attr_user_dbr_drop_recov),
	CONFIGFS_ATTR_ADD(attr_user_dbr_drop_recov_timeout),
	NULL,
};

static struct configfs_attribute *bnxt_re_p7_tun_attrs[] = {
	CONFIGFS_ATTR_ADD(attr_min_tx_depth),
	CONFIGFS_ATTR_ADD(attr_stats_query_sec),
	CONFIGFS_ATTR_ADD(attr_gsi_qp_mode),
	CONFIGFS_ATTR_ADD(attr_acc_tx_path),
	CONFIGFS_ATTR_ADD(attr_en_qp_dbg),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_enable),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_time),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_algo_threshold),
	CONFIGFS_ATTR_ADD(attr_dbr_pacing_en_int_threshold),
	CONFIGFS_ATTR_ADD(attr_dbr_def_do_pacing),
	CONFIGFS_ATTR_ADD(attr_user_dbr_drop_recov),
	CONFIGFS_ATTR_ADD(attr_user_dbr_drop_recov_timeout),
	NULL,
};

#ifdef HAVE_OLD_CONFIGFS_API
static ssize_t bnxt_re_tungrp_attr_show(struct config_item *item,
					struct configfs_attribute *attr,
					char *page)
{
	struct configfs_attr *tungrp_attr =
			container_of(attr, struct configfs_attr, attr);
	ssize_t rc = -EINVAL;

	if (!tungrp_attr)
		goto out;

	if (tungrp_attr->show)
		rc = tungrp_attr->show(item, page);
out:
	return rc;
}

static ssize_t bnxt_re_tungrp_attr_store(struct config_item *item,
					 struct configfs_attribute *attr,
					 const char *page, size_t count)
{
	struct configfs_attr *tungrp_attr =
			container_of(attr, struct configfs_attr, attr);
	ssize_t rc = -EINVAL;

	if (!tungrp_attr)
		goto out;
	if (tungrp_attr->store)
		rc = tungrp_attr->store(item, page, count);
out:
	return rc;
}

static struct configfs_item_operations bnxt_re_tungrp_ops = {
	.show_attribute         = bnxt_re_tungrp_attr_show,
	.store_attribute	= bnxt_re_tungrp_attr_store,
};

#else
static struct configfs_item_operations bnxt_re_tungrp_ops = {
};
#endif

static struct config_item_type bnxt_re_tungrp_type = {
	.ct_attrs = bnxt_re_tun_attrs,
	.ct_item_ops = &bnxt_re_tungrp_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_item_type bnxt_re_p7_tungrp_type = {
	.ct_attrs = bnxt_re_p7_tun_attrs,
	.ct_item_ops = &bnxt_re_tungrp_ops,
	.ct_owner = THIS_MODULE,
};

static int make_bnxt_re_tunables(struct bnxt_re_port_group *portgrp,
				 struct bnxt_re_dev *rdev, u32 gidx)
{
	struct bnxt_re_tunable_group *tungrp;
	int rc;

	tungrp = kzalloc(sizeof(*tungrp), GFP_KERNEL);
	if (!tungrp) {
		rc = -ENOMEM;
		goto out;
	}

	tungrp->rdev = rdev;
	if (_is_chip_p7(rdev->chip_ctx))
		config_group_init_type_name(&tungrp->group, "tunables",
					    &bnxt_re_p7_tungrp_type);
	else
		config_group_init_type_name(&tungrp->group, "tunables",
					    &bnxt_re_tungrp_type);
#ifndef HAVE_CFGFS_ADD_DEF_GRP
	portgrp->nportgrp.default_groups = portgrp->default_grp;
	portgrp->default_grp[gidx] = &tungrp->group;
	portgrp->default_grp[gidx + 1] = NULL;
#else
	configfs_add_default_group(&tungrp->group, &portgrp->nportgrp);
#endif
	portgrp->tungrp = tungrp;
	tungrp->portgrp = portgrp;

	return 0;
out:
	kfree(tungrp);
	return rc;
}


static void bnxt_re_release_nport_group(struct bnxt_re_port_group *portgrp)
{
	kfree(portgrp->ccgrp);
	kfree(portgrp->tungrp);
}

static struct config_item_type bnxt_re_nportgrp_type = {
        .ct_owner = THIS_MODULE,
};

static int make_bnxt_re_ports(struct bnxt_re_dev_group *devgrp,
			      struct bnxt_re_dev *rdev)
{
#ifndef HAVE_CFGFS_ADD_DEF_GRP
	struct config_group **portsgrp = NULL;
#endif
	struct bnxt_re_port_group *ports;
	struct ib_device *ibdev;
	int nports, rc, indx;

	if (!rdev)
		return -ENODEV;
	ibdev = &rdev->ibdev;
	devgrp->nports = ibdev->phys_port_cnt;
	nports = devgrp->nports;
	ports = kcalloc(nports, sizeof(*ports), GFP_KERNEL);
	if (!ports) {
		rc = -ENOMEM;
		goto out;
	}

#ifndef HAVE_CFGFS_ADD_DEF_GRP
	portsgrp = kcalloc(nports + 1, sizeof(*portsgrp), GFP_KERNEL);
	if (!portsgrp) {
		rc = -ENOMEM;
		goto out;
	}
#endif
	for (indx = 0; indx < nports; indx++) {
		char port_name[10];
		ports[indx].port_num = indx + 1;
		snprintf(port_name, sizeof(port_name), "%u", indx + 1);
		ports[indx].devgrp = devgrp;
		config_group_init_type_name(&ports[indx].nportgrp,
					    port_name, &bnxt_re_nportgrp_type);
		rc = make_bnxt_re_cc(&ports[indx], rdev, 0);
		if (rc)
			goto out;
		rc = make_bnxt_re_tunables(&ports[indx], rdev, 1);
		if (rc)
			goto out;
#ifndef HAVE_CFGFS_ADD_DEF_GRP
		portsgrp[indx] = &ports[indx].nportgrp;
#else
		configfs_add_default_group(&ports[indx].nportgrp,
					   &devgrp->port_group);
#endif
	}

#ifndef HAVE_CFGFS_ADD_DEF_GRP
	portsgrp[indx] = NULL;
	devgrp->default_portsgrp = portsgrp;
#endif
	devgrp->ports = ports;

	return 0;
out:
#ifndef HAVE_CFGFS_ADD_DEF_GRP
	kfree(portsgrp);
#endif
	kfree(ports);
	return rc;
}

static void bnxt_re_release_ports_group(struct bnxt_re_dev_group *devgrp)
{
	int i;

	/*
	 * nport group is dynamically created along with ports creation, so
	 * that it should also be released along with ports group release.
	 */
	for (i = 0; i < devgrp->nports; i++)
		bnxt_re_release_nport_group(&devgrp->ports[i]);

#ifndef HAVE_CFGFS_ADD_DEF_GRP
	kfree(devgrp->default_portsgrp);
	devgrp->default_portsgrp = NULL;
#endif
	kfree(devgrp->ports);
	devgrp->ports = NULL;
}

static void bnxt_re_release_device_group(struct config_item *item)
{
	struct config_group *group = container_of(item, struct config_group,
						  cg_item);
	struct bnxt_re_dev_group *devgrp =
				container_of(group, struct bnxt_re_dev_group,
					     dev_group);

	/*
	 * ports group is dynamically created along dev group creation, so that
	 * it should also be released along with dev group release.
	 */
	bnxt_re_release_ports_group(devgrp);

	kfree(devgrp);
}

static struct config_item_type bnxt_re_ports_group_type = {
	.ct_owner = THIS_MODULE,
};

static struct configfs_item_operations bnxt_re_dev_item_ops = {
	.release = bnxt_re_release_device_group
};

static struct config_item_type bnxt_re_dev_group_type = {
	.ct_item_ops = &bnxt_re_dev_item_ops,
	.ct_owner = THIS_MODULE,
};

static struct config_group *make_bnxt_re_dev(struct config_group *group,
					     const char *name)
{
	struct bnxt_re_dev_group *devgrp = NULL;
	struct bnxt_re_dev *rdev;
	int rc = -ENODEV;

	rdev = __get_rdev_from_name(name);
	if (PTR_ERR(rdev) == -ENODEV)
		goto out;

	devgrp = kzalloc(sizeof(*devgrp), GFP_KERNEL);
	if (!devgrp) {
		rc = -ENOMEM;
		goto out;
	}

	if (strlen(name) >= sizeof(devgrp->name)) {
		rc = -EINVAL;
		goto out;
	}
	strcpy(devgrp->name, name);
	config_group_init_type_name(&devgrp->port_group, "ports",
                                    &bnxt_re_ports_group_type);
	rc = make_bnxt_re_ports(devgrp, rdev);
	if (rc)
		goto out;
	config_group_init_type_name(&devgrp->dev_group, name,
                                    &bnxt_re_dev_group_type);
#ifndef HAVE_CFGFS_ADD_DEF_GRP
	devgrp->port_group.default_groups = devgrp->default_portsgrp;
	devgrp->dev_group.default_groups = devgrp->default_devgrp;
	devgrp->default_devgrp[0] = &devgrp->port_group;
	devgrp->default_devgrp[1] = NULL;
#else
	configfs_add_default_group(&devgrp->port_group,
				   &devgrp->dev_group);
#endif

	return &devgrp->dev_group;
out:
	kfree(devgrp);
	return ERR_PTR(rc);
}

static void drop_bnxt_re_dev(struct config_group *group, struct config_item *item)
{
	config_item_put(item);
}

static struct configfs_group_operations bnxt_re_group_ops = {
	.make_group = &make_bnxt_re_dev,
	.drop_item = &drop_bnxt_re_dev
};

static struct config_item_type bnxt_re_subsys_type = {
	.ct_group_ops	= &bnxt_re_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem bnxt_re_subsys = {
	.su_group	= {
		.cg_item	= {
			.ci_namebuf	= "bnxt_re",
			.ci_type	= &bnxt_re_subsys_type,
		},
	},
};

int bnxt_re_configfs_init(void)
{
	config_group_init(&bnxt_re_subsys.su_group);
	mutex_init(&bnxt_re_subsys.su_mutex);
	return configfs_register_subsystem(&bnxt_re_subsys);
}

void bnxt_re_configfs_exit(void)
{
	configfs_unregister_subsystem(&bnxt_re_subsys);
}
