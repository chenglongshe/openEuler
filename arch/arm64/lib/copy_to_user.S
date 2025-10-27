/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
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

dstin	.req	x0
src	.req	x1
end	.req	x5
srcin	.req	x15
count	.req	x2
tmp1	.req	x3
tmp1w	.req	w3
tmp2	.req	x4
tmp2w	.req	w4
dst	.req	x6

A_l	.req	x7
A_h	.req	x8
B_l	.req	x9
B_h	.req	x10
C_l	.req	x11
C_h	.req	x12
D_l	.req	x13
D_h	.req	x14

#define USER_OFF(off, x...)    USER(fixup_offset_##off, x)
#define FIXUP_OFFSET(n)                                \
fixup_offset_##n:                              \
       sub     x0, end, dst;                   \
       sub     x0, x0, n;                      \
       ret

FIXUP_OFFSET(0)
FIXUP_OFFSET(8)
FIXUP_OFFSET(16)
FIXUP_OFFSET(24)
FIXUP_OFFSET(32)
FIXUP_OFFSET(40)
FIXUP_OFFSET(48)
FIXUP_OFFSET(56)

SYM_FUNC_START(__arch_copy_to_user)
	add	end, x0, x2
	mov	srcin, x1
	mov	dst, dstin
	cmp	count, #16
	/*When memory length is less than 16, the accessed are not aligned.*/
	b.lo	.Ltiny15

	neg	tmp2, src
	ands	tmp2, tmp2, #15/* Bytes to reach alignment. */
	b.eq	.LSrcAligned
	sub	count, count, tmp2
	/*
	* Copy the leading memory data from src to dst in an increasing
	* address order.By this way,the risk of overwriting the source
	* memory data is eliminated when the distance between src and
	* dst is less than 16. The memory accesses here are alignment.
	*/
	tbz	tmp2, #0, 1f
	ldrb	tmp1w, [src], #1
USER_OFF(0,    sttrb tmp1w, [dst, #0])
	add     dst, dst, #1
1:
	tbz	tmp2, #1, 2f
	ldrh	tmp1w, [src], #2
USER_OFF(0,    sttrh tmp1w, [dst, #0])
	add     dst, dst, #2
2:
	tbz	tmp2, #2, 3f
	ldr	tmp1w, [src], #4
USER_OFF(0,    sttr tmp1w, [dst, #0])
	add     dst, dst, #4
3:
	tbz	tmp2, #3, .LSrcAligned
	ldr	tmp1, [src], #8
USER_OFF(0,    sttr tmp1, [dst, #0])
	add     dst, dst, #8

.LSrcAligned:
	cmp	count, #64
	b.ge	.Lcpy_over64
	/*
	* Deal with small copies quickly by dropping straight into the
	* exit block.
	*/
.Ltail63:
	/*
	* Copy up to 48 bytes of data. At this point we only need the
	* bottom 6 bits of count to be accurate.
	*/
	ands	tmp1, count, #0x30
	b.eq	.Ltiny15
	ldp	A_l, A_h, [src], #16
	cmp	tmp1w, #0x20
	b.eq	1f
	b.lt	2f
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
	ldp	A_l, A_h, [src], #16
	add     dst, dst, #16
1:
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
	ldp	A_l, A_h, [src], #16
	add     dst, dst, #16
2:
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
	add     dst, dst, #16
.Ltiny15:
	/*
	* Prefer to break one ldp/stp into several load/store to access
	* memory in an increasing address order,rather than to load/store 16
	* bytes from (src-16) to (dst-16) and to backward the src to aligned
	* address,which way is used in original cortex memcpy. If keeping
	* the original memcpy process here, memmove need to satisfy the
	* precondition that src address is at least 16 bytes bigger than dst
	* address,otherwise some source data will be overwritten when memove
	* call memcpy directly. To make memmove simpler and decouple the
	* memcpy's dependency on memmove, withdrew the original process.
	*/
	tbz	count, #3, 1f
	ldr	tmp1, [src], #8
USER_OFF(0,    sttr tmp1, [dst, #0])
	add     dst, dst, #8
1:
	tbz	count, #2, 2f
	ldr	tmp1w, [src], #4
USER_OFF(0,    sttr tmp1w, [dst, #0])
	add     dst, dst, #4
2:
	tbz	count, #1, 3f
	ldrh	tmp1w, [src], #2
USER_OFF(0,    sttrh tmp1w, [dst, #0])
	add     dst, dst, #2
3:
	tbz	count, #0, .Lexitfunc
	ldrb	tmp1w, [src], #1
USER_OFF(0,    sttrb tmp1w, [dst, #0])
	add     dst, dst, #1

	b	.Lexitfunc

.Lcpy_over64:
	.p2align	L1_CACHE_SHIFT
	ldp	A_l, A_h, [src, #0]
	subs	count, count, #128
	b.ge	.Lcpy_body_large
	/*
	* Less than 128 bytes to copy, so handle 64 here and then jump
	* to the tail.
	*/
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
	ldp	B_l, B_h, [src, #16]
	ldp	C_l, C_h, [src, #32]
USER_OFF(16,   sttr B_l, [dst, #16])
USER_OFF(24,   sttr B_h, [dst, #24])
USER_OFF(32,   sttr C_l, [dst, #32])
USER_OFF(40,   sttr C_h, [dst, #40])
	ldp	D_l, D_h, [src, #48]
	add     src, src, #64
USER_OFF(48,   sttr D_l, [dst, #48])
USER_OFF(56,   sttr D_h, [dst, #56])
	add     dst, dst, #64

	tst	count, #0x3f
	b.ne	.Ltail63
	b	.Lexitfunc

	/*
	* Critical loop.  Start at a new cache line boundary.  Assuming
	* 64 bytes per line this ensures the entire loop is in one line.
	*/
.Lcpy_body_large:
	/* pre-get 64 bytes data. */
	ldp	B_l, B_h, [src, #16]
	ldp	C_l, C_h, [src, #32]
	ldp	D_l, D_h, [src, #48]
	add     src, src, #64
1:
	/*
	* interlace the load of next 64 bytes data block with store of the last
	* loaded 64 bytes data.
	*/
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
	ldp	A_l, A_h, [src, #0]
USER_OFF(16,   sttr B_l, [dst, #16])
USER_OFF(24,   sttr B_h, [dst, #24])
	ldp	B_l, B_h, [src, #16]
USER_OFF(32,   sttr C_l, [dst, #32])
USER_OFF(40,   sttr C_h, [dst, #40])
	ldp	C_l, C_h, [src, #32]
USER_OFF(48,   sttr D_l, [dst, #48])
USER_OFF(56,   sttr D_h, [dst, #56])
	add     dst, dst, #64
	ldp	D_l, D_h, [src, #48]
	add     src, src, #64
	subs	count, count, #64
	b.ge	1b
USER_OFF(0,    sttr A_l, [dst, #0])
USER_OFF(8,    sttr A_h, [dst, #8])
USER_OFF(16,   sttr B_l, [dst, #16])
USER_OFF(24,   sttr B_h, [dst, #24])
USER_OFF(32,   sttr C_l, [dst, #32])
USER_OFF(40,   sttr C_h, [dst, #40])
USER_OFF(48,   sttr D_l, [dst, #48])
USER_OFF(56,   sttr D_h, [dst, #56])
	add     dst, dst, #64

	tst	count, #0x3f
	b.ne	.Ltail63
.Lexitfunc:
	mov	x0, #0
	ret

SYM_FUNC_END(__arch_copy_to_user)
EXPORT_SYMBOL(__arch_copy_to_user)
