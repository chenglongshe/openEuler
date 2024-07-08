// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef BNXT_UDCC_H
#define BNXT_UDCC_H

#define BNXT_UDCC_MAX_SESSIONS		2048

#define BNXT_UDCC_HASH_SIZE		64

#define BNXT_UDCC_SESSION_CREATE	0
#define BNXT_UDCC_SESSION_DELETE	1

struct bnxt_udcc_session_entry {
	u32			session_id;
	u32			rx_flow_id;
	u32			tx_flow_id;
	u64			rx_counter_hndl;
	u64			tx_counter_hndl;
	u8			dest_mac[ETH_ALEN];
	u8			src_mac[ETH_ALEN];
	u8			dst_mac_mod[ETH_ALEN];
	u8			src_mac_mod[ETH_ALEN];
	struct in6_addr		dst_ip;
	struct in6_addr		src_ip;
	u32			src_qp_num;
	u32			dest_qp_num;
	struct dentry		*debugfs_dir;
	struct bnxt		*bp;
};

struct bnxt_udcc_work {
	struct work_struct	work;
	struct bnxt		*bp;
	u32			session_id;
	u8			session_opcode;
};

struct bnxt_udcc_info {
	u32				max_sessions;
	struct bnxt_udcc_session_entry	*session_db[BNXT_UDCC_MAX_SESSIONS];
	/* to serialize adding to and deleting from the session_db */
	spinlock_t			session_db_lock;
	u32				session_count;
	struct dentry			*udcc_debugfs_dir;
};

int bnxt_alloc_udcc_info(struct bnxt *bp);
void bnxt_free_udcc_info(struct bnxt *bp);
void bnxt_udcc_session_db_cleanup(struct bnxt *bp);
void bnxt_udcc_task(struct work_struct *work);
int bnxt_hwrm_udcc_session_query(struct bnxt *bp, u32 session_id,
				 struct hwrm_udcc_session_query_output *resp_out);
#endif
