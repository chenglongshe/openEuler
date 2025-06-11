/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XCALL_H
#define _LINUX_XCALL_H

#include <linux/types.h>
#include <linux/sched.h>

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
};

int xcall_init_task(struct task_struct *p, struct task_struct *orig);
void xcall_task_free(struct task_struct *p);
#endif
