// SPDX-License-Identifier: GPL-2.0+
/*
 * Vstream manage for XPU device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
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
#include <linux/anon_inodes.h>
#include <linux/vstream.h>
#include <linux/xsched.h>
#include <linux/delay.h>

#ifdef CONFIG_XCU_VSTREAM
static int vstream_del(vstream_info_t *vstream, uint32_t vstream_id);
static int vstream_file_release(struct inode *inode, struct file *file);
static const struct file_operations vstreamfd_fops = {
	.release = vstream_file_release,
};

static inline struct file *vstream_file_get(int vs_fd)
{
	return fget(vs_fd);
}

static inline void vstream_file_put(struct file *vstream_file)
{
	fput(vstream_file);
}

static int vstream_file_create(struct vstream_info *vs)
{
	return anon_inode_getfd("[vstreamfd]", &vstreamfd_fops, vs,
		O_RDWR | O_CLOEXEC | O_NONBLOCK);
}

static int vstream_destroy(vstream_info_t *vstream)
{
	int err;
	struct xsched_context *ctx = NULL;
	struct xsched_entity *xse = NULL;

	err = vstream_del(vstream, vstream->id);
	if (err)
		return err;

	xse = &vstream->ctx->xse;
	ctx = vstream->ctx;
	kref_put(&ctx->kref, xsched_task_free);

	return 0;
}

static int vstream_file_release(struct inode *inode, struct file *file)
{
	vstream_info_t *vstream;
	(void) inode;

	if (!file->private_data)
		return 0;

	vstream = file->private_data;
	return vstream_destroy(vstream);
}

static void init_xsched_ctx(struct xsched_context *ctx,
				const struct vstream_info *vs)
{
	ctx->tgid = vs->tgid;
	ctx->fd = vs->fd;
	ctx->dev_id = vs->dev_id;
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
	struct xsched_cu *xcu = vstream_info->xcu;

	*ctx = ctx_find_by_tgid_and_xcu(vstream_info->tgid, xcu);
	if (*ctx)
		return 0;

	*ctx = kzalloc(sizeof(struct xsched_context), GFP_KERNEL);
	if (!*ctx) {
		XSCHED_ERR("Fail to alloc xsched context (tgid=%d) @ %s\n",
			vstream_info->tgid, __func__);
		return -ENOMEM;
	}

	init_xsched_ctx(*ctx, vstream_info);

	if (xsched_ctx_init_xse(*ctx, vstream_info) != 0) {
		XSCHED_ERR("Fail to initialize XSE for context @ %s\n",
			__func__);
		kfree(*ctx);
		return -EINVAL;
	}
	list_add(&(*ctx)->ctx_node, &xcu->ctx_list);
	++xcu->nr_ctx;

	return 0;
}

/* Bounds a new vstream_info object to a corresponding xsched context. */
static int vstream_bind_to_ctx(struct vstream_info *vs)
{
	struct xsched_context *ctx = NULL;
	struct xsched_cu *xcu = vs->xcu;
	int err = 0;

	mutex_lock(&xcu->ctx_list_lock);
	ctx = ctx_find_by_tgid_and_xcu(vs->tgid, xcu);
	if (ctx) {
		XSCHED_DEBUG("Ctx %d found @ %s\n", vs->tgid, __func__);
		kref_get(&ctx->kref);
	} else {
		err = alloc_ctx_from_vstream(vs, &ctx);
		if (err)
			goto out_err;
	}

	vs->ctx = ctx;
	list_add(&vs->ctx_node, &vs->ctx->vstream_list);

out_err:
	mutex_unlock(&xcu->ctx_list_lock);
	return err;
}

static vstream_info_t *vstream_create(struct vstream_args *arg)
{
	struct vstream_info *vstream = NULL;

	vstream = kzalloc(sizeof(vstream_info_t), GFP_KERNEL);
	if (!vstream) {
		XSCHED_ERR("Failed to allocate vstream.\n");
		return NULL;
	}

	vstream->dev_id = arg->dev_id;
	vstream->channel_id = arg->channel_id;
	vstream->kicks_count = 0;
	vstream->xcu = NULL;

	INIT_LIST_HEAD(&vstream->ctx_node);
	INIT_LIST_HEAD(&vstream->xcu_node);
	INIT_LIST_HEAD(&vstream->metadata_list);

	spin_lock_init(&vstream->stream_lock);

	return vstream;
}

static int vstream_add(vstream_info_t *vstream, uint32_t id)
{
	int err = 0;
	struct xsched_cu *xcu = vstream->xcu;

	if (id >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("Vstream id=%u out of range @ %s.\n",
			id, __func__);
		return -EINVAL;
	}

	mutex_lock(&xcu->vs_array_lock);
	if (xcu->vs_array[id] != NULL) {
		XSCHED_ERR("Vstream id=%u cell is busy.\n", id);
		err = -EINVAL;
		goto out_err;
	}
	xcu->vs_array[id] = vstream;

out_err:
	mutex_unlock(&xcu->vs_array_lock);
	return err;
}

static int vstream_del(vstream_info_t *vstream, uint32_t vstream_id)
{
	struct xsched_cu *xcu = vstream->xcu;

	if (vstream_id >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("Vstream id=%u out of range @ %s.\n",
			vstream_id, __func__);
		return -EINVAL;
	}

	mutex_lock(&xcu->vs_array_lock);
	xcu->vs_array[vstream_id] = NULL;
	mutex_unlock(&xcu->vs_array_lock);
	return 0;
}

static vstream_info_t *vstream_get(struct xsched_cu *xcu, uint32_t vstream_id)
{
	vstream_info_t *vstream = NULL;

	if (vstream_id >= MAX_VSTREAM_NUM) {
		XSCHED_ERR("Vstream id=%u out of range @ %s.\n",
			vstream_id, __func__);
		return NULL;
	}

	mutex_lock(&xcu->vs_array_lock);
	vstream = xcu->vs_array[vstream_id];
	mutex_unlock(&xcu->vs_array_lock);

	return vstream;
}

static vstream_info_t *
vstream_get_by_user_stream_id(struct xsched_cu *xcu, uint32_t user_stream_id)
{
	int id;
	static vstream_info_t *ret;

	mutex_lock(&xcu->vs_array_lock);
	for (id = 0; id < MAX_VSTREAM_NUM; id++) {
		if (xcu->vs_array[id] != NULL &&
			xcu->vs_array[id]->user_stream_id == user_stream_id) {
			ret = xcu->vs_array[id];
			break;
		}
	}
	mutex_unlock(&xcu->vs_array_lock);
	return ret;
}

static int sqcq_alloc(struct vstream_args *arg)
{
	vstream_alloc_args_t *va_args = &arg->va_args;
	struct xsched_context *ctx = NULL;
	struct xcu_op_handler_params params;
	uint32_t logic_cq_id = 0;
	vstream_info_t *vstream;
	int ret = 0;
	uint32_t tgid = 0;
	uint32_t cq_id = 0;
	uint32_t sq_id = 0;

	vstream = vstream_create(arg);
	if (!vstream)
		return -ENOSPC;

	vstream->fd = arg->fd;
	vstream->task_type = arg->task_type;

	ret = vstream_bind_to_xcu(vstream);
	if (ret) {
		ret = -EINVAL;
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
	ret = xcu_alloc(&params);
	if (ret) {
		XSCHED_ERR("Fail to allocate SQ/CQ memory to a vstream.\n");
		goto out_err_vstream_free;
	}

	vstream->drv_ctx = params.param_5;
	vstream->id = sq_id;
	vstream->vcq_id = cq_id;
	vstream->logic_vcq_id = logic_cq_id;
	vstream->user_stream_id = va_args->user_stream_id;
	vstream->tgid = tgid;
	vstream->sqcq_type = va_args->type;

	ret = vstream_bind_to_ctx(vstream);
	if (ret)
		goto out_err_vstream_free;

	ctx = vstream->ctx;
	ret = vstream_file_create(vstream);
	if (ret < 0) {
		XSCHED_ERR("Fail to alloc anon inode for vstream %u @ %s\n",
			vstream->id, __func__);
		goto out_err_vstream_free;
	}
	vstream->inode_fd = ret;

	/* Add new vstream to array after allocating inode */
	ret = vstream_add(vstream, vstream->id);
	if (ret)
		goto out_err_vstream_free;

	return 0;

out_err_vstream_free:
	kfree(vstream);
	return ret;
}

static int logic_cq_alloc(struct vstream_args *arg)
{
	int err = 0;
	struct xcu_op_handler_params params;
	vstream_info_t *vstream = NULL;
	vstream_alloc_args_t *logic_cq_alloc_para = &arg->va_args;
	struct xsched_cu *xcu_found = NULL;
	uint32_t logic_cq_id = 0;
	uint32_t type = XCU_TYPE_XPU;

	xcu_found = xcu_find(&type, arg->dev_id, arg->channel_id);
	if (!xcu_found)
		return -EINVAL;

	vstream = vstream_get_by_user_stream_id(xcu_found,
		logic_cq_alloc_para->user_stream_id);
	if (vstream)
		xcu_found = vstream->xcu;
	params.group = xcu_found->group;
	params.fd = arg->fd;
	params.payload = arg->payload;
	params.param_1 = &logic_cq_id;
	err = xcu_logic_alloc(&params);
	if (err) {
		XSCHED_ERR("Fail to alloc logic CQ memory to a vstream.\n");
		return err;
	}
	if (vstream)
		vstream->logic_vcq_id = logic_cq_id;

	return 0;
}

int vstream_alloc(struct vstream_args *arg)
{
	vstream_alloc_args_t *va_args = &arg->va_args;
	int ret;

	if (!va_args->type)
		ret = sqcq_alloc(arg);
	else
		ret = logic_cq_alloc(arg);

	return ret;
}

int vstream_free(struct vstream_args *arg)
{
	struct file *vs_file;
	struct xcu_op_handler_params params;
	struct xsched_cu *xcu_found;
	uint32_t vstream_id = arg->sq_id;
	uint32_t type = XCU_TYPE_XPU;
	vstream_info_t *vstream = NULL;
	int err = 0;

	xcu_found = xcu_find(&type, arg->dev_id, arg->channel_id);
	if (!xcu_found)
		return -EINVAL;

	vstream = vstream_get(xcu_found, vstream_id);
	if (!vstream) {
		XSCHED_ERR("Fail to free NULL vstream, vstream id=%u\n", vstream_id);
		return -EINVAL;
	}

	params.group = vstream->xcu->group;
	params.fd = arg->fd;
	params.payload = arg->payload;

	vs_file = vstream_file_get(vstream->inode_fd);
	if (vs_file) {
		vs_file->private_data = NULL;
		vstream_file_put(vs_file);
	}

	/* After vstream_get(), destroying the vstream may not fail */
	vstream_destroy(vstream);
	err = xcu_finish(&params);
	if (err)
		XSCHED_ERR("Fail to free vstream sqId=%u, cqId=%u.\n",
			arg->sq_id, arg->cq_id);

	return err;
}

int vstream_kick(struct vstream_args *arg)
{
	vstream_info_t *vstream;
	struct xsched_cu *xcu = NULL;
	struct xsched_entity *xse;
	int err = 0;
	uint32_t vstream_id = arg->sq_id;
	uint32_t type = XCU_TYPE_XPU;

	xcu = xcu_find(&type, arg->dev_id, arg->channel_id);
	if (!xcu)
		return -EINVAL;

	/* Get vstream. */
	vstream = vstream_get(xcu, vstream_id);
	if (!vstream || !vstream->ctx) {
		XSCHED_ERR("Vstream NULL or doesn't have a context. "
			"vstream_id=%u, dev_id=%u\n", vstream_id, arg->dev_id);
		return -EINVAL;
	}

	xse = &vstream->ctx->xse;
	XSCHED_DEBUG("New kick on xse %d @ %s\n", xse->tgid, __func__);

	do {
		mutex_lock(&xcu->xcu_lock);
		spin_lock(&vstream->stream_lock);

		/* Adding kick metadata. */
		err = xsched_vsm_add_tail(vstream, arg);
		if (err == -EBUSY) {
			spin_unlock(&vstream->stream_lock);
			mutex_unlock(&xcu->xcu_lock);

			/* Retry after a while */
			usleep_range(100, 200);
			continue;
		}

		/* Don't forget to unlock */
		if (err) {
			XSCHED_ERR("Fail to add kick metadata to vs %u @ %s\n",
				vstream->id, __func__);
			break;
		}

		enqueue_ctx(xse, xcu);
		/* Increasing a total amount of kicks on an CU to which this
		 * context is attached to based on sched_class.
		 */
		xsched_inc_pending_kicks_xse(&vstream->ctx->xse);
	} while (err == -EBUSY);

	spin_unlock(&vstream->stream_lock);
	mutex_unlock(&xcu->xcu_lock);
	if (!err)
		wake_up_interruptible(&xcu->wq_xcu_idle);

	return err;
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
		XSCHED_ERR("copy_from_user failed\n");
		return -EFAULT;
	}

	res = vstream_command_table[cmd](&vstream_arg);
	if (copy_to_user(arg, &vstream_arg, sizeof(struct vstream_args))) {
		XSCHED_ERR("copy_to_user failed\n");
		return -EFAULT;
	}

	XSCHED_DEBUG("vstream_manage: cmd %d\n", cmd);
	return res;
}
#else
SYSCALL_DEFINE2(vstream_manage, struct vstream_args __user *, arg, int, cmd)
{
	return 0;
}
#endif
