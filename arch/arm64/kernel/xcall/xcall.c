// SPDX-License-Identifier: GPL-2.0-only
/*
 * xcall related code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */

#include <linux/bitmap.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <asm/xcall.h>

static inline int sw_xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	p->xinfo = kzalloc(sizeof(struct xcall_info), GFP_KERNEL);
	if (!p->xinfo)
		return -ENOMEM;

	if (orig->xinfo) {
		bitmap_copy(TASK_XINFO(p)->xcall_enable, TASK_XINFO(orig)->xcall_enable,
			    __NR_syscalls);
	}

	return 0;
}

int xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	if (static_branch_unlikely(&xcall_enable))
		return sw_xcall_init_task(p, orig);

	return 0;
}

void xcall_task_free(struct task_struct *p)
{
	if (static_branch_unlikely(&xcall_enable))
		kfree(p->xinfo);
}

#define __NR_fast_syscalls	512
static u8 fast_syscall_enabled[__NR_fast_syscalls + 1] = {
	[0 ... __NR_fast_syscalls] = 0,
};
asmlinkage DEFINE_PER_CPU(u8*, __cpu_fast_syscall) = fast_syscall_enabled;
