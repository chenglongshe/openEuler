// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef _EXTEN_CALL_H_
#define _EXTEN_CALL_H_

#define IP_ADDRESS_LEN_MAX 64
#define FILE_UUID_BUFF_LEN 38
#define MAX_SHARD_NUMBER_IN_CLUSTER_4FS (1024 * 64)
#define MAX_GLOBAL_CTRL_NODE_NUM 2048
#define INVALID_CPU_ID 2147483647

#define ENFS_CAPABILITY_LSID_NOTSUPPORT 0x0001	/* lsversion query capability */
#define ENFS_CAPABILITY_LSID_SUPPORT 0x0002	/* lsversion query capability */

typedef enum {
	NFS3_GET_FSINFO_OP = 0,
	NFS3_GET_LIF_VIEW_OP,
	NFS_ENFS_QUERY_DNS_OP,
	NFS3_GET_LS_VERSION_OP,
	NFS_ENFS_OP_BUTT
} EnfsExtendOpCode;

typedef struct {
	uint8_t data[FILE_UUID_BUFF_LEN];	/* uuid byte array */
	uint32_t dataLen;	/* uuid byte size */
} FILE_UUID;
typedef struct {
	uint32_t tenantId;
	uint32_t ipNumber;
	char ipAddr[0];
} LIF_ARGS;
typedef struct {
	uint32_t ipType;	// 需要存储返的ip类型对应Ip, IP_TYPE_E中的值
	uint32_t dnsNameCount;
	char dnsName[0];
} DNS_ARGS;
typedef enum IP_TYPE_E {
	IP_TYPE_V4 = 0,		/* *< 只需要IPv4类型 */
	IP_TYPE_V6 = 1,		/* *< 只需要IPv6类型 */
	IP_TYPE_BOTH = 2,	/* 同时需要ipv4 ipv6  */
	IP_TYPE_BUTT = 3	/* *< 无效值 */
} IP_TYPE_E;

typedef struct {
	uint64_t lsid;
	uint32_t cpuId;
} FS_SHARD_VIEW_SINGLE;

typedef struct {
	uint64_t clusterId;
	uint32_t storagePoolId;
	uint32_t fsId;
	uint32_t tenantId;
	uint32_t num;
	FS_SHARD_VIEW_SINGLE shardView[0];
} FS_SHARD_VIEW;

typedef struct {
	uint64_t lsVersion;
	uint32_t lsId;
} EXTEND_GET_LS_VERSION_SINGLE;

typedef struct {
	uint32_t num;
	uint64_t clusterId;
	EXTEND_GET_LS_VERSION_SINGLE lsInfo[0];
} EXTEND_GET_LS_VERSION;

typedef struct {
	uint32_t isfound;
	uint32_t workStatus;
	uint64_t lsId;
	uint32_t tenantId;
	uint64_t homeSiteWwn;
	uint32_t cpuId;
} LIF_PORT_INFO_SINGLE;

typedef struct {
	char ipAddr[IP_ADDRESS_LEN_MAX];
} DNS_QUERY_IP_INFO;

typedef struct {
	uint64_t lsId;
	uint32_t offset;
	uint32_t count;
} DNS_QUERY_LSID_INFO;

typedef struct {
	char ipAddr[IP_ADDRESS_LEN_MAX];
	uint64_t lsId;
	uint32_t cpuId;
} DNS_QUERY_IP_INFO_SINGLE;

typedef struct {
	uint32_t ipNumber;
	DNS_QUERY_IP_INFO_SINGLE ipInfo[0];
} DNS_QUERY_IP_INFO_MULTIPLE;

typedef struct {
	uint32_t lifNumber;
	LIF_PORT_INFO_SINGLE lifport[0];
} LIF_PORT_INFO_MULTIPLE;

typedef struct {
	char ipAddr[IP_ADDRESS_LEN_MAX];
	uint32_t workStatus;
	uint64_t lsId;
	uint64_t wwn;
	uint32_t cpuId;
} LIF_PORT_INFO;

typedef struct {
	uint32_t opcode;
	uint32_t version;
	union {
		FILE_UUID Uuid;
		LIF_ARGS lifArgs;
		DNS_ARGS dnsArgs;
	} extend_args_u;
} EXTEND3args;

typedef struct {
	uint32_t opcode;
	uint32_t version;
	union {
		FS_SHARD_VIEW fsInfo;
		LIF_PORT_INFO_MULTIPLE lifInfo;
		DNS_QUERY_IP_INFO_MULTIPLE dnsQueryIpInfo;
		EXTEND_GET_LS_VERSION lsView;
	} extend_res_u;
} EXTEND3res;

typedef struct {
	union {
		EXTEND3args args;
		EXTEND3res res;
	} extend_u;
} nfs_extend_s;

int dorado_extend_op(struct rpc_clnt *clnt, char *buf, int *buflen);
int dorado_query_fs_shard(struct rpc_clnt *clnt, FILE_UUID * file_uuid,
			  FS_SHARD_VIEW ** resDataOut);
int dorado_query_lifview(struct rpc_clnt *clnt, struct rpc_xprt *xprt,
			 char *ipAddr, uint32_t ipNumber,
			 LIF_PORT_INFO * lifInfo);
int enfs_query_lifview(struct rpc_clnt *clnt, struct rpc_xprt *xprt,
		       char *ipaddr, uint64_t * lsid, uint64_t * wwn,
		       uint32_t * cpuId);

int dorado_query_dns(struct rpc_clnt *clnt,
		     DNS_QUERY_IP_INFO_SINGLE ** dnsQueryIpInfo,
		     uint32_t ip_type, uint32_t dnsNamecount, char *dnsName,
		     int *ipNumber);
int dorado_query_lsId(struct rpc_clnt *clnt,
		      EXTEND_GET_LS_VERSION ** resDataOut);

// =============
// =============
#define NFS3PROC_EXTEND 22

#define DIR_BIT_POS 63
#define NID_BITS_SZ 12

#ifndef NID_BITS_MASK
#define NID_BITS_MASK ((1 << NID_BITS_SZ) - 1)	// ???????????
#endif
#define NID_BITS_POS (DIR_BIT_POS - NID_BITS_SZ)
#define ENFS_GLOBAL_OID_BITS 47

#define UUID_OFFSET 16
#define UUID_DEVID_OFFSET 2	// 8byte,2-9   devid wwn
#define UUID_FSID_OFFSET 10	// 4byte,10-13 fsid
#define UUID_DTREEID_OFFSET 14	// 4byte,14-17 dtreeid
#define UUID_SNAPID_OFFSET 18	// 4byte,18-21 snapid
// | 1       | 12    | 51  |
// | reserved| fspid | oid |
// pfid contains fspid &oid
// fspid: File Service Partition == shardId or partitionID
// oid: dentrytable id
#define UUID_PFID_OFFSET 22	// 8byte,22-29 birthpfid
#define UUID_FID_OFFSET 30	// 8byte,30-38 fileid

// nfs objectid to fspid
#define DIR_BIT_POS 63
#define NID_BITS_SZ 12

#define NFS_UUID_LEN 38

static inline void fh_file_uuid(const struct nfs_fh *fh, FILE_UUID *file_uuid)
{
	memcpy((void *)file_uuid->data, (void *)(fh->data + UUID_OFFSET),
	       FILE_UUID_BUFF_LEN);
	file_uuid->dataLen = FILE_UUID_BUFF_LEN;
}

static inline uint64_t *fh_devid(struct nfs_fh *fh)
{
	uint8_t *uuid = (uint8_t *) (fh->data + UUID_OFFSET);
	return ((uint64_t *) (uuid + UUID_DEVID_OFFSET));
}

static inline uint32_t *fh_fsid(struct nfs_fh *fh)
{
	uint8_t *uuid = (uint8_t *) (fh->data + UUID_OFFSET);
	return ((uint32_t *) (uuid + UUID_FSID_OFFSET));
}

static inline uint64_t get_objectid_from_uuid(FILE_UUID *file_uuid)
{
	uint8_t *uuid = (uint8_t *) (file_uuid->data);
	uint64_t objectId = *((uint64_t *) (uuid + UUID_FID_OFFSET));

	// default is directory, use file id
	// if is file, use pfid
	if ((objectId >> DIR_BIT_POS) == 0) {
		objectId = *((uint64_t *) (uuid + UUID_PFID_OFFSET));
	}
	return objectId;
}

#define GET_DEVID_FROM_UUID(puuid) \
	(*((uint64_t *)((puuid)->data + UUID_DEVID_OFFSET)))
#define GET_FSID_FROM_UUID(puuid) \
	(*((uint32_t *)((puuid)->data + UUID_FSID_OFFSET)))
#define GET_DTREEID_FROM_UUID(puuid) \
	(*((uint32_t *)((puuid)->data + UUID_DTREEID_OFFSET)))
#define GET_SNAPID_FROM_UUID(puuid) \
	(*((uint32_t *)((puuid)->data + UUID_SNAPID_OFFSET)))
#define GET_PFID_FROM_UUID(puuid) \
	(*((uint64_t *)((puuid)->data + UUID_PFID_OFFSET)))
#define GET_FID_FROM_UUID(puuid) \
	(*((uint64_t *)((puuid)->data + UUID_FID_OFFSET)))
#define ENFS_GET_FSID_HIGHEST_BIT(fsid) \
	(((fsid) >> 31) & 1)
#define ENFS_GET_LOCAL_FSPID_FROM_FID(fid) \
	(uint32_t)(((fid) >> NID_BITS_POS) & 0xFFF)
#define ENFS_GET_GLOBAL_FSPID_FROM_FID(fid) \
	(uint32_t)(((fid) >> ENFS_GLOBAL_OID_BITS) & 0xFFFF)
#define ENFS_GET_FSP_FROM_FSID_FID(fsid, fid) \
    (ENFS_GET_FSID_HIGHEST_BIT(fsid) ? ENFS_GET_GLOBAL_FSPID_FROM_FID(fid) : ENFS_GET_LOCAL_FSPID_FROM_FID(fid))
// =============

static inline uint64_t get_fspid_from_uuid(FILE_UUID *file_uuid)
{
	uint64_t objectId = get_objectid_from_uuid(file_uuid);
	uint32_t fsId = GET_FSID_FROM_UUID(file_uuid);

	return ENFS_GET_FSP_FROM_FSID_FID(fsId, objectId);
}

static inline uint32_t get_shardid_from_uuid(FILE_UUID *file_uuid)
{
	return get_fspid_from_uuid(file_uuid) % MAX_SHARD_NUMBER_IN_CLUSTER_4FS;
}

int scan_uuid(const char *str, uint8_t * arr, int arrlen);
int sprint_uuid(char *buf, int buflen, FILE_UUID * file_uuid);

void enfs_print_uuid(FILE_UUID * file_uuid);

#endif // _EXTEN_CALL_H_
