/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_XCALL_H
#define _LINUX_XCALL_H

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
	bool prefetch;
};
#endif
