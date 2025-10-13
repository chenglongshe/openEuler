// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt)	"SMT QoS: " fmt

#include <linux/sizes.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/compiler_types.h>

#include <asm/smt_qos.h>

unsigned int sysctl_delay_cycles = 10000000;

SYSCALL_DEFINE0(vdso_wfxt_return)
{
	struct pt_regs *regs = current_pt_regs();

	regs->pc = task_thread_info(current)->qos_context1;
	regs->regs[8] = task_thread_info(current)->qos_context2;
	trace_printk("restored pc: 0x%llx, restored x8: 0x%llx\n", regs->pc, regs->regs[8]);

	return regs->regs[0];
}
