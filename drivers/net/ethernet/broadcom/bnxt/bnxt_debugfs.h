/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2017-2018 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include "bnxt_hsi.h"
#include "bnxt.h"

#ifdef CONFIG_DEBUG_FS
void bnxt_debug_init(void);
void bnxt_debug_exit(void);
void bnxt_debug_dev_init(struct bnxt *bp);
void bnxt_debug_dev_exit(struct bnxt *bp);
void bnxt_debugfs_create_udcc_session(struct bnxt *bp, u32 session_id);
void bnxt_debugfs_delete_udcc_session(struct bnxt *bp, u32 session_id);
int bnxt_debug_bs_create_entries(struct bnxt *bp, uint32_t tsid);
#else
static inline void bnxt_debug_init(void) {}
static inline void bnxt_debug_exit(void) {}
static inline void bnxt_debug_dev_init(struct bnxt *bp) {}
static inline void bnxt_debug_dev_exit(struct bnxt *bp) {}
static inline void bnxt_debugfs_create_udcc_session(struct bnxt *bp, u32 session_id) {}
static inline void bnxt_debugfs_delete_udcc_session(struct bnxt *bp, u32 session_id) {}
static inline int bnxt_debug_bs_create_entries(struct bnxt *bp, uint32_t tsid) { return 0; }
#endif
