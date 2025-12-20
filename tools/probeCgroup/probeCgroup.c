// SPDX-License-Identifier: GPL-2.0
/*
 * probeCgroup.c - A tool used to get memory usage for each process in a cgroup
 *
 * Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>
 */

#include "probeCgroup.h"

// kretprobe at mem_cgroup_charge
struct charge_data {
	struct cgroup *cgrp;
	struct mem_cgroup *memcg;
	struct task_struct *task;
	unsigned long addr;
};

static int mem_cgroup_charge_entry_handler(struct kretprobe_instance *ri,
					   struct pt_regs *regs)
{
	struct charge_data *data;
	struct folio *page;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct cgroup_subsys_state css;
	struct cgroup *cgrp;

	if (!current->mm)
		return 1;
	page = (struct folio *)regs->di;
	mm = (struct mm_struct *)regs->si;
	if (mm == NULL || page == NULL)
		return -1;
	memcg = get_mem_cgroup_from_mm(mm);
	if (memcg != NULL) {
		css = memcg->css;
		cgrp = css.cgroup;

		data = (struct charge_data *)ri->data;
		data->memcg = memcg;
		data->addr = (unsigned long)page;
		data->task = current;
		data->cgrp = cgrp;
	}
	return 0;
}

NOKPROBE_SYMBOL(mem_cgroup_charge_entry_handler);

static int mem_cgroup_charge_ret_handler(struct kretprobe_instance *ri,
					 struct pt_regs *regs)
{
	unsigned long retval = regs_return_value(regs);
	struct charge_data *data = (struct charge_data *)ri->data;
	int id;
	struct cgroup_info *cgrp_info;
	struct task_info *tsk_info;

	if (data->memcg != NULL && retval == 0) {
		id = ((data->memcg)->css).id;

		spin_lock(&lock);
		cgrp_info = find_cgroup_info(id);
		if (cgrp_info == NULL) {
			cgrp_info = create_cgroup_info(data->cgrp, data->memcg);
			if (cgrp_info == NULL) {
				spin_unlock(&lock);
				return -1;
			}
			add_cgroup_info(cgrp_info);
		}
		spin_unlock(&lock);

		read_lock(&cgrp_info->cgrp_lock);
		tsk_info = find_task_info(cgrp_info, data->task->tgid);
		read_unlock(&cgrp_info->cgrp_lock);

		// for some cases, task->comm changes over time
		if (tsk_info != NULL
		    && strcmp(data->task->comm, tsk_info->comm) != 0) {
			strscpy(tsk_info->comm, data->task->comm,
				sizeof(tsk_info->comm));
		}

		if (tsk_info == NULL) {
			tsk_info = create_task_info(data->task);
			if (tsk_info == NULL)
				return -1;
			add_task_to_cgroup_info(cgrp_info, tsk_info);
		}

		if (HashMap_insert(tsk_info->pages, data->addr)) {
			//update counter
			spin_lock(&(tsk_info->cnt_lock));
			tsk_info->count +=
			    folio_nr_pages((struct folio *)data->addr);
			spin_unlock(&(tsk_info->cnt_lock));
		}
	}

	return 0;

}

NOKPROBE_SYMBOL(mem_cgroup_charge_ret_handler);

static struct kretprobe mem_cgroup_charge_kretprobe = {
	.handler = mem_cgroup_charge_ret_handler,
	.entry_handler = mem_cgroup_charge_entry_handler,
	.data_size = sizeof(struct charge_data),
	.maxactive = 20,
};

static int mem_cgroup_charge_kretprobe_init(void)
{
	int ret;

	mem_cgroup_charge_kretprobe.kp.symbol_name = "__mem_cgroup_charge";
	ret = register_kretprobe(&mem_cgroup_charge_kretprobe);
	if (ret < 0) {
		pr_err("register_kretprobe failed, returned %d\n", ret);
		return ret;
	}
	pr_info("Planted return probe at %s: %p\n",
		mem_cgroup_charge_kretprobe.kp.symbol_name,
		mem_cgroup_charge_kretprobe.kp.addr);
	return 0;
}

static void mem_cgroup_charge_kretprobe_exit(void)
{
	unregister_kretprobe(&mem_cgroup_charge_kretprobe);
	pr_info("kretprobe at %p unregistered\n",
		mem_cgroup_charge_kretprobe.kp.addr);

	/* nmissed > 0 suggests that maxactive was set too low. */
	pr_info("Missed probing %d instances of %s\n",
		mem_cgroup_charge_kretprobe.nmissed,
		mem_cgroup_charge_kretprobe.kp.symbol_name);
}

// kretprobe at uncharge_folio

struct uncharge_data {
	struct cgroup *cgrp;
	struct mem_cgroup *memcg;
	unsigned long addr;
	bool isKmem;
	int nr_pages;
};

static int uncharge_folio_entry_handler(struct kretprobe_instance *ri,
					struct pt_regs *regs)
{
	struct uncharge_data *data;
	struct folio *page;
	struct mem_cgroup *memcg = NULL;
	struct cgroup_subsys_state css;
	struct cgroup *cgrp;
	struct obj_cgroup *objcg;
	int nr_pages = 0;

	data = (struct uncharge_data *)ri->data;
	page = (struct folio *)regs->di;
	if (page == NULL) {
		data->memcg = NULL;
		return -1;
	}
	if (page->memcg_data & MEMCG_DATA_KMEM) {	// if the page belongs to kmem
		if (!folio_test_large(page))
			nr_pages = 1;
		else
			nr_pages = page->_folio_nr_pages;
		// nr_pages = thp_nr_pages(page);
		objcg = __folio_objcg(page);
		if (objcg != NULL)
			memcg = objcg->memcg;
		data->isKmem = true;
		data->nr_pages = nr_pages;
	} else {
		memcg = __folio_memcg(page);
		data->isKmem = false;
	}

	if (memcg != NULL) {
		css = memcg->css;
		cgrp = css.cgroup;

		data->memcg = memcg;
		data->addr = (unsigned long)page;
		data->cgrp = cgrp;
	}
	return 0;
}

NOKPROBE_SYMBOL(uncharge_folio_entry_handler);

static int uncharge_folio_ret_handler(struct kretprobe_instance *ri,
				      struct pt_regs *regs)
{
	struct uncharge_data *data = (struct uncharge_data *)ri->data;
	int id;
	struct cgroup_info *cgrp_info;
	int ret = -1;

	if (data->memcg != NULL) {
		id = ((data->memcg)->css).id;
		cgrp_info = find_cgroup_info(id);
		if (cgrp_info == NULL)
			return -1;
		if (data->isKmem)
			ret = -1;
		else
			ret = remove_page_from_cgroup_info(data->addr, cgrp_info);
	}

	return ret;
}

NOKPROBE_SYMBOL(uncharge_folio_ret_handler);

static struct kretprobe uncharge_folio_kretprobe = {
	.handler = uncharge_folio_ret_handler,
	.entry_handler = uncharge_folio_entry_handler,
	.data_size = sizeof(struct uncharge_data),
	.maxactive = 20,
};

static int uncharge_folio_kretprobe_init(void)
{
	int ret;

	uncharge_folio_kretprobe.kp.symbol_name = "uncharge_folio";
	ret = register_kretprobe(&uncharge_folio_kretprobe);
	if (ret < 0) {
		pr_err("register_kretprobe failed, returned %d\n", ret);
		return ret;
	}
	pr_info("Planted return probe at %s: %p\n",
		uncharge_folio_kretprobe.kp.symbol_name,
		uncharge_folio_kretprobe.kp.addr);
	return 0;
}

static void uncharge_folio_kretprobe_exit(void)
{
	unregister_kretprobe(&uncharge_folio_kretprobe);
	pr_info("kretprobe at %p unregistered\n",
		uncharge_folio_kretprobe.kp.addr);

	/* nmissed > 0 suggests that maxactive was set too low. */
	pr_info("Missed probing %d instances of %s\n",
		uncharge_folio_kretprobe.nmissed,
		uncharge_folio_kretprobe.kp.symbol_name);
}

//kprobe at do_exit
static struct kprobe do_exit_kprobe;
static int do_exit_kprobe_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct task_struct *cur = current;
	int tgid = cur->tgid;
	struct mm_struct *mm = cur->mm;
	struct mem_cgroup *memcg = get_mem_cgroup_from_mm(mm);
	struct cgroup_subsys_state css;
	struct cgroup *cgrp;
	struct cgroup_info *cgrp_info;
	struct task_info *tsk_info;
	int id;

	if (memcg != NULL) {
		css = memcg->css;
		cgrp = css.cgroup;
		id = (memcg->css).id;
		cgrp_info = find_cgroup_info(id);
		if (cgrp_info != NULL) {
			write_lock(&cgrp_info->cgrp_lock);
			tsk_info = find_task_info(cgrp_info, tgid);
			if (tsk_info != NULL) {
				list_del(&tsk_info->list);
				write_unlock(&cgrp_info->cgrp_lock);
				remove_task_from_cgroup_info(cgrp_info,
							     tsk_info);
			} else {
				write_unlock(&cgrp_info->cgrp_lock);
			}
			return 0;
		}
	}
	return 0;
}

static void do_exit_kprobe_post_handler(struct kprobe *p,
					struct pt_regs *regs,
					unsigned long flags)
{

}

static int do_exit_kprobe_init(void)
{
	do_exit_kprobe.pre_handler = do_exit_kprobe_pre_handler;
	do_exit_kprobe.post_handler = do_exit_kprobe_post_handler;
	do_exit_kprobe.symbol_name = "do_exit";
	if (register_kprobe(&do_exit_kprobe)) {
		pr_alert("register_kprobe on do_exit failed!\n");
		return -EINVAL;
	}
	return 0;
}

static void do_exit_kprobe_exit(void)
{
	unregister_kprobe(&do_exit_kprobe);
}

//kprobe at mark_oom_victim
static struct kprobe mark_oom_victim_kprobe;

static int mark_oom_victim_kprobe_pre_handler(struct kprobe *p,
					      struct pt_regs *regs)
{
	struct task_struct *victim;
	int tgid;
	struct mm_struct *mm;
	struct mem_cgroup *memcg;
	struct cgroup_subsys_state css;
	struct cgroup *cgrp;
	struct cgroup_info *cgrp_info;
	struct task_info *tsk_info;
	int id;
	struct task_info *oom_info;

	victim = (struct task_struct *)regs->di;
	tgid = victim->tgid;
	mm = victim->mm;
	memcg = get_mem_cgroup_from_mm(mm);
	if (memcg != NULL) {
		css = memcg->css;
		cgrp = css.cgroup;
		id = (memcg->css).id;
		cgrp_info = find_cgroup_info(id);
		if (cgrp_info != NULL) {
			read_lock(&cgrp_info->cgrp_lock);
			tsk_info = find_task_info(cgrp_info, tgid);
			read_unlock(&cgrp_info->cgrp_lock);
			if (tsk_info != NULL) {
				oom_info = create_oom_task_info(tsk_info);
				if (oom_info != NULL) {
					add_oom_task_to_cgroup_info(cgrp_info,
								    oom_info);
				}
				return 0;
			}
		}
	}
	return 0;
}

static void mark_oom_victim_kprobe_post_handler(struct kprobe *p,
						struct pt_regs *regs,
						unsigned long flags)
{

}

static int mark_oom_victim_kprobe_init(void)
{
	mark_oom_victim_kprobe.pre_handler = mark_oom_victim_kprobe_pre_handler;
	mark_oom_victim_kprobe.post_handler =
	    mark_oom_victim_kprobe_post_handler;
	mark_oom_victim_kprobe.symbol_name = "mark_oom_victim";
	if (register_kprobe(&mark_oom_victim_kprobe)) {
		pr_alert("register_kprobe on mark_oom_victim failed!\n");
		return -EINVAL;
	}
	return 0;
}

static void mark_oom_victim_kprobe_exit(void)
{
	unregister_kprobe(&mark_oom_victim_kprobe);
}

//kretporbe at cgroup_destroy_locked
struct destroy_data {
	struct cgroup *cgrp;
};

static int cgroup_destroy_locked_entry_handler(struct kretprobe_instance
					       *ri, struct pt_regs *regs)
{
	struct destroy_data *data;

	data = (struct destroy_data *)ri->data;
	data->cgrp = (struct cgroup *)regs->di;
	return 0;
}

NOKPROBE_SYMBOL(cgroup_destroy_locked_entry_handler);

static int cgroup_destroy_locked_ret_handler(struct kretprobe_instance *ri,
					     struct pt_regs *regs)
{
	struct destroy_data *data = (struct destroy_data *)ri->data;
	struct cgroup *cgrp = data->cgrp;
	struct cgroup_info *cgrp_info = NULL;
	unsigned long retval = regs_return_value(regs);

	if (!cgrp)
		return -1;
	if (retval != 0)
		return -1;
	list_for_each_entry(cgrp_info, &all_cgroup_info, list) {
		if (cgrp_info->cgrp == cgrp) {
			spin_lock(&lock);
			list_del(&cgrp_info->list);
			spin_unlock(&lock);
			destroy_cgroup_info(cgrp_info);
			return 0;
		}
	}
	return -1;
}

NOKPROBE_SYMBOL(cgroup_destroy_locked_ret_handler);

static struct kretprobe cgroup_destroy_locked_kretprobe = {
	.handler = cgroup_destroy_locked_ret_handler,
	.entry_handler = cgroup_destroy_locked_entry_handler,
	.data_size = sizeof(struct destroy_data),
	.maxactive = 20,
};

static int cgroup_destroy_locked_kretprobe_init(void)
{
	int ret;

	cgroup_destroy_locked_kretprobe.kp.symbol_name =
	    "cgroup_destroy_locked";
	ret = register_kretprobe(&cgroup_destroy_locked_kretprobe);
	if (ret < 0) {
		pr_err("register_kretprobe failed, returned %d\n", ret);
		return ret;
	}
	pr_info("Planted return probe at %s: %p\n",
		cgroup_destroy_locked_kretprobe.kp.symbol_name,
		cgroup_destroy_locked_kretprobe.kp.addr);
	return 0;
}

static void cgroup_destroy_locked_kretprobe_exit(void)
{
	unregister_kretprobe(&cgroup_destroy_locked_kretprobe);
	pr_info("kretprobe at %p unregistered\n",
		cgroup_destroy_locked_kretprobe.kp.addr);

	/* nmissed > 0 suggests that maxactive was set too low. */
	pr_info("Missed probing %d instances of %s\n",
		cgroup_destroy_locked_kretprobe.nmissed,
		cgroup_destroy_locked_kretprobe.kp.symbol_name);
}

// print the tasks in order of their memory usage
static void print_sorted_tasks_list(struct cgroup_info *cgrp_info,
				    int type, struct seq_file *m)
{
	struct list_head *cur, *insert_pos;
	struct task_info *task, *insert_task;
	struct list_head new_list = LIST_HEAD_INIT(new_list);
	struct list_head *old_list;
	struct task_info *new_task, *next_task;

	if (type == 0) {
		if (cgrp_info == NULL)
			return;
		read_lock(&cgrp_info->cgrp_lock);
		old_list = &cgrp_info->tasks_list;
	} else {
		if (cgrp_info == NULL)
			return;
		old_list = &cgrp_info->oom_list;
	}

	list_for_each_entry_safe(task, insert_task, old_list, list) {
		new_task = kmalloc(sizeof(struct task_info), GFP_ATOMIC);
		if (!new_task)
			return;
		new_task->tgid = task->tgid;
		strscpy(new_task->comm, task->comm, sizeof(new_task->comm));
		new_task->count = task->count;
		new_task->pages = NULL;
		INIT_LIST_HEAD(&new_task->list);

		//insertion sort
		cur = &new_list;
		insert_pos = cur->next;
		while (insert_pos != &new_list) {
			next_task =
			    list_entry(insert_pos, struct task_info, list);
			if (new_task->count >= next_task->count)
				break;
			cur = insert_pos;
			insert_pos = insert_pos->next;
		}

		(&new_task->list)->prev = insert_pos->prev;
		(insert_pos->prev)->next = (&new_task->list);
		(&new_task->list)->next = insert_pos;
		insert_pos->prev = (&new_task->list);
	}
	if (type == 0)
		read_unlock(&cgrp_info->cgrp_lock);

	//print
	if (type == 1 && (&new_list) != new_list.next) {
		seq_puts(m, "oom:\n");
		seq_printf(m, "%10s %20s %20s\n", "pid", "command",
			   "memory usage (KB)");
	}
	if (type == 0)
		seq_printf(m, "%10s %20s %20s\n", "pid", "command",
			   "memory usage (KB)");
	list_for_each_entry_safe(task, insert_task, &new_list, list) {
		seq_printf(m, "%10d %20s %20d\n", task->tgid, task->comm,
			   (task->count) * 4);
	}

	list_for_each_entry_safe(task, insert_task, &new_list, list) {
		list_del(&task->list);
		kfree(task);
	}
}

static struct proc_dir_entry *cgroup_info_read;
#define procfs_file_read	"cgroup_memory_usage_per_process"

void seq_print_tasks(struct cgroup_info *cgroup_info, struct seq_file *m)
{
	if (!cgroup_info)
		return;

	print_sorted_tasks_list(cgroup_info, 0, m);
}

void seq_print_oom_tasks(struct cgroup_info *cgroup_info, struct seq_file *m)
{
	if (!cgroup_info)
		return;

	print_sorted_tasks_list(cgroup_info, 1, m);
}

void seq_print_cgroups(struct seq_file *m)
{
	struct cgroup_info *cgrp, *pos;

	spin_lock(&lock);
	list_for_each_entry_safe(cgrp, pos, &all_cgroup_info, list) {
		seq_printf(m, "cgroup name : %s\n", cgrp->name);
		seq_print_tasks(cgrp, m);
		seq_print_oom_tasks(cgrp, m);
		seq_puts(m, "\n");
	}
	spin_unlock(&lock);
}

static int memory_usage_show(struct seq_file *m, void *v)
{
	seq_print_cgroups(m);
	return 0;
}

static int __init global_init(void)
{
	int ret = 0;

	cgroup_info_read =
	    proc_create_single(procfs_file_read, 0, NULL, memory_usage_show);
	if (!cgroup_info_read)
		return -ENOMEM;
	ret = mem_cgroup_charge_kretprobe_init();
	uncharge_folio_kretprobe_init();
	do_exit_kprobe_init();
	mark_oom_victim_kprobe_init();
	cgroup_destroy_locked_kretprobe_init();

	return ret;
}

static void __exit global_exit(void)
{
	struct cgroup_info *cgrp_info, *pos;

	mem_cgroup_charge_kretprobe_exit();
	uncharge_folio_kretprobe_exit();
	do_exit_kprobe_exit();
	mark_oom_victim_kprobe_exit();
	cgroup_destroy_locked_kretprobe_exit();

	remove_proc_entry(procfs_file_read, NULL);

	//release all memory use
	list_for_each_entry_safe(cgrp_info, pos, &all_cgroup_info, list) {
		list_del(&cgrp_info->list);
		destroy_cgroup_info(cgrp_info);
	}
}

module_init(global_init)
module_exit(global_exit)
MODULE_LICENSE("GPL");
