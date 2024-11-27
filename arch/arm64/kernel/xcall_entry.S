/* SPDX-License-Identifier: GPL-2.0-only */
/* Low-level xcall handling code, based on arch/arm64/kernel/entry.S */

	.macro xcall_entry

	alternative_insn nop, SET_PSTATE_DIT(1), ARM64_HAS_DIT

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

	mrs	x22, elr_el1
	mrs	x23, spsr_el1
	stp	lr, x21, [sp, #S_LR]  			// save LR,USER SP

	stp	xzr, xzr, [sp, #S_STACKFRAME]
	add	x29, sp, #S_STACKFRAME			// calc FP

	stp	x22, x23, [sp, #S_PC]			// save ELR,SPSR

	/* Not in a syscall by default (el0_svc overwrites for real syscall) */
	mov w21, #NO_SYSCALL
	str w21, [sp, #S_SYSCALLNO]

#ifdef CONFIG_ARM64_PSEUDO_NMI
	alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
		b .Lskip_pmr_save\@
	alternative_else_nop_endif

	mrs_s x20, SYS_ICC_PMR_EL1
	str x20, [sp, #S_PMR_SAVE]
	mov x20, #GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET
	msr_s SYS_ICC_PMR_EL1, x20

	.Lskip_pmr_save\@:
#endif
	.endm

SYM_CODE_START_LOCAL(ret_to_user_xcall)
#ifdef CONFIG_ARM64_PSEUDO_NMI
alternative_if_not ARM64_HAS_GIC_PRIO_MASKING
	b	.Lskip_pmr_restore
alternative_else_nop_endif

	ldr	x20, [sp, #S_PMR_SAVE]
	msr_s	SYS_ICC_PMR_EL1, x20

	/* Ensure priority change is seen by redistributor */
alternative_if_not ARM64_HAS_GIC_PRIO_RELAXED_SYNC
	dsb	sy
alternative_else_nop_endif

.Lskip_pmr_restore:
#endif
	ldp x21, x22, [sp, #S_PC]  // load ELR, SPSR

	ldr x23, [sp, #S_SP]  // load return stack pointer
	msr sp_el0, x23

#ifdef CONFIG_ARM64_PTR_AUTH
alternative_if ARM64_HAS_ADDRESS_AUTH
	/*
	 * IA was enabled for in-kernel PAC. Disable it now if needed, or
	 * alternatively install the user's IA. All other per-task keys and
	 * SCTLR bits were updated on task switch.
	 *
	 * No kernel C function calls after this.
	 */
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

	msr elr_el1, x21   // set up the return data
	msr spsr_el1, x22
	ldp x0, x1, [sp, #16 * 0]
	ldp x2, x3, [sp, #16 * 1]
	ldp x4, x5, [sp, #16 * 2]
	ldp x6, x7, [sp, #16 * 3]
	ldp x8, x9, [sp, #16 * 4]
	ldp x10, x11, [sp, #16 * 5]
	ldp x12, x13, [sp, #16 * 6]
	ldp x14, x15, [sp, #16 * 7]
	ldp x16, x17, [sp, #16 * 8]
	ldp x18, x19, [sp, #16 * 9]
	ldp x20, x21, [sp, #16 * 10]
	ldp x22, x23, [sp, #16 * 11]
	ldp x24, x25, [sp, #16 * 12]
	ldp x26, x27, [sp, #16 * 13]
	ldp x28, x29, [sp, #16 * 14]

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	alternative_insn "b .L_skip_tramp_exit_xcall", nop, ARM64_UNMAP_KERNEL_AT_EL0

	msr far_el1, x29

	ldr_this_cpu x30, this_cpu_vector, x29
	tramp_alias x29, tramp_exit
	msr  vbar_el1, x30  // install vector table
	ldr  lr, [sp, #S_LR]  // restore x30
	add  sp, sp, #PT_REGS_SIZE // restore sp
	br  x29

.L_skip_tramp_exit_xcall:
#endif

	ldr lr, [sp, #S_LR]
	add sp, sp, #PT_REGS_SIZE  // restore sp

	/* This must be after the last explicit memory access */
	alternative_if ARM64_WORKAROUND_SPECULATIVE_UNPRIV_LOAD
		tlbi vale1, xzr
		dsb nsh
	alternative_else_nop_endif
	eret
SYM_CODE_END(ret_to_user_xcall)

SYM_CODE_START_LOCAL(xcall_handler)
	xcall_entry
	mov	x0, sp
	bl	el0t_64_xcall_handler
	b	ret_to_user_xcall
SYM_CODE_END(xcall_handler)

	.macro check_xcall_pre_kernel_entry
	stp	x20, x21, [sp, #0]
	/* check ESR_ELx_EC_SVC64 */
	mrs	x20, esr_el1
	lsr	w20, w20, #ESR_ELx_EC_SHIFT
	cmp	x20, #ESR_ELx_EC_SVC64
	bne	.Lskip_xcall\@
	/* x8 >= __NR_syscalls */
	cmp	x8, __NR_syscalls
	bhs	.Lskip_xcall\@
	str	x8, [sp, #16]
	/* x21 = task_struct->xcall_enable */
	ldr_this_cpu	x20, __entry_task, x21
	ldr	x21, [x20, #TSK_XCALL]
	/* x20 = sc_no / 8 */
	lsr	x20, x8, 3
	ldr	x21, [x21, x20]
	/* x8 = sc_no % 8 */
	and	x8, x8, 7
	mov	x20, 1
	lsl	x20, x20, x8
	and	x21, x21, x20
	cmp	x21, 0
	ldr	x8, [sp, #16]
	beq	.Lskip_xcall\@
	ldp	x20, x21, [sp, #0]
	b	xcall_handler
.Lskip_xcall\@:
	ldp	x20, x21, [sp, #0]
	.endm
