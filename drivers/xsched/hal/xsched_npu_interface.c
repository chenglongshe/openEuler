/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/vmalloc.h>
#include <linux/file.h>

#include "xcu_group.h"
#include "xsched_npu_interface.h"

extern int xsched_xcu_register(struct xcu_group *group, int phys_id);
extern int xsched_xcu_unregister(struct xcu_group *group, int phys_id);
ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_send_ptr;
ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_alloc_ptr;
ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_free_ptr;
ioctl_trs_sqcq_handler_t ioctl_trs_sqcq_recv_ptr;
uda_dev_inst_get_handler_t uda_dev_inst_get_ptr;
uda_dev_inst_put_handler_t uda_dev_inst_put_ptr;
soc_subsys_get_num_handler_t soc_subsys_get_num_ptr;

/* Gets device driver TS context from a file descriptor of opened device. */
static void *tsdrv_ctx_find(int fd)
{
	struct davinci_intf_private_stru *file_private_data;
	void *ctx = NULL;
	struct fd f;

	f = fdget(fd);
	if (!f.file)
		return NULL;

	file_private_data = f.file->private_data;
	if (file_private_data)
		ctx = file_private_data->priv_filep.private_data;

	fdput(f);
	return ctx;
}

int trs_xsched_ctx_run(struct xcu_op_handler_params *params)
{
	uint32_t sq_id = *(uint32_t *)params->param_1;
	uint32_t tsId = *(uint32_t *)params->param_2;
	uint8_t *sqe_addr = params->param_3;
	uint32_t sqe_num = *(uint32_t *)params->param_4;
	int32_t timeout = *(int32_t *)params->param_5;
	int32_t type = *(int32_t *)params->param_6;
	struct halTaskSendInfo input = {0};
	struct trs_proc_ctx *ctx = params->param_7;
	uint32_t logic_cqId = *(uint32_t *)params->param_8;

	input.tsId = tsId;
	input.sqId = sq_id;
	input.timeout = timeout;
	input.sqe_addr = sqe_addr;
	input.sqe_num = sqe_num;
	input.type = type;

	XSCHED_DEBUG("%s %d: tsId %u sqId %u timeout %d num %u\n",
		__func__, __LINE__, tsId, sq_id, timeout, sqe_num);

	if (!ioctl_trs_sqcq_send_ptr) {
		XSCHED_ERR("Invalid ioctl_trs_sqcq_send_ptr %p @ %s\n", ioctl_trs_sqcq_send_ptr, __func__);
		return -ENOENT;
	}
	/* Send SQ tail to a doorbel. */
	return ioctl_trs_sqcq_send_ptr(ctx, logic_cqId, (unsigned long)&input);
}

int trs_xsched_ctx_free(struct xcu_op_handler_params *params)
{
	struct trs_proc_ctx *ctx;

	ctx = tsdrv_ctx_find(params->fd);
	if (!ctx)
		return -ENOENT;

	if (!ioctl_trs_sqcq_free_ptr) {
		XSCHED_ERR("Invalid ioctl_trs_sqcq_free_ptr %p @ %s\n", ioctl_trs_sqcq_free_ptr, __func__);
		return -ENOENT;
	}
	return ioctl_trs_sqcq_free_ptr(ctx, 0, (unsigned long)params->payload);
}

int trs_xsched_ctx_wait(struct xcu_op_handler_params *params)
{
	uint32_t tsId = *(uint32_t *)params->param_1;
	uint32_t cqId = *(uint32_t *)params->param_2;
	uint32_t streamId = *(uint32_t *)params->param_3;
	struct ts_stars_sqe_header *sqe = params->param_4;
	uint8_t *cqe_addr = params->param_5;
	struct trs_proc_ctx *ctx = params->param_6;
	int32_t timeout = *(uint32_t *)params->param_7;
	int32_t cqe_num = 1;
	struct halReportRecvInfo input = {0};
	uint32_t task_id = sqe->task_id;

	input.type = DRV_LOGIC_TYPE;
	input.tsId = tsId;
	input.cqId = cqId;
	input.timeout = timeout;
	input.cqe_num = cqe_num;
	input.cqe_addr = cqe_addr;
	input.stream_id = streamId;
	input.task_id = task_id;
	input.res[0] = 1; /* version 1 for new runtime. */

	XSCHED_DEBUG("%s %d: tdId %u logic_cqId %u streamid %u task_id %d timeout %d\n",
		__func__, __LINE__, tsId, cqId, streamId, task_id, timeout);

	if (!ioctl_trs_sqcq_recv_ptr) {
		XSCHED_ERR("Invalid ioctl_trs_sqcq_recv_ptr %p @ %s\n", ioctl_trs_sqcq_recv_ptr, __func__);
		return -ENOENT;
	}
	/* Wait for cq irq and read result. */
	return ioctl_trs_sqcq_recv_ptr(ctx, 0, (unsigned long)&input);
}

int trs_xsched_ctx_complete(struct xcu_op_handler_params *params)
{
	return 0;
}

int trs_xsched_ctx_alloc(struct xcu_op_handler_params *params)
{
	struct halSqCqInputInfo *input_info = params->payload;
	uint32_t *tgid = (uint32_t *)params->param_1;
	uint32_t *sq_id = (uint32_t *)params->param_2;
	uint32_t *cq_id = (uint32_t *)params->param_3;
	uint32_t *user_stream_id = (uint32_t *)params->param_4;
	struct trs_proc_ctx *ctx;
	int ret = 0;

	XSCHED_DEBUG("%s %d, input_info %lx, type: %d\n",
		__func__, __LINE__, (unsigned long)input_info, input_info->type);

	ctx = tsdrv_ctx_find(params->fd);
	if (!ctx)
		return -ENOENT;
	XSCHED_DEBUG("%s %d, pid %d, task_id %d, size %ld\n",
		__func__, __LINE__, ctx->pid, ctx->task_id, sizeof(*ctx));

	if (!ioctl_trs_sqcq_alloc_ptr) {
		XSCHED_ERR("Invalid ioctl_trs_sqcq_alloc_ptr %p @ %s\n", ioctl_trs_sqcq_alloc_ptr, __func__);
		return -ENOENT;
	}
	ret = ioctl_trs_sqcq_alloc_ptr(ctx, 0, (unsigned long)input_info);
	if (ret != 0)
		return ret;

	*tgid = ctx->pid;
	*sq_id = input_info->sqId;
	*cq_id = input_info->cqId;
	*user_stream_id = input_info->info[0];
	params->param_5 = ctx;
	return 0;
}

int trs_xsched_ctx_logic_alloc(struct xcu_op_handler_params *params)
{
	struct halSqCqInputInfo *input_info = params->payload;
	uint32_t *logic_cq_id = (uint32_t *)params->param_1;
	struct trs_proc_ctx *ctx;
	int ret = 0;

	XSCHED_DEBUG("%s %d, type: %d\n", __func__, __LINE__, input_info->type);

	ctx = tsdrv_ctx_find(params->fd);
	if (!ctx)
		return -ENOENT;
	XSCHED_DEBUG("%s %d, pid %d, task_id %d, size %ld\n",
		__func__, __LINE__, ctx->pid, ctx->task_id, sizeof(*ctx));

	if (!ioctl_trs_sqcq_alloc_ptr) {
		XSCHED_ERR("Invalid ioctl_trs_sqcq_alloc_ptr %p @ %s\n", ioctl_trs_sqcq_alloc_ptr, __func__);
		return -ENOENT;
	}
	ret = ioctl_trs_sqcq_alloc_ptr(ctx, 0, (unsigned long)input_info);
	if (ret != 0)
		return ret;

	*logic_cq_id = input_info->cqId;
	XSCHED_DEBUG("%s %d, type: %d, cq_id: %u\n",
		__func__, __LINE__, input_info->type, *logic_cq_id);
	return 0;
}

int trs_xsched_ctx_sqe_op(struct xcu_op_handler_params *params)
{
	struct ts_stars_sqe_header *sqe = params->param_2;
	int op_type = *(int *)(params->param_1);

	switch (op_type) {
	case SQE_IS_NOTIFY:
		return (sqe->type == 0) && (sqe->wr_cqe == 1);
	case SQE_SET_NOTIFY:
		if (sqe->type == 0)
			sqe->wr_cqe = 1;
		break;
	default:
		break;
	}

	return 0;
}

static struct xcu_operation trs_xsched_ctx_xcu_ops = {
	.run = trs_xsched_ctx_run,
	.finish = trs_xsched_ctx_free,
	.wait = trs_xsched_ctx_wait,
	.complete = trs_xsched_ctx_complete,
	.alloc = trs_xsched_ctx_alloc,
	.logic_alloc = trs_xsched_ctx_logic_alloc,
	.sqe_op = trs_xsched_ctx_sqe_op,
};

/*
 * build xcu_group like
 *                    xcu_root
 *                      /
 *                     910
 *                   /    \
 *                dev0     dev1
 *               /    \
 *        channel0    channel1
 */
int xsched_xcu_group_init(
	uint32_t type, uint32_t dev_id, uint32_t channel_num)
{
	struct xcu_group *type_group, *dev_group, *channel_group;
	int channel_id, err = 0;

	XSCHED_DEBUG("dev_id %u channel_num %u\n", dev_id, channel_num);

	type_group = xcu_group_find(xcu_group_root, type);
	if (!type_group) {
		type_group = xcu_group_init(type);
		if (!type_group) {
			XSCHED_ERR("Fail to alloc xcu group with NPU\n");
			return -ENOMEM;
		}

		err = xcu_group_attach(type_group, xcu_group_root);
		if (err) {
			XSCHED_ERR("Fail to attach NPU group\n");
			xcu_group_free(type_group);
			return err;
		}
	}

	dev_group = xcu_group_init(dev_id);
	if (!dev_group) {
		XSCHED_ERR("Fail to alloc device group id=%u\n", dev_id);
		return -ENOMEM;
	}
	dev_group->id = dev_id;

	err = xcu_group_attach(dev_group, type_group);
	if (err) {
		XSCHED_ERR("Fail to attach device group id=%u\n", dev_id);
		xcu_group_free(dev_group);
		return err;
	}

	for (channel_id = 0; channel_id < channel_num; channel_id++) {
		channel_group = xcu_group_init(channel_id);
		if (!channel_group) {
			XSCHED_ERR("Fail to alloc channel group id=%u\n", channel_id);
			err = -ENOMEM;
			continue;
		}
		channel_group->opt = &trs_xsched_ctx_xcu_ops;

		err = xcu_group_attach(channel_group, dev_group);
		if (err) {
			XSCHED_ERR("Fail to attach channel group id=%u\n", channel_id);
			xcu_group_free(channel_group);
			continue;
		}

		/* one xcu map to a channel group */
		err = xsched_xcu_register(channel_group, dev_id);
		if (err) {
			XSCHED_ERR("Fail to register channel_id=%u dev_id=%u\n",
			       channel_id, dev_id);
			xcu_group_detach(channel_group);
			xcu_group_free(channel_group);
			continue;
		}

		cond_resched();
	}

	return err;
}

void xsched_xcu_group_exit(
	uint32_t type, uint32_t dev_id, uint32_t channel_num)
{
	struct xcu_group *type_group, *dev_group, *channel_group;
	int channel_id;

	type_group = xcu_group_find(xcu_group_root, type);
	if (!type_group)
		return;

	dev_group = xcu_group_find(type_group, dev_id);
	if (!dev_group)
		goto check_type_group;

	for (channel_id = 0; channel_id < channel_num; channel_id++) {
		channel_group = xcu_group_find(dev_group, channel_id);
		if (!channel_group)
			continue;

		xsched_xcu_unregister(channel_group, dev_id);
		xcu_group_detach(channel_group);
		xcu_group_free(channel_group);

		cond_resched();
	}
	xcu_group_detach(dev_group);
	xcu_group_free(dev_group);

check_type_group:
	if (xcu_group_is_empty(type_group)) {
		xcu_group_detach(type_group);
		xcu_group_free(type_group);
	}
}

int xcu_populate(uint32_t dev_id)
{
	int ret;
	uint32_t ts_num;
	struct uda_dev_inst *dev_inst = uda_dev_inst_get_ptr(dev_id);

	if (!dev_inst)
		return 0;

	/* Get number of TS in a device. */
	ret = soc_subsys_get_num_ptr(dev_id, TS_SUBSYS, &ts_num);
	if (ret) {
		XSCHED_ERR("Get ts num fail. (ret=%d; dev_id=%u; ts_num=%u)\n", ret, dev_id, ts_num);
		goto err_out;
	}

	ret = xsched_xcu_group_init(XCU_TYPE_NPU, dev_id, ts_num);
	if (ret) {
		XSCHED_ERR("Failed to initialize xcu group (dev_id=%u; ts_num=%u)\n", dev_id, ts_num);
		goto err_out;
	}
	XSCHED_INFO("Registered device: dev_id=%d, ts_num=%d\n", dev_id, ts_num);

err_out:
	uda_dev_inst_put_ptr(dev_inst);
	return ret;
}

int xcu_depopulate(uint32_t dev_id)
{
	int ret;
	uint32_t ts_num;
	struct uda_dev_inst *dev_inst = uda_dev_inst_get_ptr(dev_id);

	if (!dev_inst)
		return 0;

	/* Get number of TS in a device. */
	ret = soc_subsys_get_num_ptr(dev_id, TS_SUBSYS, &ts_num);
	if (ret) {
		XSCHED_ERR("Get ts num fail. (ret=%d; dev_id=%u; ts_num=%u)\n", ret, dev_id, ts_num);
		goto err_out;
	}

	xsched_xcu_group_exit(XCU_TYPE_NPU, dev_id, ts_num);
	XSCHED_INFO("Unregistered device: dev_id=%d, ts_num=%d\n", dev_id, ts_num);

err_out:
	uda_dev_inst_put_ptr(dev_inst);
	return ret;
}
