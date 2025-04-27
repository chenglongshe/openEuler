/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: enfs multipath
 * Author:
 * Create: 2023-07-31
 */

#ifndef ENFS_MULTIPATH_H
#define ENFS_MULTIPATH_H
#include 
#include "enfs_multipath_parse.h"

#define MAX_XPRT_NUM_PER_CLIENT 31

int enfs_multipath_init(void);
void enfs_multipath_exit(void);
void enfs_xprt_ippair_create(struct xprt_create *xprtargs, struct rpc_clnt *clnt, void *data);
int enfs_config_xprt_create_args(struct xprt_create *xprtargs, struct rpc_create_args *args,
									char *servername, size_t length);
void print_enfs_multipath_addr(struct sockaddr *local, struct sockaddr *remote);
int multipath_query_dns(struct multipath_mount_options *opt,
						unsigned short family, bool use_cache, struct rpc_clnt *clnt);
void enfs_for_each_rpc_clnt(int (*fn)(struct rpc_clnt *clnt, void *data), void *data);

#endif // ENFS_MULTIPATH_H
