/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PS3_TYPES_H__
#define __PS3_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __KERNEL__
#include <stddef.h>
#endif

#define PS3_FALSE 0
#define PS3_TRUE 1

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t)(&((TYPE *)0)->MEMBER))
#endif

#ifndef PS3_MIN
#define PS3_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef PS3_MAX
#define PS3_MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef PS3_MIN_NON_ZERO
#define PS3_MIN_NON_ZERO(a, b)                                                 \
	((a) == 0 ? (b) : ((b) == 0 ? (a) : (PS3_MIN(a, b))))
#endif

#ifndef TYPEOF
#ifdef __cplusplus
#define TYPEOF decltype
#else
#define TYPEOF typeof
#endif
#endif

#ifndef container_of
#ifndef PCLINT
#define container_of(ptr, type, member)                                        \
	({                                                                     \
		const TYPEOF(((type *)0)->member) * __mptr = (ptr);             \
		(type *)((char *)__mptr - offsetof(type, member));             \
	})
#else
#define container_of(ptr, type, member) ((type *)(void *)(char *)ptr)
#endif
#endif

#ifndef PS3_DESC
#define PS3_DESC(a) 1
#endif

#ifndef PS3_IN
#define PS3_IN
#endif

#ifndef PS3_OUT
#define PS3_OUT
#endif

union RmMiscInfo {
	struct {
		unsigned char idx;
		unsigned char status;
		unsigned char valid;
		unsigned char hasInQueued;
	};
	unsigned int val;
};

#ifdef __cplusplus
}
#endif

#endif
