/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XSCHED_NPU_INTERFACE_H
#define _XSCHED_NPU_INTERFACE_H

#include "ascend_hal_define.h"
#include "davinci_api.h"
#include "davinci_interface.h"
#include "davinci_intf_init.h"
#include "soc_res.h"
#include "syms_lookup.h"
#include "task_struct.h"
#include "trs_pub_def.h"
#include "trs_res_id_def.h"
#include "trs_proc.h"
#include "xsched.h"

/* NPU IOCTL handlers */
typedef int (*ioctl_trs_sqcq_handler_t)(struct trs_proc_ctx *proc_ctx, unsigned int cmd, unsigned long arg);
typedef struct uda_dev_inst *(*uda_dev_inst_get_handler_t)(u32 udevid);
typedef void (*uda_dev_inst_put_handler_t)(struct uda_dev_inst *dev_inst);
typedef int (*soc_subsys_get_num_handler_t)(u32 devid, enum soc_sub_type type, u32 *subnum);

extern ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_send_ptr;
extern ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_alloc_ptr;
extern ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_free_ptr;
extern ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_recv_ptr;

extern uda_dev_inst_get_handler_t uda_dev_inst_get_ptr;
extern uda_dev_inst_put_handler_t uda_dev_inst_put_ptr;
extern soc_subsys_get_num_handler_t soc_subsys_get_num_ptr;

static int __init ioctl_trs_sqcq_handler_find(void)
{
	if (!generic_kallsyms_lookup_name) {
		XSCHED_ERR("Generic kallsyms_lookup_name is NULL\n");
		return -ENOENT;
	}

	if (!ioctl_trs_sqcq_send_ptr) {
		ioctl_trs_sqcq_send_ptr = (ioctl_trs_sqcq_handler_t)generic_kallsyms_lookup_name("ioctl_trs_sqcq_send");
		if (!ioctl_trs_sqcq_send_ptr) {
			XSCHED_ERR("Failed to find ioctl_trs_sqcq_send symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found ioctl_trs_sqcq_send at %p\n", ioctl_trs_sqcq_send_ptr);
	}

	if (!ioctl_trs_sqcq_alloc_ptr) {
		ioctl_trs_sqcq_alloc_ptr = (ioctl_trs_sqcq_handler_t)generic_kallsyms_lookup_name("ioctl_trs_sqcq_alloc");
		if (!ioctl_trs_sqcq_alloc_ptr) {
			XSCHED_ERR("Failed to find ioctl_trs_sqcq_alloc symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found ioctl_trs_sqcq_alloc at %p\n", ioctl_trs_sqcq_alloc_ptr);
	}

	if (!ioctl_trs_sqcq_free_ptr) {
		ioctl_trs_sqcq_free_ptr = (ioctl_trs_sqcq_handler_t)generic_kallsyms_lookup_name("ioctl_trs_sqcq_free");
		if (!ioctl_trs_sqcq_free_ptr) {
			XSCHED_ERR("Failed to find ioctl_trs_sqcq_free symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found ioctl_trs_sqcq_free at %p\n", ioctl_trs_sqcq_free_ptr);
	}

	if (!ioctl_trs_sqcq_recv_ptr) {
		ioctl_trs_sqcq_recv_ptr = (ioctl_trs_sqcq_handler_t)generic_kallsyms_lookup_name("ioctl_trs_sqcq_recv");
		if (!ioctl_trs_sqcq_recv_ptr) {
			XSCHED_ERR("Failed to find ioctl_trs_sqcq_recv symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found ioctl_trs_sqcq_recv at %p\n", ioctl_trs_sqcq_recv_ptr);
	}

	if (!uda_dev_inst_get_ptr) {
		uda_dev_inst_get_ptr = (uda_dev_inst_get_handler_t)generic_kallsyms_lookup_name("uda_dev_inst_get");
		if (!uda_dev_inst_get_ptr) {
			XSCHED_ERR("Failed to find uda_dev_inst_get symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found uda_dev_inst_get at %p\n", uda_dev_inst_get_ptr);
	}

	if (!uda_dev_inst_put_ptr) {
		uda_dev_inst_put_ptr = (uda_dev_inst_put_handler_t)generic_kallsyms_lookup_name("uda_dev_inst_put");
		if (!uda_dev_inst_put_ptr) {
			XSCHED_ERR("Failed to find uda_dev_inst_get symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found uda_dev_inst_put at %p\n", uda_dev_inst_put_ptr);
	}

	if (!soc_subsys_get_num_ptr) {
		soc_subsys_get_num_ptr = (soc_subsys_get_num_handler_t)generic_kallsyms_lookup_name("soc_resmng_subsys_get_num");
		if (!soc_subsys_get_num_ptr) {
			XSCHED_ERR("Failed to find uda_dev_inst_get symbol\n");
			return -ENOENT;
		}
		XSCHED_INFO("Found soc_resmng_subsys_get_num at %p\n", soc_subsys_get_num_ptr);
	}

	return 0;
}

static void __exit ioctl_trs_sqcq_handler_free(void)
{
	ioctl_trs_sqcq_send_ptr = NULL;
	ioctl_trs_sqcq_alloc_ptr = NULL;
	ioctl_trs_sqcq_free_ptr = NULL;
	ioctl_trs_sqcq_recv_ptr = NULL;
	uda_dev_inst_get_ptr = NULL;
	uda_dev_inst_put_ptr = NULL;
	soc_subsys_get_num_ptr = NULL;
}

int xcu_populate(uint32_t dev_id);
int xcu_depopulate(uint32_t dev_id);
#endif /* _XSCHED_NPU_INTERFACE_H */
