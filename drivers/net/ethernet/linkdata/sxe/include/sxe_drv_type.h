/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2024 - 2025 Intel Corporation. */

#ifndef __SXE_DRV_TYPEDEF_H__
#define __SXE_DRV_TYPEDEF_H__

#ifdef SXE_DPDK
#include "sxe_types.h"
#ifndef bool
typedef _Bool bool;
#endif
#else
#include <linux/types.h>
#endif

typedef u8 U8;
typedef u16 U16;
typedef u32 U32;
typedef u64 U64;

#endif
