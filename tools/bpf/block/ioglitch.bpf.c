// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
// Copyright (C) 2025 KylinSoft Co., Ltd. All rights reserved.

#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_helpers.h>
#include <linux/errno.h>

#include "ioglitch.h"

const volatile unsigned int target_dev = 0;

struct trace_event_block_latency {
	short res_s;
	char res_c[2];
	int res_i;
	u32 dev;
	u64 sector;
	u32 nr_sector;
	int error;
	u64 time_ns[STAGE_TOTAL_NR];
	char rwbs[8];
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, struct hist_key);
	__type(value, struct hist);
} hists SEC(".maps");

static struct hist zero;

static __u32 get_latency_us(u64 start, u64 end)
{
	if (start == 0 || end == 0)
		return 0;

	return (start > end ? 0 : end - start) /
	       BLK_NS_TO_US;
}

static __always_inline void *
bpf_map_lookup_or_try_init(void *map, const void *key, const void *init)
{
	void *val;
	/* bpf helper functions like bpf_map_update_elem() below normally return
	 * long, but using int instead of long to store the result is a workaround
	 * to avoid incorrectly evaluating err in cases where the following criteria
	 * is met:
	 *     the architecture is 64-bit
	 *     the helper function return type is long
	 *     the helper function returns the value of a call to a bpf_map_ops func
	 *     the bpf_map_ops function return type is int
	 *     the compiler inlines the helper function
	 *     the compiler does not sign extend the result of the bpf_map_ops func
	 *
	 * if this criteria is met, at best an error can only be checked as zero or
	 * non-zero. it will not be possible to check for a negative value or a
	 * specific error value. this is because the sign bit would have been stuck
	 * at the 32nd bit of a 64-bit long int.
	 */
	int err;

	val = bpf_map_lookup_elem(map, key);
	if (val)
		return val;

	err = bpf_map_update_elem(map, key, init, BPF_NOEXIST);
	if (err && err != -EEXIST)
		return 0;

	return bpf_map_lookup_elem(map, key);
}

static __always_inline __u64 log2(__u32 v)
{
	__u32 shift, r;

	r = (v > 0xFFFF) << 4; v >>= r;
	shift = (v > 0xFF) << 3; v >>= shift; r |= shift;
	shift = (v > 0xF) << 2; v >>= shift; r |= shift;
	shift = (v > 0x3) << 1; v >>= shift; r |= shift;
	r |= (v >> 1);

	return r;
}

static void storage_slot(u32 latency, u32 *value)
{
	u64 slot;

	if (!latency)
		return;

	slot = log2(latency);
	if (slot >= MAX_SLOTS)
		slot = MAX_SLOTS - 1;

	__sync_fetch_and_add(&value[slot], 1);
}

static void storage_latency_info(struct latency_info *latency_info, u32 last_latency,
				 struct trace_event_block_latency *ctx)
{
	if (latency_info->max_latency > last_latency)
		return;

	latency_info->max_latency = last_latency;
	__builtin_memcpy(latency_info->rwbs, ctx->rwbs, sizeof(latency_info->rwbs));
	latency_info->sector = ctx->sector;
	latency_info->nr_sector = ctx->nr_sector;
}

static void storge_info(u64 start, u64 end, u32 *value, struct latency_info *latency_info,
			struct trace_event_block_latency *ctx)
{
	u32 latency;

	latency = get_latency_us(start, end);

	storage_slot(latency, value);

	storage_latency_info(latency_info, latency, ctx);
}

SEC("tracepoint/block/block_io_glitch_detection")
int handle_block_io_glitch_detection(void *args)
{
	u64 *time_ns;
	u64 now;
	u32 dev;
	struct hist_key hkey = {0};
	struct hist *histp;

	struct trace_event_block_latency *ctx = args;

	dev = ctx->dev;
	if (target_dev != dev)
		return 0;

	now = bpf_ktime_get_ns();
	time_ns = ctx->time_ns;

	hkey.dev = ctx->dev;
	__builtin_memcpy(&hkey.rwbs, ctx->rwbs, sizeof(hkey.rwbs));
	histp = bpf_map_lookup_or_try_init(&hists, &hkey, &zero);
	if (!histp)
		return 0;

	/* d2c*/
	storge_info(time_ns[STAGE_RQ_ISSUE], now, histp->issue_to_complete,
		    &histp->latency_info[D2C], ctx);
	/* a2c */
	storge_info(time_ns[STAGE_BIO_ALLOC], now, histp->alloc_to_complete,
		    &histp->latency_info[A2C], ctx);
	/* a2t */
	storge_info(time_ns[STAGE_BIO_ALLOC], time_ns[STAGE_BIO_TH_END],
		    histp->alloc_to_throend, &histp->latency_info[A2T], ctx);
	/* t2r */
	storge_info(time_ns[STAGE_BIO_TH_END], time_ns[STAGE_BIO_RQS_END],
		    histp->throend_to_rqsend, &histp->latency_info[T2R], ctx);
	/* r2g */
	storge_info(time_ns[STAGE_BIO_RQS_END], time_ns[STAGE_RQ_GETRQ],
		    histp->rqsend_to_getrq, &histp->latency_info[R2G], ctx);
	/* g2i */
	storge_info(time_ns[STAGE_RQ_GETRQ], time_ns[STAGE_RQ_SCHED],
		    histp->getrq_to_insert, &histp->latency_info[G2I], ctx);
	/* i2d */
	storge_info(time_ns[STAGE_RQ_SCHED], time_ns[STAGE_RQ_ISSUE],
		    histp->insert_to_issue, &histp->latency_info[I2D], ctx);
	/* g2d */
	storge_info(time_ns[STAGE_RQ_GETRQ], time_ns[STAGE_RQ_ISSUE],
		    histp->getrq_to_issue, &histp->latency_info[G2D], ctx);

	return 0;
}

char _license[] SEC("license") = "GPL";
