/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright 2024, Huawei Inc
 *
 * 多路径管理头文件, 对nfs/dpc client提供多路径的创删和管理. 对外结构体和头文件应保持最小依赖原则, 不要包含内部头文件.
 * 不允许将内部结构体函数放进来.
 */
#ifndef _MULTIPATH_API_H_
#define _MULTIPATH_API_H_
#include "multipath_types.h"

#define MULP_INVAILD_MP_ID (~0ULL)
#define MULP_MAX_IP_STR_LEN 64
#define MULP_USER_NAME_LEN 64
#define MULP_USER_AUTHKEY_LEN 256
#define MULP_FILE_UUID_LEN 38
#define MULP_PORT_NAME_LEN 16
#define MULP_ZONE_MAX_CNT 64
#define MULP_ONE_ZONE_MAX_CTRL_CNT 64
#define MULP_ONE_NODE_MAX_CPU_CNT 4

typedef enum {
    PATH_INFO_STATUS = 0,
    PATH_INFO_DETECT_TIME = 1,
    // 读写信息统计
    PATH_INFO_READ_CNT = 2,
    PATH_INFO_READ_LEN = 3,
    PATH_INFO_READ_SUM_DELAY = 4,
    PATH_INFO_READ_FAILED_CNT = 5,
    PATH_INFO_WRITE_CNT = 6,
    PATH_INFO_WRITE_LEN = 7,
    PATH_INFO_WRITE_SUM_DELAY = 8,
    PATH_INFO_WRITE_FAILED_CNT = 9,
    PATH_INFO_ERRNO = 10,
} PATH_INFO_TYPE;
typedef struct {
    uint32_t len; /* 实际长度 */
    uint8_t data[MULP_FILE_UUID_LEN];
} mulp_file_uuid;

typedef enum {
    MULP_APP_NFS,
    MULP_APP_DPC,
    MULP_APP_MAX,
} mulp_app_id_type;

typedef enum {
    MULP_STRATEGY_ROUNDROBIN,
    MULP_STRATEGY_SHARDVIEW,
    MULP_STRATEGY_MAX,
} mulp_select_path_strategy;

typedef enum {
    MULP_NETWORK_TCP,
    MULP_NETWORK_RDMA,
    MULP_NETWORK_MAX,
} mulp_network_type;

typedef struct {
    char *local_ip;
    char *remote_ip;
    uint64_t wwn;
    uint32_t lsid; /* 二进制末6位节点id应小于MULP_ONE_ZONE_MAX_CTRL_CNT */
    uint32_t zone_id; /* 应小于MULP_ZONE_MAX_CNT */
    uint32_t cpu_id; /* 应小于MULP_ONE_NODE_MAX_CPU_CNT */
    uint32_t is_add; /* 当前只支持全量更新 */
    char *port_name;
} mulp_ip_pair;

#define MULP_MAX_NCONNECT 8
typedef struct {
    uint32_t ip_pair_cnt;
    mulp_ip_pair *pair_arr;
    uint64_t client_id;
    uint32_t client_ls_id;
    uint32_t nconnect; /* 每个ip pair创建多个链路, 需要大于0, 最大值MULP_MAX_NCONNECT */
    mulp_network_type network_type;
    mulp_select_path_strategy strategy;
    char *user_name;
    char *user_authkey; /* 是字符串还是二进制? */
    void *ctx; /* 创建多链路是后台操作,创建完成后,多路径模块进行回调的应用上下文. 可以为NULL */
    void (*callback)(int result, uint64_t mp_id, void *ctx); /* 创建多链路是后台操作,完成后多路径模块进行回调. 可以为NULL */
    uint32_t detect_period; // 探测周期(s)
} mulp_create_mp_args;

typedef struct {
    uint64_t mp_id;
    uint32_t ip_pair_cnt;
    uint32_t nconnect;
    mulp_ip_pair *pair_arr;
} mulp_update_ip_pair_args;

typedef struct {
    char *ip;
    uint32_t lsid;
    uint32_t zone_id;
    uint32_t cpu_id;
} mulp_ip_view;

typedef struct {
    uint64_t mp_id;
    uint32_t ip_cnt;
    mulp_ip_view *view_arr;
} mulp_update_ip_view_args;

typedef struct {
    uint32_t lsid;
    uint32_t zone_id;
    uint32_t cpu_id;
} mulp_shard_view;

typedef struct {
    uint64_t mp_id;
    uint64_t wwn;
    uint64_t pool_id; /* 当前是storage pool id */
    uint64_t cluster_id;
    uint32_t shard_cnt;
    mulp_shard_view *view_arr; /* 数组, 下标即fsp id */
} mulp_update_shard_view_args;

typedef struct {
    mulp_ip_pair *ip_pair;
    mulp_network_type network_type;
    void* path;
} mulp_ops_create_path_args;

typedef struct {
    int (*ping_func)(void *path, uint64_t mp_id, void *path_info, void *context,
        void (*cb)(int result, uint64_t mp_id, void *path_info, void *context));
    int (*create_path_func)(mulp_ops_create_path_args *args);
    int (*destroy_path_func)(void *path, uint64_t mp_id, void *path_info, void *context,
        void (*cb)(int result, uint64_t mp_id, void *path_info, void *context));
} mulp_app_ops_set;

typedef struct {
    mulp_file_uuid *uuid;
    uint64_t cluster_id;
    uint64_t pool_id;
} mulp_file_info;

/* ****************************************************************************
 * 创建一个多路径集, 同步返回多路径集id. 但是建链是异步, 建链完成后会进行回调
 * 通知结果.
 *
 * app_id --- 入参, 应用的id
 * mp_id ---出参, 创建的多路径集id,后续操作需要使用传入此id.
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int mulp_create_mp(mulp_app_id_type app_id, mulp_create_mp_args *args, uint64_t *mp_id);
int mulp_update_detect_period(uint64_t mp_id, uint32_t detect_period);
int mulp_update_ip_pair(mulp_update_ip_pair_args *args);
int mulp_update_ip_view(mulp_update_ip_view_args *args);
int mulp_update_shard_view(mulp_update_shard_view_args *args);

/* ****************************************************************************
 * 删除一个多路径集. 调用之后,多路径集id,不再可用. 清理资源过程是异步的,完成后会进行回调
 * 通知结果.
 *
 * mp_id --- 入参, 多路径集的id.
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int mulp_destroy_mp(uint64_t mp_id);

/* ****************************************************************************
 * 注册上层应用的ops集, 多路径会在流程中进行回调, 必须先调用此函数才能进行其他流程调用.
 *
 * app_id --- 入参, 应用的id.
 * set  --- 应用的操作集
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int mulp_reg_app_ops(mulp_app_id_type app_id, mulp_app_ops_set *set);


int mulp_unreg_app_ops(mulp_app_id_type app_id);

int mulp_ping_all_path(uint64_t mp_id, void *ctx, void (*callback)(int result, uint64_t mp_id, void *ctx));

/* ****************************************************************************
 * 根据文件的句柄(通常是uuid), 获取最佳路径的指针. 需要使用mulp_io_put_path进行释放引用
 *
 * mp_id --- 入参, 多路径集的id.
 * uuid --- 入参, 文件的uuid
 * path_mgmt ---- 出入参, 返回multipath路径管理结构指针, path_mgmt传入时不允许为NULL.
 * path ---- 出入参, 返回dpc/nfs创建的路径的指针, path传入时不允许为NULL.
 * is_direct_ctrl --- 出入参，返回选的链路是否为直连控制器, is_direct_ctrl不允许为NULL
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int mulp_io_get_optimal_path(uint64_t mp_id, mulp_file_info *file_info, uint64_t timestamp, void **path_mgmt,
    void **path, uint32_t *is_direct_ctrl);

/* ****************************************************************************
 * 释放使用mulp_io_get_optimal_path获取最佳路径的指针引用.
 *
 * mp_id --- 入参, 多路径集的id.
 * path_mgmt ---- 入参, 路径指针, path_mgmt传入时不允许为NULL.
 * 返回值: 0 -- 成功, 其他--失败
 * *************************************************************************** */
int mulp_io_put_path(uint64_t mp_id, void *path_mgmt);

int mulp_dump_path_info(void (*callback)(const char *buffer), uint64_t mp_id, BOOLEAN_T debug);

void mulp_path_count_stats(void *path_info, PATH_INFO_TYPE type, uint64_t value);

int mulp_path_clean_mp_rw_info(const char *data);

void mulp_path_notify_io_result(void *path_info, int io_result, uint64_t start_time);

int mulp_get_shard_view(uint64_t wwn, uint64_t cluster_id, uint64_t pool_id, mulp_shard_view *shard_view);

void mulp_destroy_shard_view(void);

int mulp_ctor(void);
void mulp_dector(void);
#endif