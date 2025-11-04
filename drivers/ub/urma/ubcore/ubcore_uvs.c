// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * Description: Ubcore uvs management
 * Author: Huawei
 * Create: 2024-5-21
 */

#include "ubcore_log.h"
#include "ubcore_priv.h"
#include "ubcore_uvs.h"

struct ubcore_uvs_list {
	spinlock_t lock;
	struct list_head list; /* uvs instance list */
	int count; /* number of uvs instance in list */
	uint32_t next_id; /* next id for uvs */
};

static struct ubcore_uvs_list g_ubcore_uvs_instances = { 0 };

/* In the lifecyle of ubcore, ue2uvs_tables doesn't support to delete emelent.
 * Elements of g_ubcore_ue_tables will be free when unload ubcore module.
 */
static spinlock_t g_ubcore_ue_tables_lock;
static struct ubcore_ue_table *g_ubcore_ue_tables[UBCORE_MAX_MUE_NUM] = { 0 };

static inline struct ubcore_uvs_list *get_uvs_instances(void)
{
	return &g_ubcore_uvs_instances;
}

int ubcore_uvs_list_get_alive_count(void)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *ins;
	int count = 0;

	spin_lock(&instances->lock);
	list_for_each_entry(ins, &instances->list, list_node) {
		if (ins->state == UBCORE_UVS_STATE_ALIVE)
			count++;
	}
	spin_unlock(&instances->lock);

	return count;
}

struct ubcore_uvs_instance **ubcore_uvs_list_get_all_alive(int *count)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *cur;
	struct ubcore_uvs_instance **result;
	int i = 0;
	*count = 0;

	spin_lock(&instances->lock);
	if (instances->count == 0) {
		spin_unlock(&instances->lock);
		return NULL;
	}

	result = kcalloc(instances->count, sizeof(struct ubcore_uvs_instance *),
			 GFP_ATOMIC);
	if (result == NULL) {
		spin_unlock(&instances->lock);
		return NULL;
	}

	list_for_each_entry(cur, &instances->list, list_node) {
		if (cur->state != UBCORE_UVS_STATE_ALIVE)
			continue;

		result[i] = cur;
		ubcore_uvs_kref_get(cur);
		i++;
	}
	spin_unlock(&instances->lock);

	*count = i;
	return result;
}

void ubcore_uvs_list_put(struct ubcore_uvs_instance **uvs_list, int count)
{
	int i;

	if (uvs_list == NULL || count == 0)
		return;

	for (i = 0; i < count; i++)
		ubcore_uvs_kref_put(uvs_list[i]);

	kfree(uvs_list);
}

void ubcore_uvs_list_init(void)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();

	spin_lock_init(&instances->lock);
	INIT_LIST_HEAD(&instances->list);
	instances->count = 0;
	/* 0 for invalid uvs id */
	instances->next_id = 1;
}

void ubcore_uvs_list_uninit(void)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *ins, *tmp;

	spin_lock(&instances->lock);
	list_for_each_entry_safe(ins, tmp, &instances->list, list_node) {
		list_del(&ins->list_node);
		ubcore_uvs_kref_put(ins);
	}
	instances->count = 0;
	instances->next_id = 0;
	spin_unlock(&instances->lock);
}

static inline int ubcore_uvs_cmp(const struct ubcore_uvs_instance *a,
				 const struct ubcore_uvs_instance *b)
{
	return (a->id == b->id && strcmp(a->name, b->name) == 0) ? 0 : -1;
}

/* used in single UVS scenario */
struct ubcore_uvs_instance *ubcore_get_default_uvs(void)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *uvs = NULL;

	spin_lock(&instances->lock);
	if (!list_empty(&instances->list)) {
		uvs = list_first_entry(&instances->list,
				       typeof(struct ubcore_uvs_instance),
				       list_node);
		ubcore_uvs_kref_get(uvs);
	}

	spin_unlock(&instances->lock);

	return uvs;
}

void ubcore_ue2uvs_tables_init(void)
{
	int i;

	for (i = 0; i < UBCORE_MAX_MUE_NUM; i++)
		g_ubcore_ue_tables[i] = NULL;

	spin_lock_init(&g_ubcore_ue_tables_lock);
}

/* this is called when ubcore module is unloading */
void ubcore_ue2uvs_tables_uninit(void)
{
	struct ubcore_ue_table **tables = g_ubcore_ue_tables;
	int i;

	for (i = 0; i < UBCORE_MAX_MUE_NUM; i++) {
		if (tables[i] == NULL)
			continue;

		ubcore_log_info("delete ue2uvs table %d for mue %s\n", i,
				tables[i]->mue_name);
		kfree(tables[i]);
		tables[i] = NULL;
	}
}

static int ue2uvs_tables_find_nolock(const char *mue_name)
{
	struct ubcore_ue_table **tables = g_ubcore_ue_tables;
	int i;

	for (i = 0; i < UBCORE_MAX_MUE_NUM; i++) {
		if (tables[i] == NULL)
			continue;

		if (strcmp(mue_name, tables[i]->mue_name) == 0)
			break;
	}

	return i;
}

static int ue2uvs_tables_find_first_unused_nolock(void)
{
	struct ubcore_ue_table **tables = g_ubcore_ue_tables;
	int i;

	for (i = 0; i < UBCORE_MAX_MUE_NUM; i++) {
		if (tables[i] == NULL)
			break;
	}

	return i;
}

/* Fetch one ue2uvs table for mue, if the table doesn't exist, then create a new one. */
struct ubcore_ue_table *ubcore_ue2uvs_fetch(const char *mue_name)
{
	struct ubcore_ue_table **tables = g_ubcore_ue_tables;
	struct ubcore_ue_table *result = NULL;
	int idx;

	spin_lock(&g_ubcore_ue_tables_lock);
	idx = ue2uvs_tables_find_nolock(mue_name);
	if (idx >= UBCORE_MAX_MUE_NUM) {
		/* select one free slot to use when no reusable slot found */
		idx = ue2uvs_tables_find_first_unused_nolock();
		if (idx == UBCORE_MAX_MUE_NUM) {
			spin_unlock(&g_ubcore_ue_tables_lock);
			ubcore_log_err(
				"number of ue2uvs table slot reaches the max %d\n",
				UBCORE_MAX_MUE_NUM);
			return result;
		}

		/* found one free slot */
		ubcore_log_info(
			"found a free slot %d to create new ue2uvs table for %s\n",
			idx, mue_name);
	}

	if (tables[idx] == NULL) {
		tables[idx] =
			kzalloc(sizeof(struct ubcore_ue_table), GFP_ATOMIC);
		if (tables[idx] == NULL) {
			spin_unlock(&g_ubcore_ue_tables_lock);
			return result;
		}

		(void)strscpy(tables[idx]->mue_name, mue_name, UBCORE_MAX_DEV_NAME);
		spin_lock_init(&tables[idx]->ue2uvs_lock);
		ubcore_log_info(
			"create a new ue2uvs table at slot %d for mue %s\n",
			idx, mue_name);
	} else
		ubcore_log_info("reuse ue2uvs table slot %d for mue %s\n", idx,
				mue_name);

	result = tables[idx];
	spin_unlock(&g_ubcore_ue_tables_lock);

	return result;
}

bool ubcore_check_ue2uvs_mapping(struct ubcore_device *dev, uint32_t ue_idx,
				 uint32_t target_uvs_id)
{
	struct ubcore_uvs_instance *uvs;
	bool result = false;

	uvs = ubcore_find_get_uvs_by_ue(dev, ue_idx);
	if (uvs == NULL)
		return result;

	if (uvs->id == target_uvs_id)
		result = true;

	ubcore_uvs_kref_put(uvs);
	return result;
}

struct ubcore_uvs_instance *ubcore_find_get_uvs_by_ue(struct ubcore_device *dev,
						      uint32_t ue_idx)
{
	struct ubcore_ue_table *ue2uvs_table;
	struct ubcore_uvs_instance *uvs;

	if (!dev->attr.tp_maintainer) {
		ubcore_log_err(
			"try to find uvs in non-mue device %s, ue_idx %u\n",
			dev->dev_name, ue_idx);
		return NULL;
	}

	/* use the first uvs when transport type is not UB */
	if (dev->transport_type != UBCORE_TRANSPORT_UB)
		return ubcore_get_default_uvs();

	if (ue_idx >= UBCORE_MAX_UE_CNT) {
		ubcore_log_err("invalid ue_idx %u, valid range [0, %u)\n",
			       ue_idx, UBCORE_MAX_UE_CNT);
		return NULL;
	}

	ue2uvs_table = ubcore_ue2uvs_fetch(dev->dev_name);
	if (ue2uvs_table == NULL) {
		ubcore_log_err(
			"ue2uvs tables is full, can't create new ue2table\n");
		return NULL;
	}

	spin_lock(&ue2uvs_table->ue2uvs_lock);
	uvs = ue2uvs_table->ue_entries[ue_idx].uvs_inst;
	if (uvs != NULL)
		ubcore_uvs_kref_get(uvs);
	else
		ubcore_log_err("Fail to find uvs, ue_idx is %u.\n", ue_idx);
	spin_unlock(&ue2uvs_table->ue2uvs_lock);

	return uvs;
}

static int ubcore_unset_ue2uvs_mapping(struct ubcore_ue_table *ue_table,
				       uint32_t ue_idx)
{
	struct ubcore_ue_entry *entries = ue_table->ue_entries;
	struct ubcore_uvs_instance *old_uvs;

	spin_lock(&ue_table->ue2uvs_lock);
	old_uvs = entries[ue_idx].uvs_inst;
	if (old_uvs == NULL) {
		spin_unlock(&ue_table->ue2uvs_lock);
		return 0;
	}

	/* unset mapping */
	atomic_dec(&old_uvs->map2ue);
	entries[ue_idx].uvs_inst = NULL;
	bitmap_zero(entries[ue_idx].eid_bitmap, UBCORE_MAX_EID_CNT);
	spin_unlock(&ue_table->ue2uvs_lock);

	ubcore_log_info(
		"remove ue2uvs mapping, ue_idx %u, uvs_name %s, uvs_id %u\n",
		ue_idx, old_uvs->name, old_uvs->id);
	ubcore_uvs_kref_put(old_uvs);
	return 0;
}

static int
ubcore_set_ue2uvs_mapping_internal(struct ubcore_ue_table *ue2uvs_table,
				   uint32_t ue_idx, const char *uvs_name)
{
	struct ubcore_ue_entry *entries = ue2uvs_table->ue_entries;
	struct ubcore_uvs_instance *uvs, *old_uvs;

	uvs = ubcore_uvs_lookup_get(uvs_name);
	if (uvs == NULL) {
		ubcore_log_err("uvs %s doesn't exist\n", uvs_name);
		return -ENOENT;
	}

	spin_lock(&ue2uvs_table->ue2uvs_lock);
	old_uvs = entries[ue_idx].uvs_inst;
	if (old_uvs == NULL) {
		entries[ue_idx].uvs_inst = uvs;
		atomic_inc(&uvs->map2ue);
		spin_unlock(&ue2uvs_table->ue2uvs_lock);
		ubcore_log_info(
			"add new ue2uvs mapping, ue_idx %u, uvs_name %s, uvs_id %u\n",
			ue_idx, uvs_name, uvs->id);
		return 0;
	}

	/* old uvs is same with the target uvs, checking if we can reuse it */
	if (ubcore_uvs_cmp(old_uvs, uvs) == 0) {
		if (old_uvs->state == UBCORE_UVS_STATE_ALIVE) {
			ubcore_log_info(
				"reuse ue2uvs mapping ue_idx %u, uvs %s, id %u\n",
				ue_idx, uvs_name, old_uvs->id);
			spin_unlock(&ue2uvs_table->ue2uvs_lock);
			ubcore_uvs_kref_put(uvs);
			return 0;
		}

		spin_unlock(&ue2uvs_table->ue2uvs_lock);
		ubcore_log_err(
			"uvs state is not alive when set ue2uvs mapping, ue_idx %u, uvs %s\n",
			ue_idx, uvs_name);
		ubcore_uvs_kref_put(uvs);
		return -EPERM;
	}

	/* old uvs and target uvs are different, checking state of old uvs */
	if (old_uvs->state == UBCORE_UVS_STATE_ALIVE) {
		ubcore_log_err(
			"ue_idx %u is already mapped to uvs %s, uvs_id %u\n",
			ue_idx, old_uvs->name, old_uvs->id);
		spin_unlock(&ue2uvs_table->ue2uvs_lock);
		ubcore_uvs_kref_put(uvs);
		return -EEXIST;
	}

	/* old uvs is dead, replace it */
	entries[ue_idx].uvs_inst = uvs;
	atomic_inc(&uvs->map2ue);
	atomic_dec(&old_uvs->map2ue);
	spin_unlock(&ue2uvs_table->ue2uvs_lock);
	ubcore_log_info(
		"replace mapping ue_idx %u, old_uvs %s, old_id %u with new_uvs %s, new_id %u",
		ue_idx, old_uvs->name, old_uvs->id, uvs->name, uvs->id);
	ubcore_uvs_kref_put(old_uvs);
	return 0;
}

int ubcore_set_ue2uvs_mapping(struct ubcore_device *dev, uint32_t ue_idx,
			      const char *uvs_name)
{
	struct ubcore_ue_table *ue2uvs_table;

	if (ue_idx >= UBCORE_MAX_UE_CNT) {
		ubcore_log_err("invalid ue_idx %u, valid range [0, %u)\n",
			       ue_idx, UBCORE_MAX_UE_CNT);
		return -EINVAL;
	}

	ue2uvs_table = ubcore_ue2uvs_fetch(dev->dev_name);
	if (ue2uvs_table == NULL) {
		ubcore_log_err(
			"ue2uvs tables is full, can't create new ue2table.\n");
		return -ENOSPC;
	}

	/* remove the mapping when uvs_name is an empty string */
	if (strlen(uvs_name) == 0)
		return ubcore_unset_ue2uvs_mapping(ue2uvs_table, ue_idx);

	return ubcore_set_ue2uvs_mapping_internal(ue2uvs_table, ue_idx,
						  uvs_name);
}

struct ubcore_uvs_instance *ubcore_uvs_find_get_by_genl_port(uint32_t genl_port)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *result = NULL;
	struct ubcore_uvs_instance *ins;

	spin_lock(&instances->lock);
	list_for_each_entry(ins, &instances->list, list_node) {
		if (ins->genl_port == genl_port) {
			result = ins;
			ubcore_uvs_kref_get(result);
			break;
		}
	}
	spin_unlock(&instances->lock);

	return result;
}

struct ubcore_uvs_instance *ubcore_uvs_lookup_get(const char *uvs_name)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *result = NULL;
	struct ubcore_uvs_instance *ins;

	spin_lock(&instances->lock);
	list_for_each_entry(ins, &instances->list, list_node) {
		if (strcmp(ins->name, uvs_name) == 0) {
			result = ins;
			ubcore_uvs_kref_get(result);
			break;
		}
	}
	spin_unlock(&instances->lock);

	return result;
}

int ubcore_uvs_set_genl_info(const char *uvs_name, uint32_t genl_port,
			     struct sock *genl_sock)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *cur, *tmp;
	int ret = -ENODATA;

	spin_lock(&instances->lock);
	list_for_each_entry_safe(cur, tmp, &instances->list, list_node) {
		if (strcmp(cur->name, uvs_name) == 0) {
			cur->genl_port = genl_port;
			cur->genl_sock = genl_sock;
			atomic_set(&cur->nl_wait_buffer, 0);
			ret = 0;
			break;
		}
	}
	if (ret == 0)
		ubcore_log_info(
			"successfully set genl info for uvs %s, uvs_id %u, genl_port %u\n",
			cur->name, cur->id, cur->genl_port);
	spin_unlock(&instances->lock);

	return ret;
}

struct ubcore_uvs_instance *ubcore_find_get_uvs_by_pid(uint32_t pid)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *cur, *tmp;

	spin_lock(&instances->lock);
	list_for_each_entry_safe(cur, tmp, &instances->list, list_node) {
		if (cur->pid == pid) {
			ubcore_uvs_kref_get(cur);
			spin_unlock(&instances->lock);
			ubcore_log_info("find uvs %s by pid %u\n", cur->name,
					pid);
			return cur;
		}
	}
	ubcore_log_err("can't find uvs by pid %u\n", pid);
	spin_unlock(&instances->lock);
	return NULL;
}

int ubcore_uvs_add(const char *uvs_name, uint32_t policy)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *new_uvs, *cur, *tmp;
	bool reuse_uvs = false;

	uint32_t pid;

	pid = (uint32_t)task_tgid_vnr(current);

	new_uvs = kzalloc(sizeof(struct ubcore_uvs_instance), GFP_ATOMIC);
	if (new_uvs == NULL)
		return -ENOMEM;

	spin_lock(&instances->lock);
	if (instances->count == UBCORE_MAX_UVS_CNT) {
		ubcore_log_err("number of uvs reaches the max %d\n",
			       UBCORE_MAX_UVS_CNT);
		spin_unlock(&instances->lock);
		kfree(new_uvs);
		return -ENOSPC;
	}

	list_for_each_entry_safe(cur, tmp, &instances->list, list_node) {
		if (strcmp(cur->name, uvs_name) != 0)
			continue;

		/* we have one old uvs instance which is alive */
		if (cur->state == UBCORE_UVS_STATE_ALIVE) {
			spin_unlock(&instances->lock);
			ubcore_log_err(
				"there is already one running uvs with name %s\n",
				uvs_name);
			kfree(new_uvs);
			return -EEXIST;
		}

		/* the old uvs instance is not alive, reuse it and active it */
		reuse_uvs = true;
		cur->state = UBCORE_UVS_STATE_ALIVE;
		cur->pid = pid;
		ubcore_log_info("add uvs %s, pid %u\n", uvs_name, pid);
		goto add_out;
	}

	(void)strscpy(new_uvs->name, uvs_name, UBCORE_MAX_DEV_NAME);
	kref_init(&new_uvs->ref);
	spin_lock_init(&new_uvs->sip_list_lock);
	INIT_LIST_HEAD(&new_uvs->sip_list);
	new_uvs->pid = pid;
	ubcore_log_info("add uvs %s, pid %u\n", uvs_name, pid);
	new_uvs->id = instances->next_id;
	new_uvs->policy = policy;
	new_uvs->state = UBCORE_UVS_STATE_ALIVE;
	atomic_set(&new_uvs->map2ue, 0);

	list_add_tail(&new_uvs->list_node, &instances->list);
	instances->count++;
	instances->next_id++;

add_out:
	if (reuse_uvs) {
		kfree(new_uvs);
		ubcore_log_info("reuse uvs instance %s with id %u, policy %u\n",
				cur->name, cur->id, cur->policy);
	} else
		ubcore_log_info(
			"add uvs instance %s with id %u, policy %u done\n",
			new_uvs->name, new_uvs->id, new_uvs->policy);
	spin_unlock(&instances->lock);
	return 0;
}

int ubcore_uvs_remove(const char *uvs_name)
{
	struct ubcore_uvs_list *instances = get_uvs_instances();
	struct ubcore_uvs_instance *ins, *tmp;
	int ret = -ENODATA;

	spin_lock(&instances->lock);
	list_for_each_entry_safe(ins, tmp, &instances->list, list_node) {
		if (strcmp(ins->name, uvs_name) != 0)
			continue;

		if (ins->state == UBCORE_UVS_STATE_DEAD) {
			ubcore_log_warn("uvs %s is already set dead.\n",
					uvs_name);
			ret = -EPERM;
			break;
		}

		/* Uvs was referenced by ue, can't remove uvs from list. */
		if (atomic_read(&ins->map2ue) != 0) {
			ins->state = UBCORE_UVS_STATE_DEAD;
			ubcore_log_info(
				"uvs %s was referenced by ue, so set dead and keep it.\n",
				uvs_name);
			spin_unlock(&instances->lock);
			return 0;
		}

		/* Uvs was not referenced by fe, it can be remove from the list. */
		list_del(&ins->list_node);
		ins->state = UBCORE_UVS_STATE_DEAD;
		instances->count--;
		ubcore_uvs_kref_put(ins);
		ret = 0;
		break;
	}
	spin_unlock(&instances->lock);

	if (ret == 0)
		ubcore_log_info("uvs instance %s was successfully removed\n",
				uvs_name);

	return ret;
}

static int ubcore_update_set_eid_idx(struct ubcore_device *dev, uint32_t ue_idx,
				     uint32_t eid_idx, bool is_set)
{
	struct ubcore_ue_table *ue_table;
	struct ubcore_ue_entry *ue;

	if (ue_idx >= UBCORE_MAX_UE_CNT) {
		ubcore_log_err("Invalid ue_idx:%u\n", ue_idx);
		return -EINVAL;
	}

	ue_table = ubcore_ue2uvs_fetch(dev->dev_name);
	if (ue_table == NULL) {
		ubcore_log_err("Failed to get ue_table mue_name%s.\n",
			       dev->dev_name);
		return -ENOSPC;
	}

	spin_lock(&ue_table->ue2uvs_lock);
	ue = &ue_table->ue_entries[ue_idx];

	if (is_set)
		set_bit(eid_idx, ue->eid_bitmap);
	else
		clear_bit(eid_idx, ue->eid_bitmap);
	spin_unlock(&ue_table->ue2uvs_lock);
	return 0;
}

int ubcore_ue_set_eid_idx(struct ubcore_device *dev, uint32_t ue_idx,
			  uint32_t eid_idx)
{
	return ubcore_update_set_eid_idx(dev, ue_idx, eid_idx, true);
}

int ubcore_ue_clear_eid_idx(struct ubcore_device *dev, uint32_t ue_idx,
			    uint32_t eid_idx)
{
	return ubcore_update_set_eid_idx(dev, ue_idx, eid_idx, false);
}

int ubcore_get_eid_use_cnt(struct ubcore_device *dev, uint32_t ue_idx,
			   uint32_t *eid_use_cnt)
{
	struct ubcore_ue_table *ue_table;
	struct ubcore_ue_entry *ue;

	if (ue_idx >= UBCORE_MAX_UE_CNT) {
		ubcore_log_err("Invalid ue_idx:%u\n", ue_idx);
		return -EINVAL;
	}

	ue_table = ubcore_ue2uvs_fetch(dev->dev_name);
	if (ue_table == NULL) {
		ubcore_log_err("Failed to get ue_table mue_name%s.\n",
			       dev->dev_name);
		return -ENOSPC;
	}

	spin_lock(&ue_table->ue2uvs_lock);
	ue = &ue_table->ue_entries[ue_idx];
	*eid_use_cnt = bitmap_weight(ue->eid_bitmap, UBCORE_MAX_EID_CNT);
	spin_unlock(&ue_table->ue2uvs_lock);
	return 0;
}

static void ubcore_uvs_kref_release(struct kref *ref)
{
	struct ubcore_uvs_instance *uvs =
		container_of(ref, struct ubcore_uvs_instance, ref);

	ubcore_log_info("release uvs %s, uvs_id %u\n", uvs->name, uvs->id);
	kfree(uvs);
}

void ubcore_uvs_kref_put(struct ubcore_uvs_instance *uvs)
{
	uint32_t refcnt;

	if (uvs == NULL)
		return;
	refcnt = kref_read(&uvs->ref);
	ubcore_log_debug(
		"kref_put: uvs %s, id %u, old refcnt %u, new refcnt %u\n",
		uvs->name, uvs->id, refcnt, refcnt > 0 ? refcnt - 1 : 0);

	(void)kref_put(&uvs->ref, ubcore_uvs_kref_release);
}

void ubcore_uvs_kref_get(struct ubcore_uvs_instance *uvs)
{
	kref_get(&uvs->ref);
	ubcore_log_debug("kref_get: uvs %s, id %u, refcnt %u\n", uvs->name,
			 uvs->id, kref_read(&uvs->ref));
}
