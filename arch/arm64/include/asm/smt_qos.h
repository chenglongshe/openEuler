/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SMT_QOS_H
#define __ASM_SMT_QOS_H

#include <linux/perf/arm_pmuv3.h>
#include <asm/sysreg.h>

#define INST_RETIRED_COUNTER		0
#define CYCLE_COUNTER			1

DECLARE_PER_CPU(bool, pmu_enable);

extern unsigned int sysctl_delay_cycles;
extern unsigned int sysctl_sample_interval_inst;
extern unsigned int sysctl_sample_interval_cycles;

// Enable Performance Monitors
static inline void pmu_start(void)
{
	u64 reg_val;

	reg_val = read_sysreg(pmcr_el0);
	reg_val |= ARMV8_PMU_PMCR_E;		// Enable the PMU counter
	write_sysreg(reg_val, pmcr_el0);
	isb();
}

// Disable the PMU entirely
static inline void pmu_stop(void)
{
	u64 reg_val;

	reg_val = read_sysreg(pmcr_el0);
	reg_val &= ~ARMV8_PMU_PMCR_E; // Clear bit 0 (E) to Disable the PMU
	write_sysreg(reg_val, pmcr_el0);
	isb();
}

static inline void write_pmevtypern_el0(int n, u64 val)
{
	u64 and = ARMV8_PMU_INCLUDE_EL2 | ARMV8_PMU_EXCLUDE_EL1;

	switch(n) {
		case 0:
			write_sysreg(val | and, pmevtyper0_el0);
			break;
		case 1:
			write_sysreg(val | and, pmevtyper1_el0);
			break;
		default:
			break;
	}
}

static inline void write_pmevcntrn_el0(int n, u64 val)
{
	val |= GENMASK_ULL(63, 32);

	switch(n) {
		case 0:
			write_sysreg(val, pmevcntr0_el0);
			break;
		case 1:
			write_sysreg(val, pmevcntr1_el0);
			break;
		default:
			break;
	}
}

static inline void write_pmintenset_el1(unsigned int counter)
{
	write_sysreg(BIT(counter), pmintenset_el1);
}

static inline void write_pmcntenset_el0(unsigned int counter)
{
	write_sysreg(BIT(counter), pmcntenset_el0);
}

static inline void write_pmcntenclr_el0(unsigned int counter)
{
	write_sysreg(BIT(counter), pmcntenclr_el0);
}

static inline void write_pmintenclr_el1(unsigned int counter)
{
	write_sysreg(BIT(counter), pmintenclr_el1);
}

static inline void write_pmovsclr_el0(unsigned int counter)
{
	write_sysreg(BIT(counter), pmovsclr_el0);
}

void setup_pmu_counter(void *info);
void stop_pmu_counter(void *info);
irqreturn_t my_pmu_irq_handler(int irq, void *dev_id);

#endif /* __ASM_SMT_QOS_H */
