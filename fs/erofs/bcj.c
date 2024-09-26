// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
/*
 * Copyright (C) 2024 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by MercyHeart <2384268568@qq.com>
 * Created by Feynman G <gxnorz@gmail.com>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/psi.h>
#include "bcj.h"
#define Test86MSByte(b) ((b) == 0 || (b) == 0xFF)

struct lzma_simple_x86 {
	uint32_t prev_mask;
	uint32_t prev_pos;
};

static size_t
x86_code(void *simple_ptr, uint32_t now_pos, bool is_encoder,
		uint8_t *buffer, size_t size)
{
	static const bool MASK_TO_ALLOWED_STATUS[8]
		= { true, true, true, false, true, false, false, false };

	static const uint32_t MASK_TO_BIT_NUMBER[8]
			= { 0, 1, 2, 2, 3, 3, 3, 3 };

	struct lzma_simple_x86 *simple = simple_ptr;
	uint32_t prev_mask = simple->prev_mask;
	uint32_t prev_pos = simple->prev_pos;

	if (size < 5)
		return 0;

	if (now_pos - prev_pos > 5)
		prev_pos = now_pos - 5;

	const size_t limit = size - 5;
	size_t buffer_pos = 0;

	while (buffer_pos <= limit) {
		uint8_t b = buffer[buffer_pos];

		if (b != 0xE8 && b != 0xE9) {
			++buffer_pos;
			continue;
		}

		const uint32_t offset = now_pos + (uint32_t)(buffer_pos)
				- prev_pos;
		prev_pos = now_pos + (uint32_t)(buffer_pos);

		if (offset > 5) {
			prev_mask = 0;
		} else {
			for (uint32_t i = 0; i < offset; ++i) {
				prev_mask &= 0x77;
				prev_mask <<= 1;
			}
		}

		b = buffer[buffer_pos + 4];

		if (Test86MSByte(b)
			&& MASK_TO_ALLOWED_STATUS[(prev_mask >> 1) & 0x7]
				&& (prev_mask >> 1) < 0x10) {

			uint32_t src = ((uint32_t)(b) << 24)
				| ((uint32_t)(buffer[buffer_pos + 3]) << 16)
				| ((uint32_t)(buffer[buffer_pos + 2]) << 8)
				| (buffer[buffer_pos + 1]);

			uint32_t dest;

			while (true) {
				if (is_encoder)
					dest = src + (now_pos + (uint32_t)(
							buffer_pos) + 5);
				else
					dest = src - (now_pos + (uint32_t)(
							buffer_pos) + 5);

				if (prev_mask == 0)
					break;

				const uint32_t i = MASK_TO_BIT_NUMBER[
						prev_mask >> 1];

				b = (uint8_t)(dest >> (24 - i * 8));

				if (!Test86MSByte(b))
					break;

				src = dest ^ ((1U << (32 - i * 8)) - 1);
			}

			buffer[buffer_pos + 4]
					= (uint8_t)(~(((dest >> 24) & 1) - 1));
			buffer[buffer_pos + 3] = (uint8_t)(dest >> 16);
			buffer[buffer_pos + 2] = (uint8_t)(dest >> 8);
			buffer[buffer_pos + 1] = (uint8_t)(dest);
			buffer_pos += 5;
			prev_mask = 0;

		} else {
			++buffer_pos;
			prev_mask |= 1;
			if (Test86MSByte(b))
				prev_mask |= 0x10;
		}
	}

	simple->prev_mask = prev_mask;
	simple->prev_pos = prev_pos;

	return buffer_pos;
}

static size_t arm_code(uint32_t now_pos, bool is_encoder,
		uint8_t *buffer, size_t size)
{
	size_t i;

	for (i = 0; i + 4 <= size; i += 4) {
		if (buffer[i + 3] == 0xEB) {
			uint32_t src = ((uint32_t)(buffer[i + 2]) << 16)
					| ((uint32_t)(buffer[i + 1]) << 8)
					| (uint32_t)(buffer[i + 0]);
			src <<= 2;

			uint32_t dest;

			if (is_encoder)
				dest = now_pos + (uint32_t)(i) + 8 + src;
			else
				dest = src - (now_pos + (uint32_t)(i) + 8);

			dest >>= 2;
			buffer[i + 2] = (dest >> 16);
			buffer[i + 1] = (dest >> 8);
			buffer[i + 0] = dest;
		}
	}

	return i;
}

// 检查系统的字节序，判断是小端还是大端
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define IS_LITTLE_ENDIAN 1
#else
#define IS_LITTLE_ENDIAN 0
#endif

// 如果是大端系统，定义字节交换宏
#define UINT32_C(c) c##U
#ifndef bswap32
#define bswap32(n) ((uint32_t)( \
(((n) & UINT32_C(0x000000FF)) << 24) | \
(((n) & UINT32_C(0x0000FF00)) << 8)  | \
(((n) & UINT32_C(0x00FF0000)) >> 8)  | \
(((n) & UINT32_C(0xFF000000)) >> 24)))
#endif

// 读取32位小端整数
static inline uint32_t read32le(const uint8_t *buf)
{
	uint32_t num;

	memcpy(&num, buf, sizeof(num));
    // 如果系统是大端序，则进行字节交换
	if (!IS_LITTLE_ENDIAN)
		num = bswap32(num);
	return num;
}

// 写入32位小端整数
static inline void write32le(uint8_t *buf, uint32_t num)
{
	if (!IS_LITTLE_ENDIAN)
		num = bswap32(num);
	memcpy(buf, &num, sizeof(num));
}

static size_t arm64_code(uint32_t now_pos, bool is_encoder,
		uint8_t *buffer, size_t size)
{
	size_t i;

#ifdef __clang__
#	pragma clang loop vectorize(disable)
#endif
	for (i = 0; i + 4 <= size; i += 4) {
		uint32_t pc = (uint32_t)(now_pos + i);
		uint32_t instr = read32le(buffer + i);

		if ((instr >> 26) == 0x25) {
			const uint32_t src = instr;

			instr = 0x94000000;

			pc >>= 2;
			if (!is_encoder)
				pc = 0U - pc;

			instr |= (src + pc) & 0x03FFFFFF;
			write32le(buffer + i, instr);

		} else if ((instr & 0x9F000000) == 0x90000000) {
			const uint32_t src = ((instr >> 29) & 3)
					| ((instr >> 3) & 0x001FFFFC);
			if ((src + 0x00020000) & 0x001C0000)
				continue;

			instr &= 0x9000001F;

			pc >>= 12;
			if (!is_encoder)
				pc = 0U - pc;

			const uint32_t dest = src + pc;

			instr |= (dest & 3) << 29;
			instr |= (dest & 0x0003FFFC) << 3;
			instr |= (0U - (dest & 0x00020000)) & 0x00E00000;
			write32le(buffer + i, instr);
		}
	}

	return i;
}

int bcj_code(uint8_t *buf, uint32_t startpos, size_t size, int bcj_type, bool is_encode)
{
	size_t processed_size = 0;
	struct lzma_simple_x86 simple;

	switch (bcj_type) {
	case 1:
		simple.prev_mask = 0;
		simple.prev_pos = (uint32_t)(-5);
		processed_size = x86_code(&simple, startpos, is_encode, buf, size);
		break;
	case 2:
		processed_size = arm_code(startpos, is_encode, buf, size);
		break;
	case 3:
		processed_size = arm64_code(startpos, is_encode, buf, size);
		break;
	default:
		break;
	}
	return processed_size;
}
