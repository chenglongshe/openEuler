/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SYMS_LOOKUP_H
#define _SYMS_LOOKUP_H

#include <linux/types.h>

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

extern kallsyms_lookup_name_t generic_kallsyms_lookup_name;

/*
 * lookup kallsyms_lookup_name()
 */
extern int __init syms_lookup_init(void);

/*
 * free kallsyms_lookup_name()
 */
extern void syms_lookup_exit(void);
#endif /* _SYMS_LOOKUP_H */
