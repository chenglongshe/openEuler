/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_XCALL_H
#define __ASM_XCALL_H

#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/types.h>

#include <asm/actlr.h>

extern bool is_xcall_support;
extern bool is_hw_xcall_support;

struct xcall_info {
	/* Must be first! */
	DECLARE_BITMAP(xcall_enable, __NR_syscalls);
};

#define TASK_XINFO(p)	((struct xcall_info *)p->xinfo)

int xcall_init_task(struct task_struct *p, struct task_struct *orig);
void xcall_task_free(struct task_struct *p);

#ifdef CONFIG_ACTLR_XCALL_XINT
struct hw_xcall_info {
	/* Must be first! */
	void *xcall_entry[__NR_syscalls];
	bool xcall_scno_enabled;
};

#define TASK_HW_XINFO(p)	((struct hw_xcall_info *)p->xinfo)
#define XCALL_ENTRY_SIZE	(sizeof(unsigned long) * __NR_syscalls)

extern void xcall_entry(void);
extern void no_xcall_entry(void);

static inline bool is_xcall_entry(struct hw_xcall_info *xinfo, unsigned int sc_no)
{
	return xinfo->xcall_entry[sc_no] == xcall_entry;
}

static inline int has_xcall_scno_enabled(struct hw_xcall_info *xinfo)
{
	unsigned int i;

	for (i = 0; i < __NR_syscalls; i++) {
		if (is_xcall_entry(xinfo, i))
			return true;
	}

	return false;
}

static inline int set_xcall_entry(struct hw_xcall_info *xinfo, unsigned int sc_no)
{
	xinfo->xcall_entry[sc_no] = xcall_entry;
	xinfo->xcall_scno_enabled = true;

	return 0;
}

static inline int set_no_xcall_entry(struct hw_xcall_info *xinfo, unsigned int sc_no)
{
	xinfo->xcall_entry[sc_no] = no_xcall_entry;
	if (!has_xcall_scno_enabled(xinfo))
		xinfo->xcall_scno_enabled = false;

	return 0;
}

static inline void cpu_enable_arch_xcall(void)
{
	u64 el = read_sysreg(CurrentEL);

	if (el == CurrentEL_EL2)
		write_sysreg(read_sysreg(actlr_el2) | ACTLR_ELx_XCALL, actlr_el2);
	else
		write_sysreg(read_sysreg(actlr_el1) | ACTLR_ELx_XCALL, actlr_el1);
}

static inline void cpu_switch_xcall_entry(struct task_struct *tsk)
{
	if (!is_hw_xcall_support || !tsk->xinfo)
		return;

	if (TASK_HW_XINFO(tsk)->xcall_scno_enabled)
		cpu_enable_arch_xcall();
}
#endif /* CONFIG_ACTLR_XCALL_XINT */

#endif /*__ASM_XCALL_H*/
