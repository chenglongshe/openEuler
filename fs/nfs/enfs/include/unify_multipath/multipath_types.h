/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright 2024, Huawei Inc
 *
 * Platform dependent utilities
 */
#ifndef _MULTIPATH_TYPES_H_
#define _MULTIPATH_TYPES_H_

#ifndef uint8_t
typedef unsigned char uint8_t;
#endif

#ifndef uint32_t
typedef unsigned int uint32_t;
#endif

#ifndef BOOLEAN_T
#define BOOLEAN_T int
#define B_TRUE 1
#define B_FALSE 0
#endif

#ifndef LOCAL_LLT
#ifndef uint64_t
typedef unsigned long long uint64_t;
#endif
#else
#ifndef uint64_t
typedef unsigned long int uint64_t;
#endif
#endif

#endif