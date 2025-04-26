/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright 2024, Huawei Inc
 *
 * DPC client对ODC提供创删链路以及读写接口. 对外结构体和头文件应保持最小依赖原则, 不要包含内部头文件.
 * 不允许将内部结构体函数放进来.
 */
#ifndef _DPC_CLIENT_API_H_
#define _DPC_CLIENT_API_H_
#include 
#include 
#include 
#include "multipath_api.h"

#define DPC_REQ_DLG_BYPASS_FLAG BIT(0) // no need check dlg, such as write cache flush
#define DPC_REQ_IS_ASYNC BIT(1)
#define DPC_TOS_INFO_CNT_MAX 10

/* Server capabilities */
#define DPC_RPC_CAP_READDIRPLUS	(1U << 0)
#define DPC_RPC_CAP_HARDLINKS	(1U << 1)
#define DPC_RPC_CAP_SYMLINKS	(1U << 2)
#define DPC_RPC_CAP_ACLS		(1U << 3)
#define DPC_RPC_CAP_ATOMIC_OPEN	(1U << 4)
#define DPC_RPC_CAP_LGOPEN		(1U << 5)
#define DPC_RPC_CAP_FILEID		(1U << 6)
#define DPC_RPC_CAP_MODE		(1U << 7)
#define DPC_RPC_CAP_NLINK		(1U << 8)
#define DPC_RPC_CAP_OWNER		(1U << 9)
#define DPC_RPC_CAP_OWNER_GROUP	(1U << 10)
#define DPC_RPC_CAP_ATIME		(1U << 11)
#define DPC_RPC_CAP_CTIME		(1U << 12)
#define DPC_RPC_CAP_MTIME		(1U << 13)
#define DPC_RPC_CAP_POSIX_LOCK	(1U << 14)
#define DPC_RPC_CAP_UIDGID_NOMAP	(1U << 15)
#define DPC_RPC_CAP_STATEID_NFSV41	(1U << 16)
#define DPC_RPC_CAP_ATOMIC_OPEN_V1	(1U << 17)
#define DPC_RPC_CAP_SECURITY_LABEL	(1U << 18)
#define DPC_RPC_CAP_SEEK		(1U << 19)
#define DPC_RPC_CAP_ALLOCATE	(1U << 20)
#define DPC_RPC_CAP_DEALLOCATE	(1U << 21)
#define DPC_RPC_CAP_LAYOUTSTATS	(1U << 22)
#define DPC_RPC_CAP_CLONE		(1U << 23)
#define DPC_RPC_CAP_COPY		(1U << 24)
#define DPC_RPC_CAP_OFFLOAD_CANCEL	(1U << 25)
#define DPC_RPC_CAP_LAYOUTERROR	(1U << 26)
#define DPC_RPC_CAP_COPY_NOTIFY	(1U << 27)
#define DPC_RPC_CAP_XATTR		(1U << 28)
#define DPC_RPC_CAP_READ_PLUS	(1U << 29)

#define DPC_RPC_ATTR_FATTR_TYPE        (1U << 0)
#define DPC_RPC_ATTR_FATTR_MODE        (1U << 1)
#define DPC_RPC_ATTR_FATTR_NLINK        (1U << 2)
#define DPC_RPC_ATTR_FATTR_OWNER        (1U << 3)
#define DPC_RPC_ATTR_FATTR_GROUP        (1U << 4)
#define DPC_RPC_ATTR_FATTR_RDEV        (1U << 5)
#define DPC_RPC_ATTR_FATTR_SIZE        (1U << 6)
#define DPC_RPC_ATTR_FATTR_PRESIZE        (1U << 7)
#define DPC_RPC_ATTR_FATTR_BLOCKS_USED    (1U << 8)
#define DPC_RPC_ATTR_FATTR_SPACE_USED    (1U << 9)
#define DPC_RPC_ATTR_FATTR_FSID        (1U << 10)
#define DPC_RPC_ATTR_FATTR_FILEID        (1U << 11)
#define DPC_RPC_ATTR_FATTR_ATIME        (1U << 12)
#define DPC_RPC_ATTR_FATTR_MTIME        (1U << 13)
#define DPC_RPC_ATTR_FATTR_CTIME        (1U << 14)
#define DPC_RPC_ATTR_FATTR_PREMTIME        (1U << 15)
#define DPC_RPC_ATTR_FATTR_PRECTIME        (1U << 16)
#define DPC_RPC_ATTR_FATTR_CHANGE        (1U << 17)
#define DPC_RPC_ATTR_FATTR_PRECHANGE    (1U << 18)
#define DPC_RPC_ATTR_FATTR_V4_LOCATIONS    (1U << 19)
#define DPC_RPC_ATTR_FATTR_V4_REFERRAL    (1U << 20)
#define DPC_RPC_ATTR_FATTR_MOUNTPOINT    (1U << 21)
#define DPC_RPC_ATTR_FATTR_MOUNTED_ON_FILEID (1U << 22)
#define DPC_RPC_ATTR_FATTR_OWNER_NAME    (1U << 23)
#define DPC_RPC_ATTR_FATTR_GROUP_NAME    (1U << 24)
#define DPC_RPC_ATTR_FATTR_V4_SECURITY_LABEL (1U << 25)
#define DPC_RPC_ATTR_FATTR_USED        (1U << 26)


#define DPC_RPC_ATTR_FATTR (DPC_RPC_ATTR_FATTR_TYPE \
    | DPC_RPC_ATTR_FATTR_MODE \
    | DPC_RPC_ATTR_FATTR_NLINK \
    | DPC_RPC_ATTR_FATTR_OWNER \
    | DPC_RPC_ATTR_FATTR_GROUP \
    | DPC_RPC_ATTR_FATTR_RDEV \
    | DPC_RPC_ATTR_FATTR_SIZE \
    | DPC_RPC_ATTR_FATTR_FSID \
    | DPC_RPC_CAP_FILEID \
    | DPC_RPC_ATTR_FATTR_ATIME \
    | DPC_RPC_ATTR_FATTR_MTIME \
    | DPC_RPC_ATTR_FATTR_CTIME \
    | DPC_RPC_ATTR_FATTR_USED)

#define DPC_RPC_ERR_JUKEBOX (10008)

typedef enum {
    DPCREG = 1,
    DPCDIR = 2,
    DPCBLK = 3,
    DPCCHR = 4,
    DPCLNK = 5,
    DPCSOCK = 6,
    DPCFIFO = 7,
    DPCBAD = 10
} dpc_ftype;

typedef struct {
    uint32_t valid;
    dpc_ftype type;
    umode_t mode;
    uint32_t nlink;
    kuid_t uid;
    kgid_t gid;
    uint64_t size;
    uint64_t used;
    uint64_t fsid;
    uint64_t file_id;
    dev_t rdev;
    struct timespec64 atime;
    struct timespec64 mtime;
    struct timespec64 ctime;
    struct timespec64 crttime;
    uint64_t reserved1;
    uint64_t reserved2;
} dpc_clnt_file_attr;

typedef struct dpc_clnt_rw_args {
    struct rpc_task *task;
    struct cred *cred;
    struct rpc_call_ops *callback_ops;
    void *callback_data;
    void (*callback)(struct dpc_clnt_rw_args *data);
    void (*statis_callback)(struct dpc_clnt_rw_args *data, void *task);
    struct workqueue_struct *workqueue;
    uint8_t priority;

    uint64_t mp_id;                         /* 选路入参，multipath instance id */
    uint64_t cluster_id;                    /* 选路入参，集群id */
    uint64_t pool_id;                       /* 选路入参，存储池id */
    uint32_t timeout_ms;                    /* 选路入参，请求IO超时时间，超时不再换路重试 */
    uint32_t flag;                          /* 写请求入参，DPC_REQ_DLG_BYPASS_FLAG etc */
    uint32_t len;                           /* 读写请求入参，数据总长度 */
    uint64_t offset;                        /* 读写请求入参，文件内偏移 */
    uint32_t pgbase;                        /* 读写请求入参，首页页内偏移 */
    mulp_file_uuid uuid;                    /* 读写请求入参，同时作为选路入参，文件UUID */
    uint32_t share_id;                      /* 读请求入参 */

    struct page **pages;                    /* 读写请求页面载荷 */

    dpc_clnt_file_attr *prev_attr;          /* 写请求出参，存储返回的文件修改前attr */
    dpc_clnt_file_attr *post_attr;          /* 普通读、元数据扩展读、写请求出参，存储返回的文件当前attr */
    int32_t op_status;                      /* 读写请求出参，结果状态码 */
    uint64_t counted;                       /* 读请求出参，读到的数据长度 */
    uint32_t eof;                           /* 读请求出参，根据flag DPC_READ_EOF_BIT_FLAG位判断 */
} dpc_clnt_rw_args;

typedef struct dpc_clnt_port_tos {
    uint16_t port;
    uint32_t tos;
} dpc_clnt_port_tos;

typedef struct dpc_clnt_tos_info {
    dpc_clnt_port_tos tos_info[DPC_TOS_INFO_CNT_MAX];
    uint16_t cnt;
} dpc_clnt_tos_info;

/* ****************************************************************************
 * 给odc提供读接口, 同步操作.
 *
 * args --- 出入参, 详见结构体描述
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int dpc_clnt_read_page(dpc_clnt_rw_args *args);

/* ****************************************************************************
 * 给odc提供写接口, 同步操作.
 *
 * args --- 出入参, 详见结构体描述
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int dpc_clnt_write_page(dpc_clnt_rw_args *args);

int dpc_clnt_create_mp(mulp_create_mp_args *args, uint64_t *mp_id);
int dpc_clnt_update_mp(uint64_t mp_id, mulp_create_mp_args *args);
int dpc_clnt_update_detect_period(uint64_t mp_id, uint32_t detect_period);
int dpc_clnt_update_ip_pair(mulp_update_ip_pair_args *args);
int dpc_clnt_update_ip_view(mulp_update_ip_view_args *args);
int dpc_clnt_update_shard_view(mulp_update_shard_view_args *args);
int dpc_clnt_destroy_mp(uint64_t mp_id);
void* dpc_clnt_zalloc(uint32_t size, gfp_t flag);
void dpc_clnt_free(void *ptr);

int dpc_clnt_set_tos_info(uint16_t port, uint32_t tos);
int dpc_clnt_get_tos_info(dpc_clnt_tos_info *tos_info);

#endif