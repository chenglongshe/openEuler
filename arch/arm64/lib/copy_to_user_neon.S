/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2025 Huawei Ltd.
 */

#include <linux/linkage.h>

#include <asm/asm-uaccess.h>
#include <asm/assembler.h>
#include <asm/cache.h>

/*
 * Copy to user space from a kernel buffer (alignment handled by the hardware)
 *
 * Parameters:
 *	x0 - to
 *	x1 - from
 *	x2 - n
 * Returns:
 *	x0 - bytes not copied
 */
	.macro ldrb1 reg, ptr, val
	KERNEL_ME_SAFE(9998f, ldrb  \reg, [\ptr], \val)
	.endm

	.macro strb1 reg, ptr, val
	user_ldst 9998f, sttrb, \reg, \ptr, \val
	.endm

	.macro ldrh1 reg, ptr, val
	KERNEL_ME_SAFE(9998f, ldrh  \reg, [\ptr], \val)
	.endm

	.macro strh1 reg, ptr, val
	user_ldst 9997f, sttrh, \reg, \ptr, \val
	.endm

	.macro ldr1 reg, ptr, val
	KERNEL_ME_SAFE(9998f, ldr \reg, [\ptr], \val)
	.endm

	.macro str1 reg, ptr, val
	user_ldst 9997f, sttr, \reg, \ptr, \val
	.endm

	.macro ldp1 reg1, reg2, ptr, val
	KERNEL_ME_SAFE(9998f, ldp \reg1, \reg2, [\ptr], \val)
	.endm

	.macro stp1 reg1, reg2, ptr, val
	user_stp 9997f, \reg1, \reg2, \ptr, \val
	.endm

	.macro stneon reg1, reg2, reg3, reg4, ptr, val
	user_stneon 9987f, \reg1, \reg2, \reg3, \reg4, \ptr, \val
	.endm

	.macro ldneon reg1, reg2, reg3, reg4, ptr, val
	ld1 {\reg1, \reg2, \reg3, \reg4}, [\ptr]
	add \ptr, \ptr, \val
	.endm

end	.req	x5
srcin	.req	x15
SYM_FUNC_START(__arch_copy_to_user_neon)
	add	end, x0, x2
	mov	srcin, x1
	mov	x3, #1		/* enable neon copy */
#include "copy_template.S"
	mov	x0, #0
	ret

	// Exception fixups
9987:	ld1	{V_a.16b, V_b.16b, V_c.16b, V_d.16b}, [sp]  /* restore current regs */
	add	sp, sp, #64
9997:	cmp	dst, dstin
	b.ne	9998f
	// Before being absolutely sure we couldn't copy anything, try harder
KERNEL_ME_SAFE(9998f, ldrb	tmp1w, [srcin])
USER(9998f, sttrb tmp1w, [dst])
	add	dst, dst, #1
9998:	sub	x0, end, dst			// bytes not copied
	ret
SYM_FUNC_END(__arch_copy_to_user_neon)
EXPORT_SYMBOL(__arch_copy_to_user_neon)
