/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VM_OBJECT_H
#define _VM_OBJECT_H

#include <linux/mm_types.h>
#include <linux/gmem.h>

#ifdef CONFIG_GMEM
/* vm_object KPI */
int __init vm_object_init(void);
struct vm_object *vm_object_create(struct vm_area_struct *vma);
void vm_object_drop_locked(struct vm_area_struct *vma);
void dup_vm_object(struct vm_area_struct *dst, struct vm_area_struct *src);
void vm_object_adjust(struct vm_area_struct *vma, unsigned long start,
	unsigned long end);

struct gm_mapping *alloc_gm_mapping(void);
struct gm_mapping *vm_object_lookup(struct vm_object *obj, unsigned long va);
void vm_object_mapping_create(struct vm_object *obj, unsigned long start);
void free_gm_mappings(struct vm_area_struct *vma);
#else
static inline void __init vm_object_init(void) {}
static inline struct vm_object *vm_object_create(struct vm_area_struct *vma) { return NULL; }
static inline void vm_object_drop_locked(struct vm_area_struct *vma) {}
static inline void vm_object_adjust(struct vm_area_struct *vma, unsigned long start,
			unsigned long end) {}

static inline struct gm_mapping *alloc_gm_mapping(void) { return NULL; }
static inline struct gm_mapping *vm_object_lookup(struct vm_object *obj,
					unsigned long va) { return NULL; }
static inline void vm_object_mapping_create(struct vm_object *obj,
					unsigned long start) {}
static inline void free_gm_mappings(struct vm_area_struct *vma) {}
#endif

#endif /* _VM_OBJECT_H */
