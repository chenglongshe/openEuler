// SPDX-License-Identifier: GPL-2.0+
/*
 * Vstream manage for XPU device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
 *
 * Author: Konstantin Meskhidze <konstantin.meskhidze@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/syscalls.h>
#include <linux/vstream.h>
#include <linux/xsched.h>

#ifdef CONFIG_XCU_VSTREAM
#define MAX_VSTREAM_NUM (512 - 1)

static DEFINE_MUTEX(vs_mutex);
static vstream_info_t *vstream_array[MAX_VSTREAM_NUM];
static void init_xsched_ctx(struct xsched_context *ctx,
			    const struct vstream_info *vs)
{
	ctx->tgid = vs->tgid;
	ctx->fd = vs->fd;
	ctx->devId = vs->devId;
	kref_init(&ctx->kref);

	INIT_LIST_HEAD(&ctx->vstream_list);
	INIT_LIST_HEAD(&ctx->ctx_node);

	spin_lock_init(&ctx->ctx_lock);
	mutex_init(&ctx->ctx_mutex);
}

/* Allocates a new xsched_context if a new vstream_info is bound
 * to a device that no other vstream that is currently present
 * is bound to.
 */
static int alloc_ctx_from_vstream(struct vstream_info *vstream_info,
				  struct xsched_context **ctx)
{
	int err = 0;

	XSCHED_CALL_STUB();

	*ctx = find_ctx_by_tgid(vstream_info->tgid);
	if (*ctx) {
		XSCHED_INFO("Ctx %d found @ %s\n",
				vstream_info->tgid, __func__);
		goto out_err;
	}

	*ctx = kzalloc(sizeof(struct xsched_context), GFP_KERNEL);

	if (!*ctx) {
		XSCHED_ERR("Could not allocate xsched context (tgid=%d) @ %s\n",
			   vstream_info->tgid, __func__);
		err = -ENOMEM;
		goto out_err;
	}

	init_xsched_ctx(*ctx, vstream_info);

	err = xsched_ctx_init_xse(*ctx, vstream_info);

	if (err) {
		XSCHED_ERR("Failed to initialize XSE for context @ %s\n",
			   __func__);
		kfree(*ctx);
		err = -EINVAL;
		goto out_err;
	}

	list_add(&(*ctx)->ctx_node, &xsched_ctx_list);

out_err:
	XSCHED_EXIT_STUB();

	return err;
}

/* Bounds a new vstream_info object to a corresponding xsched context. */
static int vstream_bind_to_ctx(struct vstream_info *vs)
{
	struct xsched_context *ctx = NULL;
	int alloc_err = 0;

	XSCHED_CALL_STUB();

	XSCHED_INFO("Ctx list mutext taken @ %s\n", __func__);
	mutex_lock(&xsched_ctx_list_mutex);

	ctx = find_ctx_by_tgid(vs->tgid);
	if (ctx) {
		XSCHED_INFO("Ctx %d found @ %s\n", vs->tgid, __func__);
		kref_get(&ctx->kref);
	} else {
		alloc_err = alloc_ctx_from_vstream(vs, &ctx);
		if (alloc_err)
			goto out_err;
	}

	XSCHED_INFO("Ctx = %p @ %s\n", ctx, __func__);

	vs->ctx = ctx;
	vs->xcu = ctx->xse.xcu;
	ctx->devId = vs->devId;
	list_add(&vs->ctx_node, &vs->ctx->vstream_list);

out_err:
	mutex_unlock(&xsched_ctx_list_mutex);
	XSCHED_INFO("Ctx list mutex released @ %s\n", __func__);

	XSCHED_EXIT_STUB();

	return alloc_err;
}

static vstream_info_t *vstream_create_info(struct vstream_args *arg)
{
	struct vstream_info *vstream;

	XSCHED_CALL_STUB();

	vstream = kzalloc(sizeof(vstream_info_t), GFP_KERNEL);
	if (!vstream) {
		XSCHED_ERR("Failed to allocate vstream.\n");
		vstream = NULL;
		goto out_err;
	}

	vstream->devId = arg->devid;
	vstream->channel_id = arg->channel_id;

	INIT_LIST_HEAD(&vstream->ctx_node);
	INIT_LIST_HEAD(&vstream->xcu_node);
	INIT_LIST_HEAD(&vstream->metadata_list);

	spin_lock_init(&vstream->stream_lock);

	vstream->kicks_count = 0;

	vstream->xcu = NULL;

out_err:
	XSCHED_EXIT_STUB();

	return vstream;
}

static int vstream_add(vstream_info_t *vstream, uint32_t id)
{
	XSCHED_CALL_STUB();
	XSCHED_INFO("Adding vstream %u @ %s\n", id, __func__);

	if (id >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("vstreamId out of range.\n");
		return -EINVAL;
	}

	mutex_lock(&vs_mutex);
	if (vstream_array[id] != NULL) {
		mutex_unlock(&vs_mutex);
		XSCHED_ERR("VstreamId=%u cell is busy.\n", id);
		return -EINVAL;
	}
	vstream_array[id] = vstream;
	mutex_unlock(&vs_mutex);

	return 0;
}

static int vstream_del(uint32_t vstreamid)
{
	XSCHED_INFO("Deleting vstream %u @ %s\n", vstreamid, __func__);

	if (vstreamid >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("VstreamId=%u out of range.\n", vstreamid);
		return -EINVAL;
	}

	mutex_lock(&vs_mutex);
	if (vstream_array[vstreamid] != NULL) {
		vstream_array[vstreamid] = NULL;
		mutex_unlock(&vs_mutex);
		return 0;
	}
	mutex_unlock(&vs_mutex);

	XSCHED_ERR("vstream_array[%u] is already empty.\n", vstreamid);

	return 0;
}

static vstream_info_t *vstream_get(uint32_t vstreamid)
{
	vstream_info_t *vstream = NULL;

	if (vstreamid >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("VstreamId=%u out of range.\n", vstreamid);
		return NULL;
	}

	mutex_lock(&vs_mutex);
	vstream = vstream_array[vstreamid];
	mutex_unlock(&vs_mutex);

	return vstream;
}

static vstream_info_t *
vstream_get_by_user_stream_id(uint32_t user_streamId)
{
	int id;

	for (id = 0; id < MAX_VSTREAM_NUM; id++) {
		if (vstream_array[id] != NULL)
			if (vstream_array[id]->user_streamId ==
			    user_streamId)
				return vstream_array[id];
	}
	return NULL;
}

static int sqcq_alloc(struct vstream_args *arg)
{
	vstream_alloc_args_t *va_args = &arg->va_args;
	struct xsched_context *ctx = NULL;
	struct xcu_op_handler_params params;
	uint32_t logic_cq_id = 0;
	vstream_info_t *vstream;
	int err = -EINVAL;
	uint32_t tgid = 0;
	uint32_t cq_id = 0;
	uint32_t sq_id = 0;

	XSCHED_CALL_STUB();

	vstream = vstream_create_info(arg);
	if (!vstream) {
		XSCHED_ERR("vstream create failed.\n");
		err = -ENOSPC;
		goto out_err;
	}
	vstream->fd = arg->fd;
	vstream->task_type = arg->task_type;

	err = bind_vstream_to_xcu(vstream);
	if (err) {
		XSCHED_ERR(
			"Couldn't find valid xcu for vstream dev_id=%u chan_id=%u @ %s\n",
			vstream->devId, vstream->channel_id, __func__);
		err = -EINVAL;
		goto out_err_vstream_free;
	}

	/* Allocates vstream's SQ and CQ memory on a XCU for processing. */
	params.group = vstream->xcu->group;
	params.fd = arg->fd;
	params.payload = arg->payload;
	params.param_1 = &tgid;
	params.param_2 = &sq_id;
	params.param_3 = &cq_id;
	params.param_4 = &logic_cq_id;
	err = xcu_alloc(&params);
	if (err) {
		XSCHED_ERR(
			"Failed to allocate SQ and CQ memory to an app stream.\n");
		goto out_err_vstream_free;
	}
	vstream->drv_ctx = params.param_5;
	vstream->id = sq_id;
	vstream->vcqId = cq_id;
	vstream->logic_vcqId = logic_cq_id;
	XSCHED_INFO("New vstream id %u, cq_id=%u, logic_cqid=%u @ %s\n",
		    vstream->id, vstream->vcqId, vstream->logic_vcqId, __func__);

	vstream->user_streamId = va_args->user_stream_id;
	vstream->tgid = tgid;
	vstream->sqcq_type = va_args->type;


	err = vstream_bind_to_ctx(vstream);
	if (err) {
		XSCHED_ERR(
			"Failed to bind vstream %u to an app context.\n",
			vstream->id);
		goto out_err_vstream_free;
	}

	ctx = vstream->ctx;

	err = vstream_add(vstream, vstream->id);
	if (err) {
		XSCHED_ERR(
			"Failed to add vstream id=%u to vstream_array.\n",
			vstream->id);
		goto out_err_vstream_free;
	}

	XSCHED_INFO(
		"vstream allocation success: user_streamId=%u, sqid=%u, cqid=%u, ctx=%p.\n",
		vstream->user_streamId, vstream->id, vstream->vcqId, ctx);

	XSCHED_EXIT_STUB();

	return 0;

out_err_vstream_free:
	kfree(vstream);

out_err:
	XSCHED_INFO(
		"Exit %s with error, current_pid=%d, err=%d.\n",
		__func__, current->pid, err);

	return err;
}

static int logic_cq_alloc(struct vstream_args *arg)
{
	int err = 0;
	struct xcu_op_handler_params params;
	vstream_info_t *vstream = NULL;
	vstream_alloc_args_t *logic_cq_alloc_para = &arg->va_args;
	uint32_t logic_cq_id = 0;

	XSCHED_CALL_STUB();

	XSCHED_INFO(
		"Enter %s, current_pid=%d, fd=%u.\n",
		__func__, current->pid, arg->fd);

	vstream = vstream_get_by_user_stream_id(
		logic_cq_alloc_para->user_stream_id);
	if (!vstream) {
		struct xsched_cu *xcu_found = NULL;
		__u32 type = XCU_TYPE_NPU;

		xcu_found = xcu_find(&type, arg->devid, arg->channel_id);
		if (xcu_found == NULL) {
			XSCHED_ERR(
				"Couldn't find valid xcu for control vstream dev_id=%u chan_id=%u @ %s\n",
				arg->devid, arg->channel_id, __func__);
			err = -EINVAL;
			goto out_err;
		}

		/* Allocates control vstream's SQ and CQ memory on a XCU for processing. */
		params.group = xcu_found->group;
		params.fd = arg->fd;
		params.payload = arg->payload;
		params.param_1 = &logic_cq_id;

		err = xcu_logic_alloc(&params);
		if (err) {
			XSCHED_ERR(
				"Failed to allocate logic CQ memory to a vstream.\n");
			goto out_err;
		}

		XSCHED_INFO(
			"vstream logic CQ memory allocation success: logic_cq_id=%u.\n",
			logic_cq_id);

		return 0;
	}

	params.group = vstream->xcu->group;
	params.fd = arg->fd;
	params.payload = arg->payload;
	params.param_1 = &logic_cq_id;

	err = xcu_logic_alloc(&params);
	if (err) {
		XSCHED_ERR(
			"Failed to allocate logic CQ memory to an app stream.\n");
		goto out_err;
	}

	vstream->logic_vcqId = logic_cq_id;

	XSCHED_INFO(
		"Vstream logic CQ memory allocation success: user_streamId=%u, logic_cqid=%u.\n",
		vstream->user_streamId, vstream->logic_vcqId);

	return 0;

out_err:
	XSCHED_INFO(
		"Exit %s with error, current_pid=%d, err=%d.\n",
		__func__, current->pid, err);

	return err;
}

int vstream_alloc(struct vstream_args *arg)
{
	vstream_alloc_args_t *va_args = &arg->va_args;
	int ret;

	XSCHED_CALL_STUB();
	XSCHED_INFO(
		"Enter %s, current_pid=%d, fd=%u, type=%d.\n",
		__func__, current->pid, arg->fd, va_args->type);

	if (!va_args->type)
		ret = sqcq_alloc(arg);
	else
		ret = logic_cq_alloc(arg);

	XSCHED_EXIT_STUB();
	return ret;
}

int vstream_free(struct vstream_args *arg)
{
	struct xcu_op_handler_params params;
	uint32_t vstreamId = arg->sq_id;
	struct xsched_context *ctx = NULL;
	struct xsched_entity *xse = NULL;
	vstream_info_t *vstream = NULL;
	int err = 0;

	XSCHED_CALL_STUB();

	vstream = vstream_get(vstreamId);
	if (!vstream) {
		XSCHED_ERR("Vstream get failed, vstreamId=%u.\n",
			   vstreamId);
		err = -ENOMEM;
		goto out_err;
	}

	err = vstream_del(vstream->id);
	if (err)
		goto out_err;

	params.group = vstream->xcu->group;
	params.fd = arg->fd;
	params.payload = arg->payload;
	err = xcu_finish(&params);

	if (err) {
		XSCHED_ERR(
			"Failed to free vstream's SQ/CQ queues on the device sqId=%u, cqId=%u.\n",
			arg->sq_id, arg->cq_id);
		goto out_err;
	}

	xse = &vstream->ctx->xse;
	ctx = vstream->ctx;
	kref_put(&ctx->kref, xsched_free_task);

out_err:
	XSCHED_EXIT_STUB();

	return err;
}

int vstream_kick(struct vstream_args *arg)
{
	return 0;
}

/*
 * vstream_manage_cmd table
 */
static vstream_manage_t(*vstream_command_table[MAX_COMMAND + 1]) = {
	vstream_alloc, // VSTREAM_ALLOC
	vstream_free, // VSTREAM_FREE
	vstream_kick, // VSTREAM_KICK
	NULL // MAX_COMMAND
};

SYSCALL_DEFINE2(vstream_manage, struct vstream_args __user *, arg, int, cmd)
{
	int res = 0;
	struct vstream_args vstream_arg;

	if (copy_from_user(&vstream_arg, arg, sizeof(struct vstream_args))) {
		pr_err("copy_from_user failed\n");
		return -EFAULT;
	}

	res = vstream_command_table[cmd](&vstream_arg);
	if (copy_to_user(arg, &vstream_arg, sizeof(struct vstream_args))) {
		pr_err("copy_to_user failed\n");
		return -EFAULT;
	}

	pr_debug("vstream_manage: cmd %d\n", cmd);
	return res;
}
#else
SYSCALL_DEFINE2(vstream_manage, struct vstream_args __user *, arg, int, cmd)
{
	return 0;
}
#endif
