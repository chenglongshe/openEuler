/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Low-level exception handling code
 *
 * Copyright (C) 2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

	.macro hw_xcall_save_base_regs
	stp	x0, x1, [sp, #16 * 0]			// save x0~x29
	stp	x2, x3, [sp, #16 * 1]
	stp	x4, x5, [sp, #16 * 2]
	stp	x6, x7, [sp, #16 * 3]
	stp	x8, x9, [sp, #16 * 4]
	stp	x10, x11, [sp, #16 * 5]
	stp	x12, x13, [sp, #16 * 6]
	stp	x14, x15, [sp, #16 * 7]
	stp	x16, x17, [sp, #16 * 8]
	stp	x18, x19, [sp, #16 * 9]
	stp	x20, x21, [sp, #16 * 10]
	stp	x22, x23, [sp, #16 * 11]
	stp	x24, x25, [sp, #16 * 12]
	stp	x26, x27, [sp, #16 * 13]
	stp	x28, x29, [sp, #16 * 14]

	clear_gp_regs					// clear x0~x29
	mrs	x21, sp_el0
	ldr_this_cpu	tsk, __entry_task, x20
	msr	sp_el0, tsk

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * Enable IA for in-kernel PAC if the task had it disabled. Although
	 * this could be implemented with an unconditional MRS which would avoid
	 * a load, this was measured to be slower on Cortex-A75 and Cortex-A76.
	 *
	 * Install the kernel IA key only if IA was enabled in the task. If IA
	 * was disabled on kernel exit then we would have left the kernel IA
	 * installed so there is no need to install it again.
	 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]

	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_kernel_nosync tsk, x20, x22, x23
	b	2f
1:
	mrs	x0, sctlr_el1
	orr	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

alternative_if ARM64_HAS_ADDRESS_AUTH
         isb
alternative_else_nop_endif

	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]  			// save LR,USER SP

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	add	x29, sp, #S_STACKFRAME			// calc FP

	stp	x22, x23, [sp, #S_PC]			// save ELR,SPSR

	mov	w21, #NO_SYSCALL
	str	w21, [sp, #S_SYSCALLNO]


#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_save\@
alternative_else_nop_endif

	mrs_s	x20, SYS_ICC_PMR_EL1
	str	x20, [sp, #S_PMR_SAVE]
	mov	x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s	SYS_ICC_PMR_EL1, x20

.Lskip_pmr_save\@:
#endif
	.endm /* .macro hw_xcall_save_base_regs */

	.macro hw_xcall_restore_base_regs
#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore\@
alternative_else_nop_endif

	ldr	x20, [sp, #S_PMR_SAVE]
	msr_s	SYS_ICC_PMR_EL1, x20

	/* Ensure priority change is seen by redistributor */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore\@:
#endif

	ldp	x21, x22, [sp, #S_PC]
	ldr	x23, [sp, #S_SP]
	msr	sp_el0, x23			// restore USER SP

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
	ldr	x0, [tsk, THREAD_SCTLR_USER]
	tbz	x0, SCTLR_ELx_ENIA_SHIFT, 1f
	__ptrauth_keys_install_user tsk, x0, x1, x2
	b	2f
1:
	mrs	x0, sctlr_el1
	bic	x0, x0, SCTLR_ELx_ENIA
	msr	sctlr_el1, x0
2:
alternative_else_nop_endif
#endif

	msr	elr_el1, x21			// restore ELR
	msr	spsr_el1, x22			// restore SPSR

	ldp	x0, x1, [sp, #16 * 0]		// restore x0~x29
	ldp	x2, x3, [sp, #16 * 1]
	ldp	x4, x5, [sp, #16 * 2]
	ldp	x6, x7, [sp, #16 * 3]
	ldp	x8, x9, [sp, #16 * 4]
	ldp	x10, x11, [sp, #16 * 5]
	ldp	x12, x13, [sp, #16 * 6]
	ldp	x14, x15, [sp, #16 * 7]
	ldp	x16, x17, [sp, #16 * 8]
	ldp	x18, x19, [sp, #16 * 9]
	ldp	x20, x21, [sp, #16 * 10]
	ldp	x22, x23, [sp, #16 * 11]
	ldp	x24, x25, [sp, #16 * 12]
	ldp	x26, x27, [sp, #16 * 13]
	ldp	x28, x29, [sp, #16 * 14]

	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE		// restore sp
	eret
	sb
	.endm /* .macro hw_xcall_restore_base_regs */

SYM_CODE_START_LOCAL(el0t_64_svc_entry)
alternative_if_not ARM64_HAS_HW_XCALL_XINT
	/* Hijack SVC to dynamically load syscalls via '/proc/xcall/comm' */
	ldr	x20, [sp, #S_SYSCALLNO]			// ESR.bits[15,0]
	cmp	x20, 0xfff
	b.ge	el0t_64_xcall_entry
alternative_else_nop_endif

	/* Hijack SVC to low overhead syscalls via '/prox/[pid]/xcall' */
	cmp 	x8, __NR_syscalls
	b.ge	.slow_syscall
	ldr_this_cpu	x21, __xcall_info, x20
	ldrb	w20, [x21, x8]
	cmp	x20, 0
	bne	el0t_fast_syscall

.slow_syscall:
	ldp	x20, x21, [sp, #16 * 10]
	kernel_entry 0, 64
	mov	x0, sp
	bl	el0_slow_syscall
	b	ret_to_user
SYM_INNER_LABEL(el0t_64_xcall_entry, SYM_L_GLOBAL)
	lsr	x20, x20, #12
	adr	x21, .xcall_func_table
	ldr	w20, [x21, x20, lsl #2]
	add	x20, x20, x21
	br	x20
	/* ISS==0F~FF: Entry to optimized and customized syscalls
	 */
.xcall_func_table:
	.rept 16
	.word el0t_xcall_syscall - .xcall_func_table
	.endr
SYM_CODE_END(el0t_64_svc_entry)

SYM_CODE_START_LOCAL(el0t_xcall_syscall)
	ldp	x20, x21, [sp, #16 * 10]
	hw_xcall_save_base_regs
	mov	x0, sp
	bl	el0_xcall_syscall
	hw_xcall_restore_base_regs
SYM_CODE_END(el0t_xcall_syscall)

SYM_CODE_START_LOCAL(el0t_fast_syscall)
	ldp	x20, x21, [sp, #16 * 10]
	hw_xcall_save_base_regs
	mov	x0, sp
	bl	el0_fast_syscall
	hw_xcall_restore_base_regs
SYM_CODE_END(el0t_fast_syscall)

SYM_CODE_START_LOCAL(el0t_64_sync_ventry)
	ldp	x20, x21, [sp, #16 * 10]
	add	sp, sp, #PT_REGS_SIZE
	kernel_ventry 0, t, 64, sync
SYM_CODE_END(el0t_64_sync_ventry)

SYM_CODE_START_LOCAL(el0t_64_sync_table)
	// 0 - (ESR_ELx_EC_SVC64 - 1)
	.rept ESR_ELx_EC_SVC64
	.word el0t_64_sync_table - el0t_64_sync_ventry
	.endr
	// ESR_ELx_EC_SVC64
	.word el0t_64_sync_table - el0t_64_svc_entry
	// (ESR_ELx_EC_SVC64 + 1) - ESR_ELx_EC_MAX
	.rept ESR_ELx_EC_MAX - ESR_ELx_EC_SVC64
	.word el0t_64_sync_table - el0t_64_sync_ventry
	.endr
SYM_CODE_END(el0t_64_sync_table)

/***********************************************
 *                                             *
 *   Xcall exception entry code for 920G CPU   *
 *                                             *
 ***********************************************/
#ifdef CONFIG_ACTLR_XCALL_XINT
	.macro xcall_ventry
	.align 7
.Lventry_start\@:
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	b	.Lskip_tramp_vectors_cleanup\@
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
.Lskip_tramp_vectors_cleanup\@:
	sub	sp, sp, #PT_REGS_SIZE
	stp	x20, x21, [sp, #16 * 10]
	/* Decode ESR.ICC bits[15, 0] for use later */
	mrs	x21, esr_el1
	uxth	w20, w21
	b	el0t_64_xcall_entry
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm
#endif /* CONFIG_ACTLR_XCALL_XINT */

/****************************************************************
 *                                                              *
 *      Sync exception entry code for early CPUs before 920G    *
 *                                                              *
 ****************************************************************/
	.macro sync_ventry
	.align 7
.Lventry_start\@:
	/*
	 * This must be the first instruction of the EL0 vector entries. It is
	 * skipped by the trampoline vectors, to trigger the cleanup.
	 */
	b	.Lskip_tramp_vectors_cleanup\@
	mrs	x30, tpidrro_el0
	msr	tpidrro_el0, xzr
.Lskip_tramp_vectors_cleanup\@:
	sub	sp, sp, #PT_REGS_SIZE

alternative_if_not ARM64_HAS_XCALL
#ifdef CONFIG_VMAP_STACK
	/*
	 * Test whether the SP has overflowed, without corrupting a GPR.
	 * Task and IRQ stacks are aligned so that SP & (1 << THREAD_SHIFT)
	 * should always be zero.
	 */
	add	sp, sp, x0			// sp' = sp + x0
	sub	x0, sp, x0			// x0' = sp' - x0 = (sp + x0) - x0 = sp
	tbnz	x0, #THREAD_SHIFT, 0f
	sub	x0, sp, x0			// x0'' = sp' - x0' = (sp + x0) - sp = x0
	sub	sp, sp, x0			// sp'' = sp' - x0 = (sp + x0) - x0 = sp
	b	el0t_64_sync

0:
	/*
	 * Either we've just detected an overflow, or we've taken an exception
	 * while on the overflow stack. Either way, we won't return to
	 * userspace, and can clobber EL0 registers to free up GPRs.
	 */

	/* Stash the original SP (minus PT_REGS_SIZE) in tpidr_el0. */
	msr	tpidr_el0, x0

	/* Recover the original x0 value and stash it in tpidrro_el0 */
	sub	x0, sp, x0
	msr	tpidrro_el0, x0

	/* Switch to the overflow stack */
	adr_this_cpu sp, overflow_stack + OVERFLOW_STACK_SIZE, x0

	/*
	 * Check whether we were already on the overflow stack. This may happen
	 * after panic() re-enables interrupts.
	 */
	mrs	x0, tpidr_el0			// sp of interrupted context
	sub	x0, sp, x0			// delta with top of overflow stack
	tst	x0, #~(OVERFLOW_STACK_SIZE - 1)	// within range?
	b.ne	__bad_stack			// no? -> bad stack pointer

	/* We were already on the overflow stack. Restore sp/x0 and carry on. */
	sub	sp, sp, x0
	mrs	x0, tpidrro_el0
#endif
	b	el0t_64_sync
alternative_else
	/* Save ESR and ICC.bits[15,0] for use later */
	stp	x20, x21, [sp, #16 * 10]
	mrs	x20, esr_el1
	uxth	w21, w20
	stp 	x20, x21, [sp, #(S_SYSCALLNO - 8)]
	/* Using jump table for different exception causes */
	lsr	w21, w20, #ESR_ELx_EC_SHIFT
	adr	x20, el0t_64_sync_table
	ldr	w21, [x20, x21, lsl #2]
	sub	x20, x20, x21
	br	x20
alternative_endif
.org .Lventry_start\@ + 128	// Did we overflow the ventry slot?
	.endm
