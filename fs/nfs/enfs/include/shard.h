/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: nfs client shard route
 * Author: 
 * Create: 2023-11-14
 */

#ifndef _ENFS_SHARD_H_
#define _ENFS_SHARD_H_

#include
#include "exten_call.h"

extern unsigned int enfs_uuid_debug;

void shard_set_transport(struct rpc_task *task, struct rpc_clnt *clnt);
int enfs_debug_match_cmd(char *str, size_t len);
int enfs_shard_init(void);
void enfs_shard_exit(void);

int enfs_find_clnt_root(struct rpc_clnt *clnt, FILE_UUID * root_uuid);
int enfs_insert_clnt_root(struct rpc_clnt *clnt, FILE_UUID * root_uuid);
int enfs_delete_clnt_shard_cache(struct rpc_clnt *clnt);
void enfs_query_xprt_shard(struct rpc_clnt *clnt, struct rpc_xprt *xprt);
#endif // _ENFS_SHARD_H_
