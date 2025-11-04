/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 */

#ifndef __MM_PAGE_OWNER_H
#define __MM_PAGE_OWNER_H

#include <linux/stackdepot.h>

struct page_owner {
	unsigned short order;
	short last_migrate_reason;
	gfp_t gfp_mask;
	depot_stack_handle_t handle;
	depot_stack_handle_t free_handle;
	u64 ts_nsec;
	u64 free_ts_nsec;
	char comm[TASK_COMM_LEN];
	pid_t pid;
	pid_t tgid;
#ifdef CONFIG_PAGE_OWNER_MODULE_STAT
	char module_name[MODULE_NAME_LEN];
#endif
};

#ifdef CONFIG_PAGE_OWNER_MODULE_STAT
void po_find_module_name_with_update(depot_stack_handle_t handle, char *mod_name,
		size_t size, long nr_pages);
void po_set_module_name(struct page_owner *page_owner, char *mod_name);
int po_module_name_snprint(struct page_owner *page_owner, char *kbuf, size_t size);
void po_module_stat_init(void);
bool po_is_filtered(struct page_owner *page_owner);

static inline void po_copy_module_name(struct page_owner *dst,
		struct page_owner *src)
{
	po_set_module_name(dst, src->module_name);
}

#else
static void po_find_module_name_with_update(depot_stack_handle_t handle, char *mod_name,
		size_t size, long nr_pages)
{
}

static void po_set_module_name(struct page_owner *page_owner, char *mod_name)
{
}

static inline int po_module_name_snprint(struct page_owner *page_owner,
		char *kbuf, size_t size)
{
	return 0;
}

static inline void po_copy_module_name(struct page_owner *dst, struct page_owner *src)
{
}

static inline void po_module_stat_init(void)
{
}

static inline bool po_is_filtered(struct page_owner *page_owner)
{
	return false;
}
#endif

#endif
