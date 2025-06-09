/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XCALL_H
#define _LINUX_XCALL_H

#include <linux/sysctl.h>

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
	DECLARE_BITMAP(xcall_select, __NR_syscalls);
};

bool fast_syscall_enabled(void);

static inline bool is_epoll_pwait_selected(const struct task_struct *p)
{
	return p->xinfo && test_bit(__NR_epoll_pwait, p->xinfo->xcall_select);
}
#endif
