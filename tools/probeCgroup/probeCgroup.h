/* SPDX-License-Identifier: GPL-2.0*/
/*
 * probeCgroup.h
 *
 * Copyright (C) Taoxy2004 <221870066@smail.nju.edu.cn>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/sched.h>
#include <linux/mm_types.h>
#include <linux/memcontrol.h>
#include <linux/cgroup-defs.h>
#include <linux/kernfs.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/oom.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/huge_mm.h>
#include <linux/page-flags.h>
#include <linux/spinlock.h>
#include <linux/rwlock.h>

static spinlock_t lock;		// global lock for the list of cgroup_info

struct HashNode {
	unsigned long addr;
	struct HashNode *next;
};

struct HashNode *HashNode_create(unsigned long addr)
{
	struct HashNode *node = NULL;

	node = kzalloc(sizeof(struct HashNode), GFP_ATOMIC);
	if (node == NULL)
		return NULL;
	node->addr = addr;
	node->next = NULL;
	return node;
}

struct HashBucket {
	struct HashNode *head;
	spinlock_t bkt_lock;
};

void HashBucket_init(struct HashBucket *bkt)
{
	bkt->head = NULL;
	spin_lock_init(&bkt->bkt_lock);
}

bool HashBucket_insert(struct HashBucket *bkt, unsigned long addr)
{
	struct HashNode *new_node;
	struct HashNode *node;
	struct HashNode *prev;
	bool ret = true;

	if (bkt == NULL)
		return false;

	prev = NULL;
	new_node = NULL;
	new_node = HashNode_create(addr);

	spin_lock(&bkt->bkt_lock);
	node = bkt->head;
	while (node != NULL && node->addr != addr) {
		prev = node;
		node = node->next;
	}
	if (node == NULL) {
		if (new_node == NULL) {
			pr_info("not enough memory for HashNode\n");
			spin_unlock(&bkt->bkt_lock);
			return false;
		}
		if (bkt->head == NULL)
			bkt->head = new_node;
		else
			prev->next = new_node;
		spin_unlock(&bkt->bkt_lock);
		ret = true;
	} else {
		spin_unlock(&bkt->bkt_lock);
		kfree(new_node);
		ret = false;
	}

	return ret;
}

bool HashBucket_erase(struct HashBucket *bkt, unsigned long addr)
{
	struct HashNode *node;
	struct HashNode *prev;
	bool ret = true;

	if (bkt == NULL)
		return false;

	spin_lock(&bkt->bkt_lock);
	node = bkt->head;
	prev = NULL;
	while (node != NULL && node->addr != addr) {
		prev = node;
		node = node->next;
	}
	if (node == NULL) {
		spin_unlock(&bkt->bkt_lock);
		ret = false;
	} else {
		if (bkt->head == node)
			bkt->head = node->next;
		else
			prev->next = node->next;
		kfree(node);
		spin_unlock(&bkt->bkt_lock);
		ret = true;
	}

	return ret;
}

void HashBucket_clear(struct HashBucket *bkt)
{
	struct HashNode *node;
	struct HashNode *prev;

	if (bkt == NULL)
		return;

	spin_lock(&bkt->bkt_lock);
	node = bkt->head;
	prev = NULL;
	bkt->head = NULL;
	while (node != NULL) {
		prev = node;
		node = node->next;
		kfree(prev);
	}
	spin_unlock(&bkt->bkt_lock);
}

struct HashMap {
	unsigned long size;
	struct HashBucket *HashTable;
};

unsigned long hash_func(unsigned long addr, unsigned long size)
{
	return addr % size;
}

struct HashMap *HashMap_create(unsigned long size)
{
	struct HashMap *hm = NULL;
	struct HashBucket *ht = NULL;
	int i = 0;

	hm = kmalloc(sizeof(struct HashMap), GFP_ATOMIC);
	if (hm == NULL)
		return NULL;
	ht = kmalloc((size * sizeof(struct HashBucket)), GFP_ATOMIC);
	if (ht == NULL) {
		kfree(hm);
		return NULL;
	}
	for (i = 0; i < size; i++)
		HashBucket_init(&(ht[i]));

	hm->size = size;
	hm->HashTable = ht;
	return hm;
}

bool HashMap_insert(struct HashMap *hm, unsigned long addr)
{
	unsigned long index;

	if (hm == NULL)
		return false;
	index = hash_func(addr, hm->size);
	if (hm->HashTable == NULL)
		return false;
	return HashBucket_insert(&(hm->HashTable[index]), addr);
}

bool HashMap_erase(struct HashMap *hm, unsigned long addr)
{
	unsigned long index;

	if (hm == NULL)
		return false;
	index = hash_func(addr, hm->size);
	if (hm->HashTable == NULL)
		return false;
	return HashBucket_erase(&(hm->HashTable[index]), addr);
}

void HashMap_clear(struct HashMap *hm)
{
	unsigned long size;
	struct HashBucket *ht;
	int i;

	if (hm == NULL)
		return;
	size = hm->size;
	ht = hm->HashTable;
	if (ht == NULL)
		return;
	hm->HashTable = NULL;
	for (i = 0; i < size; i++)
		HashBucket_clear(&(ht[i]));

	kfree(ht);
	kfree(hm);
}

//struct that save the information for each task
struct task_info {
	int tgid;
	char comm[TASK_COMM_LEN];
	int count;		// number of pages
	struct HashMap *pages;
	struct list_head list;
	spinlock_t cnt_lock;
};

// struct that save the information for each cgroup
struct cgroup_info {
	struct cgroup *cgrp;
	struct mem_cgroup *memcg;
	int id;
	char name[64];
	struct list_head list;
	struct list_head tasks_list;
	struct list_head oom_list;
	rwlock_t cgrp_lock;
	unsigned int cached_bytes;
};

static LIST_HEAD(all_cgroup_info);	// a list that linked all the cgroup_info struct

static struct task_info *create_task_info(struct task_struct *cur_task)
{
	struct task_info *tsk_info =
	    kmalloc(sizeof(struct task_info), GFP_ATOMIC);
	if (!tsk_info)
		return NULL;

	// initialization
	tsk_info->tgid = cur_task->tgid;
	strscpy(tsk_info->comm, cur_task->comm, sizeof(tsk_info->comm));
	tsk_info->count = 0;
	tsk_info->pages = NULL;
	tsk_info->pages = HashMap_create(1023);
	INIT_LIST_HEAD(&tsk_info->list);
	spin_lock_init(&(tsk_info->cnt_lock));

	return tsk_info;
}

static int
add_task_to_cgroup_info(struct cgroup_info *cgrp, struct task_info *task)
{
	if (!cgrp || !task)
		return -EINVAL;

	write_lock(&cgrp->cgrp_lock);
	list_add_tail(&task->list, &cgrp->tasks_list);
	write_unlock(&cgrp->cgrp_lock);
	return 0;
}

static int
remove_task_from_cgroup_info(struct cgroup_info *cgrp, struct task_info *task)
{
	if (cgrp == NULL || task == NULL)
		return -EINVAL;

	HashMap_clear(task->pages);
	// kfree(task->pages);
	kfree(task);
	return 0;
}

static struct task_info *find_task_info(struct cgroup_info *cgrp, int tgid)
{
	struct task_info *tsk_info, *pos;

	list_for_each_entry_safe(tsk_info, pos, &cgrp->tasks_list, list) {
		if (tsk_info->tgid == tgid)
			return tsk_info;
	}
	return NULL;
}

static int
remove_page_from_cgroup_info(unsigned long addr, struct cgroup_info *cgrp)
{
	struct task_info *tsk_info, *pos;

	read_lock(&cgrp->cgrp_lock);
	list_for_each_entry_safe(tsk_info, pos, &cgrp->tasks_list, list) {
		if (HashMap_erase(tsk_info->pages, addr)) {
			spin_lock(&(tsk_info->cnt_lock));
			tsk_info->count -= folio_nr_pages((struct folio *)addr);
			spin_unlock(&(tsk_info->cnt_lock));
			read_unlock(&cgrp->cgrp_lock);
			return 0;
		}
	}
	read_unlock(&cgrp->cgrp_lock);
	return -1;
}

static struct cgroup_info *create_cgroup_info(struct cgroup *cgrp,
					      struct mem_cgroup *memcg)
{
	struct cgroup_info *cgrp_info =
	    kmalloc(sizeof(struct cgroup_info), GFP_ATOMIC);
	struct kernfs_node *kn;

	if (!cgrp_info)
		return NULL;

	cgrp_info->cgrp = cgrp;
	cgrp_info->memcg = memcg;
	cgrp_info->id = (memcg->css).id;
	kn = cgrp->kn;
	strscpy(cgrp_info->name, kn->name, sizeof(cgrp_info->name));
	INIT_LIST_HEAD(&cgrp_info->list);
	INIT_LIST_HEAD(&cgrp_info->tasks_list);
	INIT_LIST_HEAD(&cgrp_info->oom_list);
	rwlock_init(&(cgrp_info->cgrp_lock));
	cgrp_info->cached_bytes = 0;

	return cgrp_info;
}

static void destroy_cgroup_info(struct cgroup_info *cgrp_info)
{
	struct task_info *task, *tmp;

	if (!cgrp_info)
		return;

	write_lock(&cgrp_info->cgrp_lock);
	list_for_each_entry_safe(task, tmp, &cgrp_info->tasks_list, list) {
		list_del(&task->list);
		remove_task_from_cgroup_info(cgrp_info, task);
	}
	write_unlock(&cgrp_info->cgrp_lock);
	list_for_each_entry_safe(task, tmp, &cgrp_info->oom_list, list) {
		list_del(&task->list);
		remove_task_from_cgroup_info(cgrp_info, task);
	}

	kfree(cgrp_info);
}

static int add_cgroup_info(struct cgroup_info *cgrp_info)
{
	if (!cgrp_info)
		return -EINVAL;

	list_add_tail(&cgrp_info->list, &all_cgroup_info);
	return 0;
}

static struct cgroup_info *find_cgroup_info(int id)
{
	struct cgroup_info *cgrp_info = NULL;

	list_for_each_entry(cgrp_info, &all_cgroup_info, list) {
		if (cgrp_info->id == id)
			return cgrp_info;
	}

	return NULL;
}

static struct task_info *create_oom_task_info(struct task_info *tsk_info)
{
	struct task_info *oom_tsk_info =
	    kmalloc(sizeof(struct task_info), GFP_ATOMIC);
	if (!oom_tsk_info)
		return NULL;

	oom_tsk_info->tgid = tsk_info->tgid;
	strscpy(oom_tsk_info->comm, tsk_info->comm, sizeof(oom_tsk_info->comm));
	oom_tsk_info->count = tsk_info->count;
	oom_tsk_info->pages = NULL;
	INIT_LIST_HEAD(&oom_tsk_info->list);

	return oom_tsk_info;
}

static int
add_oom_task_to_cgroup_info(struct cgroup_info *cgrp,
			    struct task_info *oom_task)
{
	if (!cgrp || !oom_task)
		return -EINVAL;
	list_add_tail(&oom_task->list, &cgrp->oom_list);
	return 0;
}
