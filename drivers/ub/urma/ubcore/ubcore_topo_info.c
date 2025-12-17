// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * Description: ubcore topo info file
 * Author: Liu Jiajun
 * Create: 2025-07-03
 * Note:
 * History: 2025-07-03 Create file
 */

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <ub/urma/ubcore_types.h>
#include <ub/urma/ubcore_uapi.h>
#include "ubcore_log.h"
#include "ubcore_topo_info.h"

static struct ubcore_topo_map *g_ubcore_topo_map;

struct ubcore_topo_map *
ubcore_create_global_topo_map(struct ubcore_topo_node *topo_infos,
			      uint32_t node_num)
{
	g_ubcore_topo_map =
		ubcore_create_topo_map_from_user(topo_infos, node_num);
	return g_ubcore_topo_map;
}

void ubcore_delete_global_topo_map(void)
{
	if (!g_ubcore_topo_map)
		return;
	ubcore_delete_topo_map(g_ubcore_topo_map);
	g_ubcore_topo_map = NULL;
}

struct ubcore_topo_map *ubcore_get_global_topo_map(void)
{
	return g_ubcore_topo_map;
}

struct ubcore_topo_map *
ubcore_create_topo_map_from_user(struct ubcore_topo_node *user_topo_infos,
				 uint32_t node_num)
{
	struct ubcore_topo_map *topo_map = NULL;
	int ret = 0;

	if (!user_topo_infos || node_num <= 0 ||
	    node_num > MAX_NODE_NUM) {
		ubcore_log_err("Invalid param\n");
		return NULL;
	}
	topo_map = kzalloc(sizeof(struct ubcore_topo_map), GFP_KERNEL);
	if (!topo_map)
		return NULL;
	ret = copy_from_user(topo_map->topo_infos,
			     (void __user *)user_topo_infos,
			     sizeof(struct ubcore_topo_node) * node_num);
	if (ret != 0) {
		ubcore_log_err("Failed to copy topo infos\n");
		kfree(topo_map);
		return NULL;
	}
	topo_map->node_num = node_num;
	return topo_map;
}

void ubcore_delete_topo_map(struct ubcore_topo_map *topo_map)
{
	if (!topo_map)
		return;
	kfree(topo_map);
}

bool is_aggr_dev_valid(struct ubcore_topo_aggr_dev *aggr_dev)
{
	struct ubcore_topo_aggr_dev empty_dev = {0};

	return (memcmp(aggr_dev, &empty_dev, sizeof(struct ubcore_topo_aggr_dev)) == 0) ? false : true;
}

bool is_eid_valid(const char *eid)
{
	int i;

	for (i = 0; i < EID_LEN; i++) {
		if (eid[i] != 0)
			return true;
	}
	return false;
}

bool is_aggr_and_primary_eid_valid(struct ubcore_topo_map *topo_map)
{
	int i, j, k;
	bool has_primary_eid = false;

	for (i = 0; i < topo_map->node_num; i++) {
		for (j = 0; j < DEV_NUM; j++) {
			if (!is_eid_valid(topo_map->topo_infos[i].devs[j].aggr_eid))
				return false;
			has_primary_eid = false;
			for (k = 0; k < IODIE_NUM; k++) {
				if (is_eid_valid(topo_map->topo_infos[i].devs[j].fe[k].primary_eid))
					has_primary_eid = true;
			}
			if (!has_primary_eid)
				return false;
		}
	}
	return true;
}

static int find_cur_node_index(struct ubcore_topo_map *topo_map,
			       uint32_t *node_index)
{
	int i;

	for (i = 0; i < topo_map->node_num; i++) {
		if (topo_map->topo_infos[i].is_current) {
			*node_index = i;
			break;
		}
	}
	if (i == topo_map->node_num) {
		ubcore_log_err("can't find cur node\n");
		return -EINVAL;
	}
	return 0;
}

static bool is_eid_match(const char *eid1, const char *eid2)
{
	return memcmp(eid1, eid2, EID_LEN) == 0;
}

struct ubcore_topo_node *
ubcore_get_cur_topo_info(struct ubcore_topo_map *topo_map)
{
	uint32_t cur_node_index = 0;

	if (find_cur_node_index(topo_map, &cur_node_index) != 0) {
		ubcore_log_err("find cur node index failed\n");
		return NULL;
	}
	return &(topo_map->topo_infos[cur_node_index]);
}

static void update_dev_info(struct ubcore_topo_node *new_topo_info,
				struct ubcore_topo_node *old_topo_info)
{
	int dev_id;

	for (dev_id = 0; dev_id < DEV_NUM; dev_id++) {
		if (is_aggr_dev_valid(&old_topo_info->devs[dev_id])) {
			(void)memcpy(&old_topo_info->devs[dev_id], &new_topo_info->devs[dev_id],
				sizeof(struct ubcore_topo_aggr_dev));
		}
		// TODO: update eid
	}
}

static int update_link_info(struct ubcore_topo_node *new_topo_info,
				struct ubcore_topo_node *old_topo_info)
{
	int iodie_id, port_id, old_remote_port_id;

	for (iodie_id = 0; iodie_id < IODIE_NUM; iodie_id++) {
		for (port_id = 0; port_id < PORT_NUM; ++port_id) {
			// add new connection if no connection exists
			old_remote_port_id = old_topo_info->links[iodie_id][port_id].peer_port;
			if (old_remote_port_id == UINT32_MAX) {
				(void)memcpy(&old_topo_info->links[iodie_id][port_id],
					&new_topo_info->links[iodie_id][port_id], sizeof(struct ubcore_topo_link));
			} else {
				if (memcmp(&old_topo_info->links[iodie_id][port_id], &new_topo_info->links[iodie_id][port_id],
					sizeof(struct ubcore_topo_link)) != 0) {
					ubcore_log_err("link is not the same, new: peer_node[%u]/peer_iodie[%u]/peer_port[%u],\
						old: peer_node[%u]/peer_iodie[%u]/peer_port[%u]\n",
						new_topo_info->links[iodie_id][port_id].peer_node,
						new_topo_info->links[iodie_id][port_id].peer_iodie,
						new_topo_info->links[iodie_id][port_id].peer_port,
						old_topo_info->links[iodie_id][port_id].peer_node,
						old_topo_info->links[iodie_id][port_id].peer_iodie,
						old_topo_info->links[iodie_id][port_id].peer_port);
					return -EINVAL;
				}
			}
		}
	}
	return 0;
}

// only update current node
int ubcore_update_topo_map(struct ubcore_topo_map *new_topo_map,
			   struct ubcore_topo_map *old_topo_map)
{
	struct ubcore_topo_node *new_cur_node_info;
	struct ubcore_topo_node *old_cur_node_info;
	uint32_t new_cur_node_index = 0;
	uint32_t old_cur_node_index = 0;

	if (!new_topo_map || !old_topo_map) {
		ubcore_log_err("Invalid topo map\n");
		return -EINVAL;
	}
	if (!is_aggr_and_primary_eid_valid(new_topo_map)) {
		ubcore_log_err("Invalid primary eid\n");
		return -EINVAL;
	}
	if (find_cur_node_index(new_topo_map, &new_cur_node_index) != 0) {
		ubcore_log_err("find cur node index failed in new topo map\n");
		return -EINVAL;
	}
	new_cur_node_info = &(new_topo_map->topo_infos[new_cur_node_index]);
	if (find_cur_node_index(old_topo_map, &old_cur_node_index) != 0) {
		ubcore_log_err("find cur node index failed in old topo map\n");
		return -EINVAL;
	}
	old_cur_node_info = &(old_topo_map->topo_infos[old_cur_node_index]);

	// 暂时先只更新拓扑连接关系，后续考虑更新eid
	// TODO更新拓扑逻辑：只增不改
	update_dev_info(new_cur_node_info, old_cur_node_info);
	return update_link_info(new_cur_node_info, old_cur_node_info);
}

void ubcore_show_topo_map(struct ubcore_topo_map *topo_map)
{
    int node_idx, dev_idx, die_idx, port_idx;
    struct ubcore_topo_node *node;

    if (!topo_map) {
        ubcore_log_err("topo_map is NULL\n");
        return;
    }

    ubcore_log_info(
        "========================== topo map start =============================\n");
    for (node_idx = 0; node_idx < topo_map->node_num; node_idx++) {
        node = &topo_map->topo_infos[node_idx];

        ubcore_log_info(
            "===================== node %d start =======================\n",
            node_idx);

        /* print link table for this node */
        for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
			for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
				ubcore_log_info("link[iodie_idx:%d][port_idx:%d] -> peer_node: %u, peer_iodie: %u, peer_port: %u\n",
                    die_idx,
					port_idx,
                    node->links[die_idx][port_idx].peer_node,
					node->links[die_idx][port_idx].peer_iodie,
                    node->links[die_idx][port_idx].peer_port);
			}
        }

        /* print device list (only devices with valid aggr_eid) */
        for (dev_idx = 0; dev_idx < DEV_NUM; dev_idx++) {
            if (!is_eid_valid(node->devs[dev_idx].aggr_eid))
                continue;

            ubcore_log_info("---- dev %d aggr_eid: " EID_FMT "\n", dev_idx,
                    EID_RAW_ARGS(node->devs[dev_idx].aggr_eid));

            for (die_idx = 0; die_idx < IODIE_NUM; die_idx++) {
                ubcore_log_info("------ socket_id[%d]: %u\n", die_idx,
                        node->devs[dev_idx].fe[die_idx].socket_id);

                ubcore_log_info("------ primary_eid[%d]: " EID_FMT "\n", die_idx,
                        EID_RAW_ARGS(node->devs[dev_idx].fe[die_idx].primary_eid));

                for (port_idx = 0; port_idx < PORT_NUM; port_idx++) {
                    ubcore_log_info("-------- port_eid[%d][%d]: " EID_FMT "\n",
                            die_idx, port_idx,
                            EID_RAW_ARGS(node->devs[dev_idx].fe[die_idx].port_eid[port_idx]));
                }
            }
        }

        ubcore_log_info(
            "===================== node %d end =======================\n",
            node_idx);
    }
    ubcore_log_info(
        "========================== topo map end =============================\n");
}

int ubcore_get_primary_eid(union ubcore_eid *eid, union ubcore_eid *primary_eid)
{
	int node_id, dev_id, iodie_id, port_id;
	struct ubcore_topo_node *cur_node_info;

	if (!g_ubcore_topo_map) {
		ubcore_log_info(
			"ubcore topo map doesn't exist, eid is primary_eid.\n");
		(void)memcpy(primary_eid, eid, EID_LEN);
		return 0;
	}

	for (node_id = 0; node_id < g_ubcore_topo_map->node_num; node_id++) {
		cur_node_info = g_ubcore_topo_map->topo_infos + node_id;
		for (dev_id = 0; dev_id < DEV_NUM; dev_id++) {
			if (!is_eid_match(cur_node_info->devs[dev_id].aggr_eid,
				 (char *)eid->raw)) {
				ubcore_log_err("failed to get primary eid using aggr eid\n");
				return -EINVAL;
			}

			for (iodie_id = 0; iodie_id < IODIE_NUM; iodie_id++) {
				if (is_eid_match(
						cur_node_info->devs[dev_id].fe[iodie_id].primary_eid,
						(char *)eid->raw)) {
					(void)memcpy(primary_eid,
							cur_node_info->devs[dev_id].fe[iodie_id].primary_eid,
							EID_LEN);
					ubcore_log_info("find primary eid\n");
					return 0;
				}
				for (port_id = 0; port_id < MAX_PORT_NUM; port_id++) {
					if (is_eid_match(cur_node_info->devs[dev_id].fe[iodie_id].port_eid[port_id],
							(char *)eid->raw)) {
						(void)memcpy(primary_eid,
								cur_node_info->devs[dev_id].fe[iodie_id].primary_eid,
								EID_LEN);
						ubcore_log_info(
							"find primary eid by port eid\n");
						return 0;
					}
				}
			}
		}
	}
	ubcore_log_err("can't find primary eid\n");
	return -EINVAL;
}

static struct ubcore_topo_node *
	ubcore_get_topo_info_by_aggr_eid(union ubcore_eid *aggr_eid, int *device_id)
{
	struct ubcore_topo_map *topo_map;
	int node_id, dev_id;

	topo_map = g_ubcore_topo_map;
	for (node_id = 0; node_id < topo_map->node_num; node_id++) {
		for (dev_id = 0; dev_id < DEV_NUM; dev_id++) {
			if (memcmp(aggr_eid, topo_map->topo_infos[node_id].devs[dev_id].aggr_eid,
				sizeof(*aggr_eid)) == 0) {
					*device_id = dev_id;
					return &topo_map->topo_infos[node_id];
				}
		}
	}

	ubcore_log_err(
		"Failed to get topo info, aggr_eid: "EID_FMT".\n",
		EID_ARGS(*aggr_eid));
	return NULL;
}

static int ubcore_get_route_port_eid(union ubcore_eid *src_v_eid,
	union ubcore_eid *dst_v_eid, struct ubcore_route_list *route_list)
{
	int src_dev_id, dst_dev_id, iodie_id, port_id, remote_port_id;
	struct ubcore_topo_aggr_dev *src_aggr_dev = NULL;
	struct ubcore_topo_aggr_dev *dst_aggr_dev = NULL;
	struct ubcore_topo_node *src_topo_info = NULL;
	struct ubcore_topo_node *dst_topo_info = NULL;
	uint32_t num = route_list->route_num;

	src_topo_info = ubcore_get_topo_info_by_aggr_eid(src_v_eid, &src_dev_id);
	if (IS_ERR_OR_NULL(src_topo_info)) {
		ubcore_log_err("Failed to get src_topo_info.\n");
		return -EINVAL;
	}
	src_aggr_dev = &src_topo_info->devs[src_dev_id];

	dst_topo_info = ubcore_get_topo_info_by_aggr_eid(dst_v_eid, &dst_dev_id);
	if (IS_ERR_OR_NULL(dst_topo_info)) {
		ubcore_log_err("Failed to get dst_topo_info.\n");
		return -EINVAL;
	}
	dst_aggr_dev = &dst_topo_info->devs[dst_dev_id];

	for (iodie_id = 0; iodie_id < IODIE_NUM; iodie_id++) {
		for (port_id = 0; port_id < MAX_PORT_NUM; port_id++) {
			if (num >= UBCORE_MAX_ROUTE_NUM) {
				ubcore_log_warn("Invalid route num.\n");
				return -EINVAL;
			}
			if (!is_eid_valid(src_aggr_dev->fe[iodie_id].port_eid[port_id]) ||
				src_topo_info->links[iodie_id][port_id].peer_port == UINT32_MAX) {
				continue;
			}
			// use link to get peer info
			remote_port_id = src_topo_info->links[iodie_id][port_id].peer_port;
			(void)memcpy(&route_list->buf[num].src,
				src_aggr_dev->fe[iodie_id].port_eid[port_id],
				sizeof(union ubcore_eid));
			(void)memcpy(&route_list->buf[num].dst,
				dst_aggr_dev->fe[iodie_id].port_eid[remote_port_id],
				sizeof(union ubcore_eid));
			route_list->buf[num].flag.bs.rtp = 1;
			route_list->buf[num].flag.bs.ctp = 1;
			route_list->buf[num].flag.bs.utp = 1;
			num++;
		}
	}

	if (route_list->route_num == num) {
		ubcore_log_err(
			"Failed to get topo port eid, route_num: %u.\n", num);
		return -EINVAL;
	}

	route_list->route_num = num;
	return 0;
}

// 默认选择0号iodie
int ubcore_get_primary_eid_by_aggr_eid(union ubcore_eid *aggr_eid,
	union ubcore_eid *primary_eid)
{
	struct ubcore_topo_map *topo_map;
	int node_id, dev_id;

	topo_map = ubcore_get_global_topo_map();
	if (!topo_map) {
		ubcore_log_err("Failed get global topo map");
		return -EINVAL;
	}

	for (node_id = 0; node_id < topo_map->node_num; node_id++) {
		for (dev_id = 0; dev_id < DEV_NUM; dev_id++) {
			if (memcmp(aggr_eid, topo_map->topo_infos[node_id].devs[dev_id].aggr_eid,
					sizeof(*aggr_eid)) == 0) {
				*primary_eid = *((union ubcore_eid *)
					topo_map->topo_infos[node_id].devs[dev_id].fe[0].primary_eid);
				return 0;
			}			
		}
	}
	return -EINVAL;
}

// 路由是什么粒度？---通过aggr_eid定位dev，以primary eid对为单位进行路由
static int ubcore_get_route_primary_eid(union ubcore_eid *src_v_eid,
	union ubcore_eid *dst_v_eid, struct ubcore_route_list *route_list)
{
	int src_dev_id, dst_dev_id, iodie_id, port_id, remote_port_id;
	struct ubcore_topo_aggr_dev *src_aggr_dev = NULL;
	struct ubcore_topo_aggr_dev *dst_aggr_dev = NULL;
	struct ubcore_topo_node *src_topo_info = NULL;
	struct ubcore_topo_node *dst_topo_info = NULL;
	uint32_t num = route_list->route_num;
	bool has_link = false;

	src_topo_info = ubcore_get_topo_info_by_aggr_eid(src_v_eid, &src_dev_id);
	if (IS_ERR_OR_NULL(src_topo_info)) {
		ubcore_log_err("Failed to get src_topo_info.\n");
		return -EINVAL;
	}
	src_aggr_dev = &src_topo_info->devs[src_dev_id];

	dst_topo_info = ubcore_get_topo_info_by_aggr_eid(dst_v_eid, &dst_dev_id);
	if (IS_ERR_OR_NULL(dst_topo_info)) {
		ubcore_log_err("Failed to get dst_topo_info.\n");
		return -EINVAL;
	}
	dst_aggr_dev = &dst_topo_info->devs[dst_dev_id];

	// update route---check if physical link exists?
	for (iodie_id = 0; iodie_id < IODIE_NUM; iodie_id++) {
		for (port_id = 0; port_id < PORT_NUM; port_id++) {
			remote_port_id = src_topo_info->links[iodie_id][port_id].peer_port;
			if (remote_port_id == UINT32_MAX) {
				continue;
			}
			if (src_topo_info->links[iodie_id][port_id].peer_node == dst_topo_info->id) {
				has_link = true;
				break;
			}
		}
		if (has_link == true) {
			break;
		}
	}
	if (has_link == false) {
		ubcore_log_err("No links to dst eid.\n");
		return -EINVAL;
	} 
	for (iodie_id = 0; iodie_id < IODIE_NUM; iodie_id++) {
		route_list->buf[num + iodie_id].flag.bs.ctp = 1;
		route_list->buf[num + iodie_id].hops = 0;
		(void)memcpy(&route_list->buf[num + iodie_id].src, src_aggr_dev->fe[iodie_id].primary_eid, sizeof(union ubcore_eid));
		(void)memcpy(&route_list->buf[num + iodie_id].dst, dst_aggr_dev->fe[iodie_id].primary_eid, sizeof(union ubcore_eid));
	}

	route_list->route_num += IODIE_NUM;
	return 0;
}

int ubcore_get_route_list(struct ubcore_route *route,
	struct ubcore_route_list *route_list)
{
	int ret = 0;

	// check valid pointer to v_eid
	if (IS_ERR_OR_NULL(route) || IS_ERR_OR_NULL(route_list)) {
		ubcore_log_err("Invalid parameter.\n");
		return -EINVAL;
	}
	union ubcore_eid *src_v_eid = &route->src;
	union ubcore_eid *dst_v_eid = &route->dst;

	if (!g_ubcore_topo_map) {
		ubcore_log_err(
			"Failed to get p_eid, ubcore topo map doesn't exist.\n");
		return -EINVAL;
	}

	(void)memset(route_list, 0, sizeof(struct ubcore_route_list));

	ret = ubcore_get_route_primary_eid(src_v_eid, dst_v_eid, route_list);
	if (ret != 0) {
		ubcore_log_err("Failed to get primary eid, ret: %d.\n", ret);
		return ret;
	}

	ret = ubcore_get_route_port_eid(src_v_eid, dst_v_eid, route_list);
	if (ret != 0) {
		ubcore_log_err("Failed to get port eid, ret: %d.\n", ret);
		return ret;
	}

	ubcore_log_info("Finish to query primary port eid, route_num: %u.\n",
		route_list->route_num);
	return 0;
}
EXPORT_SYMBOL(ubcore_get_route_list);
