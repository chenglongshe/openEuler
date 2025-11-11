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
unsigned int sysctl_sample_interval_inst = 100000000;
unsigned int sysctl_sample_interval_cycles = 100000000;

SYSCALL_DEFINE0(vdso_wfxt_return)
{
	struct pt_regs *regs = current_pt_regs();

	regs->pc = task_thread_info(current)->qos_context1;
	regs->regs[8] = task_thread_info(current)->qos_context2;
	trace_printk("restored pc: 0x%llx, restored x8: 0x%llx\n", regs->pc, regs->regs[8]);

	return regs->regs[0];
}

void setup_pmu_counter(void *info)
{
	if (unlikely(__this_cpu_read(pmu_enable)))
		return;

	trace_printk("Enable pmu on CPU %d.\n", smp_processor_id());

	pmu_start();

	if (sysctl_sample_interval_inst != 0) {
		write_pmevtypern_el0(INST_RETIRED_COUNTER, ARMV8_PMUV3_PERFCTR_INST_RETIRED);
		write_pmevcntrn_el0(INST_RETIRED_COUNTER, (0xffffffffUL - sysctl_sample_interval_inst));
		write_pmintenset_el1(INST_RETIRED_COUNTER);
		write_pmcntenset_el0(INST_RETIRED_COUNTER);
	}

	if (sysctl_sample_interval_cycles != 0) {
		write_pmevtypern_el0(CYCLE_COUNTER, ARMV8_PMUV3_PERFCTR_CPU_CYCLES);
		write_pmevcntrn_el0(CYCLE_COUNTER, (0xffffffffUL - sysctl_sample_interval_cycles));
		write_pmintenset_el1(CYCLE_COUNTER);
		write_pmcntenset_el0(CYCLE_COUNTER);
	}
	isb();

	__this_cpu_write(pmu_enable, true);
}

void stop_pmu_counter(void *info)
{
	if (likely(!__this_cpu_read(pmu_enable)))
		return;

	trace_printk("Disable pmu on cpu%d\n", smp_processor_id());

	if (sysctl_sample_interval_inst != 0) {
		write_pmcntenclr_el0(INST_RETIRED_COUNTER);
		write_pmintenclr_el1(INST_RETIRED_COUNTER);
		write_pmovsclr_el0(INST_RETIRED_COUNTER);
	}

	if (sysctl_sample_interval_cycles != 0) {
		write_pmcntenclr_el0(CYCLE_COUNTER);
		write_pmintenclr_el1(CYCLE_COUNTER);
		write_pmovsclr_el0(CYCLE_COUNTER);
	}
	isb();

	__this_cpu_write(pmu_enable, false);
}

irqreturn_t my_pmu_irq_handler(int irq, void *dev_id)
{
	u64 pmovsclr;

	pmovsclr = read_sysreg(pmovsclr_el0);
	write_sysreg(pmovsclr, pmovsclr_el0);

	// Check if our specific counter caused the interrupt
	if (!(pmovsclr & BIT(INST_RETIRED_COUNTER)) && !(pmovsclr & BIT(CYCLE_COUNTER)))
		return IRQ_NONE;

	pmu_stop();

	if (pmovsclr & BIT(INST_RETIRED_COUNTER))
		write_pmevcntrn_el0(INST_RETIRED_COUNTER, (0xffffffffUL - sysctl_sample_interval_inst));

	if (pmovsclr & BIT(CYCLE_COUNTER))
		write_pmevcntrn_el0(CYCLE_COUNTER, (0xffffffffUL - sysctl_sample_interval_cycles));

	pmu_start();

	return IRQ_HANDLED;
}
