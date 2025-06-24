/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VM_OBJECT_H
#define _VM_OBJECT_H

#include <linux/mm_types.h>
#include <linux/gmem.h>

#ifdef CONFIG_GMEM
/* vm_object KAPI */
static inline int __init vm_object_init(void) { return 0; }
static inline struct gm_mapping *vm_object_lookup(struct vm_object *obj,
					unsigned long va) { return NULL; }
static inline void vm_object_mapping_create(struct vm_object *obj,
					unsigned long start) { return 0; }
#endif

#endif /* _VM_OBJECT_H */
