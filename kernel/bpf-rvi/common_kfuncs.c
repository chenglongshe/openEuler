// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2025 Huawei Technologies Co., Ltd */

#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/types.h>
#include <linux/btf_ids.h>
#include <linux/bpf.h>

/*
 * no padding member "_f" compared to struct sysinfo, because sizeof(_f)
 * maybe zero and not supported by bpf_verifier.
 */
struct bpf_sysinfo {
	__kernel_long_t uptime;		/* Seconds since boot */
	__kernel_ulong_t loads[3];	/* 1, 5, and 15 minute load averages */
	__kernel_ulong_t totalram;	/* Total usable main memory size */
	__kernel_ulong_t freeram;	/* Available memory size */
	__kernel_ulong_t sharedram;	/* Amount of shared memory */
	__kernel_ulong_t bufferram;	/* Memory used by buffers */
	__kernel_ulong_t totalswap;	/* Total swap space size */
	__kernel_ulong_t freeswap;	/* swap space still available */
	__u16 procs;			/* Number of current processes */
	__u16 pad;			/* Explicit padding for m68k */
	__kernel_ulong_t totalhigh;	/* Total high memory size */
	__kernel_ulong_t freehigh;	/* Available high memory size */
	__u32 mem_unit;			/* Memory unit size in bytes */
};

__bpf_kfunc void bpf_si_memswinfo(struct bpf_sysinfo *bsi)
{
	struct sysinfo *si = (struct sysinfo *)bsi;

	if (si) {
		si_meminfo(si);
		si_swapinfo(si);
	}
}

BTF_SET8_START(bpf_common_kfuncs_ids)
BTF_ID_FLAGS(func, bpf_si_memswinfo)
BTF_SET8_END(bpf_common_kfuncs_ids)

static const struct btf_kfunc_id_set bpf_common_kfuncs_set = {
	.owner		= THIS_MODULE,
	.set		= &bpf_common_kfuncs_ids,
};

static int __init bpf_common_kfuncs_init(void)
{
	return register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					 &bpf_common_kfuncs_set);
}
late_initcall(bpf_common_kfuncs_init);
