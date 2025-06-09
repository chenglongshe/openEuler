/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XCALL_H
#define _LINUX_XCALL_H

#include <linux/sysctl.h>

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
	DECLARE_BITMAP(xcall_select, __NR_syscalls);
};
#endif
