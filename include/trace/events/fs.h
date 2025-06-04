/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM fs

#if !defined(_TRACE_FS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_FS_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <linux/fs.h>

#undef FS_DECLARE_TRACE
#ifdef DECLARE_TRACE_WRITABLE
#define FS_DECLARE_TRACE(call, proto, args, size) \
	DECLARE_TRACE_WRITABLE(call, PARAMS(proto), PARAMS(args), size)
#else
#define FS_DECLARE_TRACE(call, proto, args, size) \
	DECLARE_TRACE(call, PARAMS(proto), PARAMS(args))
#endif

FS_DECLARE_TRACE(fs_file_read,
	TP_PROTO(struct fs_file_read_ctx *ctx, int version),
	TP_ARGS(ctx, version),
	sizeof(struct fs_file_read_ctx));

DECLARE_TRACE(fs_file_release,
	TP_PROTO(struct inode *inode, struct file *filp),
	TP_ARGS(inode, filp));

#endif /* _TRACE_FS_H */

TRACE_EVENT(epoll_rc_queue,

	TP_PROTO(struct file *file, int cpu),

	TP_ARGS(file, cpu),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->cpu = cpu;
	),

	TP_printk("0x%p on cpu %d", __entry->file, __entry->cpu)
);

TRACE_EVENT(epoll_rc_prefetch,

	TP_PROTO(struct file *file, int cpu),

	TP_ARGS(file, cpu),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->cpu = cpu;
	),

	TP_printk("0x%p on cpu %d", __entry->file, __entry->cpu)
);

TRACE_EVENT(epoll_rc_ready,

	TP_PROTO(struct file *file, ssize_t len),

	TP_ARGS(file, len),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(ssize_t, len)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->len = len;
	),

	TP_printk("0x%p, len %ld", __entry->file, __entry->len)
);

TRACE_EVENT(epoll_rc_hit,

	TP_PROTO(struct file *file, ssize_t len),

	TP_ARGS(file, len),

	TP_STRUCT__entry(
		__field(struct file *, file)
		__field(ssize_t, len)
	),

	TP_fast_assign(
		__entry->file = file;
		__entry->len = len;
	),

	TP_printk("0x%p, len: %ld", __entry->file, __entry->len)
);

TRACE_EVENT(epoll_rc_miss,

	TP_PROTO(struct file *file),

	TP_ARGS(file),

	TP_STRUCT__entry(
		__field(struct file *, file)
	),

	TP_fast_assign(
		__entry->file = file;
	),

	TP_printk("0x%p", __entry->file)
);

/* This part must be outside protection */
#include <trace/define_trace.h>
