/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2019-2023 Broadcom
 * All rights reserved.
 */

#ifndef _ULP_MARK_MGR_H_
#define _ULP_MARK_MGR_H_

#include "bnxt_tf_ulp.h"

#define BNXT_ULP_MARK_VALID   0x1
#define BNXT_ULP_MARK_VFR_ID  0x2
#define BNXT_ULP_MARK_GLOBAL_HW_FID 0x4
#define BNXT_ULP_MARK_LOCAL_HW_FID 0x8

struct bnxt_lfid_mark_info {
	u16	mark_id;
	u16	flags;
};

struct bnxt_gfid_mark_info {
	u32	mark_id;
	u16	flags;
};

struct bnxt_ulp_mark_tbl {
	struct bnxt_lfid_mark_info	*lfid_tbl;
	struct bnxt_gfid_mark_info	*gfid_tbl;
	u32			lfid_num_entries;
	u32			gfid_num_entries;
	u32			gfid_mask;
	u32			gfid_type_bit;
};

int
ulp_mark_db_init(struct bnxt_ulp_context *ctxt);

int
ulp_mark_db_deinit(struct bnxt_ulp_context *ctxt);

int
ulp_mark_db_mark_get(struct bnxt_ulp_context *ctxt,
		     bool is_gfid,
		     u32 fid,
		     u32 *vfr_flag,
		     u32 *mark);

int
ulp_mark_db_mark_add(struct bnxt_ulp_context *ctxt,
		     u32 mark_flag,
		     u32 gfid,
		     u32 mark);

int
ulp_mark_db_mark_del(struct bnxt_ulp_context *ctxt,
		     u32 mark_flag,
		     u32 gfid);

#endif /* _ULP_MARK_MGR_H_ */
