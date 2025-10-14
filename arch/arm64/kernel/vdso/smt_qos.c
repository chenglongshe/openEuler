// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 userspace implementations of smt qos trampoline
 *
 */
#include <vdso/helpers.h>
#include <linux/errno.h>

#define wfit(val) do {							\
	asm volatile("msr s0_3_c1_c0_1, %0" : : "r" (val) : "memory")	\
} while (0)

#define wfet(val) do {							\
	asm volatile("msr s0_3_c1_c0_0, %0" : : "r" (val) : "memory")	\
} while (0)

static __always_inline struct qos_data *__arch_get_qos_vdso_data(void)
{
	return &_qos_data;
}

static unsigned long long get_cycles(void)
{
	unsigned long long val;

	asm volatile("mrs %0, cntvct_el0" : "=r" (val));

	return val;
}

static void delay(unsigned long cycles)
{
	unsigned long long start = get_cycles();
	unsigned long long end = start + cycles;

	wfit(end);
	while ((get_cycles() - start) < cycles)
		wfet(end);
}


void do_smt_qos_trampoline(void)
{
	struct qos_data *qos_data = __arch_get_qos_vdso_data();

	delay(qos_data->delay_cycles);
}
