/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 * Description: Kernel IPC header
 * Author: yangyun
 * Create: 2024-05-31
 */
#ifndef __KERNEL_IPC_H_
#define __KERNEL_IPC_H_

struct kernel_ipc_bind_info {
	//unsigned int session_id;
	unsigned int data_size;

	struct task_struct *client_task;
	struct task_struct *server_task;
	// struct task_struct *server_task_get;

	bool is_calling;
	bool client_need_exit;
	bool server_need_exit;

	atomic_t nr_call;

	//struct kref ref;
	spinlock_t lock;
	struct list_head node;
};

void kernel_ipc_wakeup_server_task(struct kernel_ipc_bind_info *bind_info);

void *kernel_ipc_bind(struct task_struct *server_task);

void kernel_ipc_unbind(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *server_task);

ssize_t kernel_ipc_do_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk);

long kernel_ipc_ret_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk);

long kernel_ipc_wait_call(struct kernel_ipc_bind_info *bind_info,
		struct task_struct *tsk);

void kernel_ipc_release(struct kernel_ipc_bind_info *bind_info);

#endif
