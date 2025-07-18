// SPDX-License-Identifier: GPL-2.0-only
/*
 * xcall related code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */

#include <linux/bitmap.h>
#include <linux/percpu.h>
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

#ifdef CONFIG_ACTLR_XCALL_XINT
static inline int hw_xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	int i;

	p->xinfo = kzalloc(sizeof(struct hw_xcall_info), GFP_KERNEL);
	if (!p->xinfo)
		return -ENOMEM;

	for (i = 0; i < __NR_syscalls; i++)
		TASK_HW_XINFO(p)->xcall_entry[i] = no_xcall_entry;

	if (orig->xinfo) {
		memcpy(p->xinfo, orig->xinfo, XCALL_ENTRY_SIZE);
		TASK_HW_XINFO(p)->xcall_scno_enabled = TASK_HW_XINFO(orig)->xcall_scno_enabled;
	}

	return 0;
}
#endif

int xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	if (!is_hw_xcall_support && !is_xcall_support)
		return 0;

#ifdef CONFIG_ACTLR_XCALL_XINT
	if (is_hw_xcall_support)
		return hw_xcall_init_task(p, orig);
#endif
	return sw_xcall_init_task(p, orig);
}

void xcall_task_free(struct task_struct *p)
{
	if (!is_hw_xcall_support && !is_xcall_support)
		return;

	kfree(p->xinfo);
}
