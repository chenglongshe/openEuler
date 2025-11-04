// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 *
 * page_owner_module core file
 */

#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/list_sort.h>
#include <linux/oom.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>

#include "page_owner.h"

#define PAGE_OWNER_FILTER_BUF_SIZE 16
#define PAGE_OWNER_NONE_FILTER 0
#define PAGE_OWNER_MODULE_FILTER 1

#define PO_MODULE_DEFAULT_TOPN 20

static unsigned int page_owner_filter = PAGE_OWNER_NONE_FILTER;

struct po_module {
	struct list_head list;
	struct module *mod;
	long nr_pages_used;
};

struct leaked_po_module {
	struct list_head list;
	char module_name[MODULE_NAME_LEN];
	long nr_pages_used;
	u64 unload_ns;
};

LIST_HEAD(po_module_list);
LIST_HEAD(leaked_po_module_list);
DEFINE_SPINLOCK(po_module_list_lock);

static unsigned int po_module_topn = PO_MODULE_DEFAULT_TOPN;

static int po_module_cmp(void *priv, const struct list_head *h1,
		const struct list_head *h2)
{
	struct po_module *lhs, *rhs;

	lhs = container_of(h1, struct po_module, list);
	rhs = container_of(h2, struct po_module, list);

	return lhs->nr_pages_used < rhs->nr_pages_used;
}

static inline struct po_module *po_find_module(const struct module *mod)
{
	struct po_module *po_mod;

	lockdep_assert_held(&po_module_list_lock);
	list_for_each_entry(po_mod, &po_module_list, list) {
		if (po_mod->mod == mod)
			return po_mod;
	}

	pr_warn("page_owner_module: failed to find module %s in po_module list\n",
		mod->name);
	return NULL;
}

void po_update_module_pages(const struct module *mod, long nr_pages)
{
	struct po_module *po_mod;
	unsigned long flags;

	if (unlikely(!mod))
		return;

	spin_lock_irqsave(&po_module_list_lock, flags);
	po_mod = po_find_module(mod);
	if (po_mod)
		po_mod->nr_pages_used += nr_pages;
	spin_unlock_irqrestore(&po_module_list_lock, flags);
}


void po_find_module_name_with_update(depot_stack_handle_t handle, char *mod_name,
		size_t size, long nr_pages)
{
	int i;
	struct module *mod = NULL;
	unsigned long *entries;
	unsigned int nr_entries;

	if (unlikely(!mod_name))
		return;

	nr_entries = stack_depot_fetch(handle, &entries);
	if (!in_task())
		nr_entries = filter_irq_stacks(entries, nr_entries);
	for (i = 0; i < nr_entries; i++) {
		if (core_kernel_text(entries[i]))
			continue;

		preempt_disable();
		mod = __module_address(entries[i]);
		preempt_enable();

		if (!mod)
			continue;

		strscpy(mod_name, mod->name, size);
		po_update_module_pages(mod, nr_pages);
		return;
	}
}

void po_set_module_name(struct page_owner *page_owner, char *mod_name)
{
	if (unlikely(!page_owner || !mod_name))
		return;

	if (strlen(mod_name) != 0)
		strscpy(page_owner->module_name, mod_name, MODULE_NAME_LEN);
	else
		memset(page_owner->module_name, 0, MODULE_NAME_LEN);
}

static inline bool po_is_module(struct page_owner *page_owner)
{
	return strlen(page_owner->module_name) != 0;
}

int po_module_name_snprint(struct page_owner *page_owner,
		char *kbuf, size_t size)
{
	if (unlikely(!page_owner || !kbuf))
		return 0;

	if (po_is_module(page_owner))
		return scnprintf(kbuf, size, "Page allocated by module %s\n",
					page_owner->module_name);

	return 0;
}

static ssize_t read_page_owner_filter(struct file *file,
			char __user *user_buf, size_t count, loff_t *ppos)
{
	char kbuf[PAGE_OWNER_FILTER_BUF_SIZE];
	int kcount;

	if (page_owner_filter & PAGE_OWNER_MODULE_FILTER)
		kcount = snprintf(kbuf, sizeof(kbuf), "module\n");
	else
		kcount = snprintf(kbuf, sizeof(kbuf), "none\n");

	return simple_read_from_buffer(user_buf, count, ppos, kbuf, kcount);
}

static ssize_t write_page_owner_filter(struct file *file,
	       const char __user *user_buf, size_t count, loff_t *ppos)
{
	char kbuf[PAGE_OWNER_FILTER_BUF_SIZE];
	char *p_kbuf;
	size_t kbuf_size;

	kbuf_size = min(count, sizeof(kbuf) - 1);
	if (copy_from_user(kbuf, user_buf, kbuf_size))
		return -EFAULT;

	kbuf[kbuf_size] = '\0';
	p_kbuf = strstrip(kbuf);

	if (!strcmp(p_kbuf, "module"))
		page_owner_filter = PAGE_OWNER_MODULE_FILTER;
	else if (!strcmp(p_kbuf, "none"))
		page_owner_filter = PAGE_OWNER_NONE_FILTER;
	else
		return -EINVAL;

	return count;
}

static const struct file_operations page_owner_filter_ops = {
	.read =		read_page_owner_filter,
	.write =	write_page_owner_filter,
	.llseek =	default_llseek,
};

bool po_is_filtered(struct page_owner *page_owner)
{
	if (unlikely(!page_owner))
		return false;

	if (page_owner_filter & PAGE_OWNER_MODULE_FILTER &&
		!po_is_module(page_owner))
		return true;

	return false;
}

static int po_module_coming(struct module *mod)
{
	struct po_module *po_mod;
	unsigned long flags;

	po_mod = kmalloc(sizeof(*po_mod), GFP_KERNEL);
	if (!po_mod)
		return -ENOMEM;

	po_mod->nr_pages_used = 0;
	po_mod->mod = mod;
	INIT_LIST_HEAD(&po_mod->list);
	spin_lock_irqsave(&po_module_list_lock, flags);
	list_add_tail(&po_mod->list, &po_module_list);
	spin_unlock_irqrestore(&po_module_list_lock, flags);

	return 0;
}

static void create_leaked_node(struct po_module *po_mod)
{
	struct leaked_po_module *leaked_po_mod;
	unsigned long flags;

	leaked_po_mod = kmalloc(sizeof(struct leaked_po_module), GFP_KERNEL);
	if (!leaked_po_mod)
		return;

	leaked_po_mod->unload_ns = local_clock();
	strscpy(leaked_po_mod->module_name, po_mod->mod->name, MODULE_NAME_LEN);
	leaked_po_mod->nr_pages_used = po_mod->nr_pages_used;
	INIT_LIST_HEAD(&leaked_po_mod->list);
	spin_lock_irqsave(&po_module_list_lock, flags);
	list_add_tail(&leaked_po_mod->list, &leaked_po_module_list);
	spin_unlock_irqrestore(&po_module_list_lock, flags);
}

static void po_module_going(struct module *mod)
{
	struct po_module *po_mod;
	unsigned long flags;

	spin_lock_irqsave(&po_module_list_lock, flags);
	po_mod = po_find_module(mod);
	list_del(&po_mod->list);
	spin_unlock_irqrestore(&po_module_list_lock, flags);

	if (unlikely(po_mod->nr_pages_used))
		create_leaked_node(po_mod);

	kfree(po_mod);
}

static int po_module_notify(struct notifier_block *self,
		unsigned long val, void *data)
{
	struct module *mod = data;
	int ret = 0;

	switch (val) {
	case MODULE_STATE_COMING:
		ret = po_module_coming(mod);
		break;
	case MODULE_STATE_GOING:
		po_module_going(mod);
		break;
	}

	return notifier_from_errno(ret);
}

static struct notifier_block po_module_nb = {
	.notifier_call = po_module_notify,
	.priority = 0
};

static void print_list(unsigned int nr, struct seq_file *m)
{
	struct po_module *po_mod;

	lockdep_assert_held(&po_module_list_lock);

	if (list_empty(&po_module_list))
		return;

	list_sort(NULL, &po_module_list, po_module_cmp);
	list_for_each_entry(po_mod, &po_module_list, list) {
		if (m)
			seq_printf(m, "%s %ld\n", po_mod->mod->name,
				po_mod->nr_pages_used);
		else
			pr_info("\tModule %s allocated %ld pages\n",
				po_mod->mod->name, po_mod->nr_pages_used);
		--nr;
		if (!nr)
			break;
	}
}

static void print_leaked_list(struct seq_file *m)
{
	struct leaked_po_module *leaked_po_mod;

	lockdep_assert_held(&po_module_list_lock);

	if (list_empty(&leaked_po_module_list))
		return;

	list_for_each_entry(leaked_po_mod, &leaked_po_module_list, list) {
		if (m)
			seq_printf(m, "[unloaded %llu]%s %ld\n", leaked_po_mod->unload_ns,
				leaked_po_mod->module_name,	leaked_po_mod->nr_pages_used);
		else
			pr_info("\t[unloaded %llu]Module %s allocated %ld pages\n",
				leaked_po_mod->unload_ns, leaked_po_mod->module_name,
				leaked_po_mod->nr_pages_used);
	}
}

static int po_oom_notify(struct notifier_block *self,
		unsigned long val, void *data)
{
	unsigned long flags;
	unsigned int nr = po_module_topn;
	int ret = notifier_from_errno(0);

	if (!nr)
		return ret;

	spin_lock_irqsave(&po_module_list_lock, flags);
	pr_info("Top modules allocating pages:\n");

	print_list(nr, NULL);
	print_leaked_list(NULL);

	spin_unlock_irqrestore(&po_module_list_lock, flags);

	return ret;
}

static struct notifier_block po_oom_nb = {
	.notifier_call = po_oom_notify,
	.priority = 0
};

static int po_module_topn_set(void *data, u64 val)
{
	po_module_topn = val;
	return 0;
}

static int po_module_topn_get(void *data, u64 *val)
{
	*val = po_module_topn;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(po_module_topn_fops, po_module_topn_get,
			po_module_topn_set, "%llu\n");

static int page_owner_module_stats_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned int nr = po_module_topn;

	if (!nr)
		return 0;

	spin_lock_irqsave(&po_module_list_lock, flags);

	print_list(nr, m);
	print_leaked_list(m);

	spin_unlock_irqrestore(&po_module_list_lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(page_owner_module_stats);


void po_module_stat_init(void)
{
	int ret;

	debugfs_create_file("page_owner_filter", 0600, NULL, NULL,
			&page_owner_filter_ops);

	ret = register_module_notifier(&po_module_nb);
	if (ret) {
		pr_warn("Failed to register page owner module enter notifier\n");
		return;
	}

	ret = register_oom_notifier(&po_oom_nb);
	if (ret)
		pr_warn("Failed to register page owner oom notifier\n");

	debugfs_create_file("page_owner_module_show_max", 0600, NULL, NULL, &po_module_topn_fops);
	debugfs_create_file("page_owner_module_stats", 0400, NULL, NULL,
		&page_owner_module_stats_fops);
}
