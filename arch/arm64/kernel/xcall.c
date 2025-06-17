// SPDX-License-Identifier: GPL-2.0-only
/*
 * xcall related code
 *
 * Copyright (C) 2025 Huawei Ltd.
 */

#include <linux/bitmap.h>
#include <linux/slab.h>
#include <asm/xcall.h>

int xcall_init_task(struct task_struct *p, struct task_struct *orig)
{
	if (likely(!system_supports_xcall()))
		return 0;

	p->xinfo = kzalloc(sizeof(struct xcall_info), GFP_KERNEL);
	if (!p->xinfo)
		return -ENOMEM;

	if (orig->xinfo) {
		bitmap_copy(p->xinfo->xcall_enable, orig->xinfo->xcall_enable,
			    __NR_syscalls);
#ifdef CONFIG_XCALL_PREFETCH
		p->xinfo->prefetch = orig->xinfo->prefetch;
#endif
	}

	return 0;
}

void xcall_task_free(struct task_struct *p)
{
	if (likely(!system_supports_xcall()))
		return;

	kfree(p->xinfo);
}
