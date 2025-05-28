// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level idle sequences
 */

#include <linux/cpu.h>
#include <linux/irqflags.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>

/*
 *	cpu_do_idle()
 *
 *	Idle the processor (wait for interrupt).
 *
 *	If the CPU supports priority masking we must do additional work to
 *	ensure that interrupts are not masked at the PMR (because the core will
 *	not wake up if we block the wake up signal in the interrupt controller).
 */
void noinstr cpu_do_idle(void)
{
	struct arm_cpuidle_irq_context context;

	arm_cpuidle_save_irq_context(&context);

	dsb(sy);
	wfi();

	arm_cpuidle_restore_irq_context(&context);
}

/*
 * This is our default idle handler.
 */
void noinstr arch_cpu_idle(void)
{
	/*
	 * This should do all the clock switching and wait for interrupt
	 * tricks
	 */
	cpu_do_idle();
}
EXPORT_SYMBOL_GPL(arch_cpu_idle);

#ifdef CONFIG_ACTLR_XCALL_XINT
DEFINE_PER_CPU_ALIGNED(struct arm_cpuidle_xcall_xint_context, contexts);

void arch_cpu_idle_enter(void)
{
	struct arm_cpuidle_xcall_xint_context *context;

	if (!system_uses_xcall_xint())
		return;

	context = &get_cpu_var(contexts);
	arm_cpuidle_save_xcall_xint_context(context);
	put_cpu_var(contexts);
}

void arch_cpu_idle_exit(void)
{
	struct arm_cpuidle_xcall_xint_context *context;

	if (!system_uses_xcall_xint())
		return;

	context = &get_cpu_var(contexts);
	arm_cpuidle_restore_xcall_xint_context(context);
	put_cpu_var(contexts);
}
#else
void arch_cpu_idle_enter(void) {}
void arch_cpu_idle_exit(void) {}
#endif
