/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * Description: ubagg kernel module
 * Author: Weicheng Zhang
 * Create: 2025-8-6
 * Note:
 * History: 2025-8-6: Create file
 */

#ifndef UBAGG_BITMAP_H
#define UBAGG_BITMAP_H

#include <linux/spinlock.h>
#include "ubagg_types.h"

struct ubagg_bitmap {
	unsigned long *bits;
	uint32_t size;
	spinlock_t lock;
	uint64_t alloc_idx; /* Allocated index */
};

#define UBAGG_BITMAP_MAX_SIZE (1 << 16)

struct ubagg_bitmap *ubagg_bitmap_alloc(uint32_t bitmap_size);

void ubagg_bitmap_free(struct ubagg_bitmap *bitmap);

int ubagg_bitmap_alloc_idx(struct ubagg_bitmap *bitmap);

int ubagg_bitmap_use_id(struct ubagg_bitmap *bitmap, uint32_t id);

int ubagg_bitmap_free_idx(struct ubagg_bitmap *bitmap, int idx);

int ubagg_bitmap_alloc_idx_from_offset(struct ubagg_bitmap *bitmap, int offset);

int ubagg_bitmap_alloc_idx_from_offset_nolock(struct ubagg_bitmap *bitmap,
					      uint64_t offset);

#endif
