/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef UBCORE_UVS_H
#define UBCORE_UVS_H

#include <linux/types.h>

#include <ub/urma/ubcore_types.h>
#include "ubcore_priv.h"

int ubcore_uvs_list_get_alive_count(void);
/* call ubcore_uvs_list_put to free list memory */
struct ubcore_uvs_instance **ubcore_uvs_list_get_all_alive(int *count);
void ubcore_uvs_list_put(struct ubcore_uvs_instance **uvs_list, int count);
void ubcore_uvs_list_init(void);
void ubcore_uvs_list_uninit(void);

/* whether the ue is mapped to the target uvs */
bool ubcore_check_ue2uvs_mapping(struct ubcore_device *dev, uint32_t ue_idx,
				 uint32_t target_uvs_id);
struct ubcore_uvs_instance *ubcore_find_get_uvs_by_ue(struct ubcore_device *dev,
						      uint32_t ue_idx);
struct ubcore_uvs_instance *ubcore_find_get_uvs_by_pid(uint32_t pid);
int ubcore_set_ue2uvs_mapping(struct ubcore_device *dev, uint32_t ue_idx,
			      const char *uvs_name);

void ubcore_ue2uvs_tables_init(void);
void ubcore_ue2uvs_tables_uninit(void);
struct ubcore_ue_table *ubcore_ue2uvs_fetch(const char *mue_name);

int ubcore_uvs_add(const char *uvs_name, uint32_t policy);
int ubcore_uvs_remove(const char *uvs_name);

int ubcore_ue_set_eid_idx(struct ubcore_device *dev, uint32_t ue_idx,
			  uint32_t eid_idx);
int ubcore_ue_clear_eid_idx(struct ubcore_device *dev, uint32_t ue_idx,
			    uint32_t eid_idx);
int ubcore_get_eid_use_cnt(struct ubcore_device *dev, uint32_t ue_idx,
			   uint32_t *eid_use_cnt);

int ubcore_uvs_set_genl_info(const char *uvs_name, uint32_t genl_port,
			     struct sock *genl_sock);
struct ubcore_uvs_instance *
ubcore_uvs_find_get_by_genl_port(uint32_t genl_port);
struct ubcore_uvs_instance *ubcore_uvs_lookup_get(const char *uvs_name);
struct ubcore_uvs_instance *ubcore_get_default_uvs(void);

void ubcore_uvs_kref_put(struct ubcore_uvs_instance *uvs);
void ubcore_uvs_kref_get(struct ubcore_uvs_instance *uvs);

#endif
