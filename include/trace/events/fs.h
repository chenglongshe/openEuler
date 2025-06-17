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

TRACE_EVENT(epoll_rc_ready,

	TP_PROTO(int fd, int len),

	TP_ARGS(fd, len),

	TP_STRUCT__entry(
		__field(int, fd)
		__field(int, len)
	),

	TP_fast_assign(
		__entry->fd = fd;
		__entry->len = len;
	),

	TP_printk("%d, len %d", __entry->fd, __entry->len)
);

TRACE_EVENT(epoll_rc_queue,

	TP_PROTO(int fd, int cpu),

	TP_ARGS(fd, cpu),

	TP_STRUCT__entry(
		__field(int, fd)
		__field(int, cpu)
	),

	TP_fast_assign(
		__entry->fd = fd;
		__entry->cpu = cpu;
	),

	TP_printk("%d on cpu %d", __entry->fd, __entry->cpu)
);

TRACE_EVENT(epoll_rc_hit,

	TP_PROTO(int fd, int len),

	TP_ARGS(fd, len),

	TP_STRUCT__entry(
		__field(int, fd)
		__field(int, len)
	),

	TP_fast_assign(
		__entry->fd = fd;
		__entry->len = len;
	),

	TP_printk("%d, len: %d", __entry->fd, __entry->len)
);

TRACE_EVENT(epoll_rc_miss,

	TP_PROTO(int fd),

	TP_ARGS(fd),

	TP_STRUCT__entry(
		__field(int, fd)
	),

	TP_fast_assign(
		__entry->fd = fd;
	),

	TP_printk("%d", __entry->fd)
);

TRACE_EVENT(epoll_rc_wait,

	TP_PROTO(int fd),

	TP_ARGS(fd),

	TP_STRUCT__entry(
		__field(int, fd)
	),

	TP_fast_assign(
		__entry->fd = fd;
	),

	TP_printk("%d", __entry->fd)
);

/* This part must be outside protection */
#include <trace/define_trace.h>
