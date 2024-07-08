// SPDX-License-Identifier: GPL-2.0
/* Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2023 Broadcom Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/vmalloc.h>
#include <linux/errno.h>

#include "bnxt_compat.h"
#include "bnxt_hsi.h"
#include "bnxt.h"
#include "bnxt_coredump.h"

#define BNXT_LOG_MSG_SIZE	256
#define BNXT_LOG_NUM_BUFFERS(x)	((x) / BNXT_LOG_MSG_SIZE)

struct bnxt_logger {
	struct list_head list;
	u16 logger_id;
	u32 buffer_size;
	u16 head;
	u16 tail;
	bool valid;
	void *msgs;
	u32 live_max_size;
	void *live_msgs;
	u32 max_live_buff_size;
	u32 live_msgs_len;
	void (*log_live_op)(void *dev);
};

int bnxt_register_logger(struct bnxt *bp, u16 logger_id, u32 num_buffs,
			 void (*log_live)(void *), u32 live_max_size)
{
	struct bnxt_logger *logger;
	void *data;

	if (!log_live || !live_max_size)
		return -EINVAL;

	if (!is_power_of_2(num_buffs))
		return -EINVAL;

	logger = kzalloc(sizeof(*logger), GFP_KERNEL);
	if (!logger)
		return -ENOMEM;

	logger->logger_id = logger_id;
	logger->buffer_size = num_buffs * BNXT_LOG_MSG_SIZE;
	logger->log_live_op = log_live;
	logger->max_live_buff_size = live_max_size;

	data = vmalloc(logger->buffer_size);
	if (!data) {
		kfree(logger);
		return -ENOMEM;
	}
	logger->msgs = data;

	INIT_LIST_HEAD(&logger->list);
	mutex_lock(&bp->log_lock);
	list_add_tail(&logger->list, &bp->loggers_list);
	mutex_unlock(&bp->log_lock);
	return 0;
}

void bnxt_unregister_logger(struct bnxt *bp, int logger_id)
{
	struct bnxt_logger *l = NULL, *tmp;

	mutex_lock(&bp->log_lock);
	list_for_each_entry_safe(l, tmp, &bp->loggers_list, list) {
		if (l->logger_id == logger_id) {
			list_del(&l->list);
			break;
		}
	}
	mutex_unlock(&bp->log_lock);

	if (!l) {
		netdev_err(bp->dev, "logger id %d not registered\n", logger_id);
		return;
	}

	vfree(l->msgs);
	kfree(l);
}

static int bnxt_log_info(char *buf, size_t max_len, const char *format, va_list args)
{
	static char textbuf[BNXT_LOG_MSG_SIZE];
	char *text = textbuf;
	size_t text_len;
	char *next;

	text_len = vscnprintf(text, sizeof(textbuf), format, args);

	next = memchr(text, '\n', text_len);
	if (next)
		text_len = next - text;
	else if (text[text_len] == '\0')
		text[text_len] = '\n';

	if (text_len > max_len) {
		/* Truncate */
		text_len = max_len;
		text[text_len] = '\n';
	}

	memcpy(buf, text, text_len + 1);

	return text_len + 1;
}

void bnxt_log_add_msg(struct bnxt *bp, int logger_id, const char *format, ...)
{
	struct list_head *list_head, *pos, *lg;
	struct bnxt_logger *logger = NULL;
	u16 start, tail;
	va_list args;
	void *buf;
	u32 mask;

	mutex_lock(&bp->log_lock);
	list_head = &bp->loggers_list;
	list_for_each_safe(pos, lg, list_head) {
		logger = list_entry(pos, struct bnxt_logger, list);
		if (logger->logger_id == logger_id)
			break;
	}

	if (!logger) {
		mutex_unlock(&bp->log_lock);
		return;
	}

	mask = BNXT_LOG_NUM_BUFFERS(logger->buffer_size) - 1;
	tail = logger->tail;
	start = logger->head;

	if (logger->valid && start == tail)
		logger->head = ++start & mask;

	buf = logger->msgs + BNXT_LOG_MSG_SIZE * logger->tail;
	logger->tail = ++tail & mask;

	if (!logger->valid)
		logger->valid = true;

	va_start(args, format);
	bnxt_log_info(buf, BNXT_LOG_MSG_SIZE, format, args);
	va_end(args);
	mutex_unlock(&bp->log_lock);
}

void bnxt_log_live(struct bnxt *bp, int logger_id, const char *format, ...)
{
	struct list_head *head, *pos, *lg;
	struct bnxt_logger *logger = NULL;
	va_list args;
	int len;

	head = &bp->loggers_list;
	list_for_each_safe(pos, lg, head) {
		logger = list_entry(pos, struct bnxt_logger, list);
		if (logger->logger_id == logger_id)
			break;
	}

	if (!logger || !logger->live_msgs)
		return;

	va_start(args, format);
	len = bnxt_log_info(logger->live_msgs + logger->live_msgs_len,
			    logger->max_live_buff_size - logger->live_msgs_len,
			    format, args);
	va_end(args);

	logger->live_msgs_len += len;
}

static size_t bnxt_get_data_len(char *buf)
{
	size_t count = 0;

	while (*buf++ != '\n')
		count++;
	return count + 1;
}

static size_t bnxt_collect_logs_buffer(struct bnxt_logger *logger, char *dest)
{
	u32 mask = BNXT_LOG_NUM_BUFFERS(logger->buffer_size) - 1;
	u16 head = logger->head;
	u16 tail = logger->tail;
	size_t total_len = 0;
	int count;

	if (!logger->valid)
		return 0;

	count = (tail > head) ? (tail - head) : (tail - head + mask + 1);
	while (count--) {
		void *src = logger->msgs + BNXT_LOG_MSG_SIZE * (head & mask);
		size_t len;

		len = bnxt_get_data_len(src);
		memcpy(dest + total_len, src, len);
		total_len += len;
		head++;
	}

	return total_len;
}

size_t bnxt_get_loggers_coredump_size(struct bnxt *bp)
{
	struct list_head *head, *pos, *lg;
	struct bnxt_logger *logger;
	size_t len = 0;

	mutex_lock(&bp->log_lock);
	head = &bp->loggers_list;
	list_for_each_safe(pos, lg, head) {
		logger = list_entry(pos, struct bnxt_logger, list);
		len += sizeof(struct bnxt_coredump_segment_hdr) +
		       logger->max_live_buff_size + logger->buffer_size;
	}
	mutex_unlock(&bp->log_lock);
	return len;
}

void bnxt_start_logging_coredump(struct bnxt *bp, char *dest_buf)
{
	struct list_head *head, *pos, *lg;
	struct bnxt_logger *logger;
	size_t offset = 0;
	u32 seg_id = 0;

	if (!dest_buf)
		return;

	mutex_lock(&bp->log_lock);
	head = &bp->loggers_list;
	list_for_each_safe(pos, lg, head) {
		struct bnxt_coredump_segment_hdr seg_hdr;
		void *seg_hdr_dest = dest_buf + offset;
		size_t len;

		logger = list_entry(pos, struct bnxt_logger, list);

		offset += sizeof(seg_hdr);
		/* First collect logs from buffer */
		len = bnxt_collect_logs_buffer(logger, dest_buf + offset);
		offset += len;
		/* Let logger to collect live messages */
		logger->live_msgs = dest_buf + offset;
		logger->live_msgs_len = 0;
		logger->log_live_op(bp);

		len += logger->live_msgs_len;
		offset += logger->live_msgs_len;

		bnxt_fill_coredump_seg_hdr(bp, &seg_hdr, NULL, len,
					   0, 0, 0, 13);
		seg_hdr.segment_id = cpu_to_le32(seg_id);
		memcpy(seg_hdr_dest, &seg_hdr, sizeof(seg_hdr));
		seg_id++;
	}
	mutex_unlock(&bp->log_lock);
}

void bnxt_reset_loggers(struct bnxt *bp)
{
	struct list_head *head, *pos, *lg;
	struct bnxt_logger *logger;

	mutex_lock(&bp->log_lock);
	head = &bp->loggers_list;
	list_for_each_safe(pos, lg, head) {
		logger = list_entry(pos, struct bnxt_logger, list);
		logger->head = 0;
		logger->tail = 0;
		logger->valid = false;
	}
	mutex_unlock(&bp->log_lock);
}
