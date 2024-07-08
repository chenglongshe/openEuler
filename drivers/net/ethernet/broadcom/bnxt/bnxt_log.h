// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#ifndef BNXT_LOG_H
#define BNXT_LOG_H

#define BNXT_LOGGER_L2		1

int bnxt_register_logger(struct bnxt *bp, u16 logger_id, u32 num_buffers,
			 void (*log_live)(void *), u32 live_size);
void bnxt_unregister_logger(struct bnxt *bp, int logger_id);
void bnxt_log_add_msg(struct bnxt *bp, u16 logger_id, const char *format, ...);
void bnxt_log_live(struct bnxt *bp, u16 logger_id, const char *format, ...);
void bnxt_reset_loggers(struct bnxt *bp);
size_t bnxt_get_loggers_coredump_size(struct bnxt *bp);
void bnxt_start_logging_coredump(struct bnxt *bp, char *dest_buf);
#endif
