/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2023-2023. All rights reserved.
 * Description: remount ip header file
 * Author: y00583252
 * Create: 2023-08-12
 */
#ifndef _ENFS_REMOUNT_
#define _ENFS_REMOUNT_
#include
#include "enfs.h"

int enfs_remount(struct nfs_client *nfs_client, void *enfs_option);
int enfs_remount_iplist(struct nfs_client *nfs_client, void *enfs_option);

#endif	/*  */
