// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef _ENFS_MULTIPATH_CLIENT_H_
#define _ENFS_MULTIPATH_CLIENT_H_

#include "enfs.h"

struct multipath_client_info {
	int version;
	struct work_struct work;
	struct nfs_ip_list *remote_ip_list;
	struct nfs_ip_list *local_ip_list;
	NFS_ROUTE_DNS_INFO_S *pRemoteDnsInfo;
	//struct multipath_conn_pairs conn_pairs;
	s64 client_id;
	u32 fill_local : 1;
	u32 updating_domain : 1;
	u32 reverse[2];
};

int nfs_multipath_client_info_init(void **data,
				   const struct nfs_client_initdata *cl_init);
void nfs_multipath_client_info_free(void *data);
int nfs_multipath_client_info_match(void *src, void *dst);
int nfs4_multipath_client_info_match(void *src, void *dst);
void nfs_multipath_client_info_show(struct seq_file *mount_option, void *data);
int nfs_multipath_dns_list_info_match(const NFS_ROUTE_DNS_INFO_S *dns_src,
				      const NFS_ROUTE_DNS_INFO_S *dns_dst);
int enfs_alloc_nfsclient_info(struct multipath_client_info **client_info);
void enfs_free_nfsclient_info(struct multipath_client_info *client_info);

#endif
