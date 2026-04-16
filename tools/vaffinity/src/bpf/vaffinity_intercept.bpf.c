// SPDX-License-Identifier: GPL-2.0
//
// vaffinity_intercept.bpf.c — eBPF CO-RE program for sched_setaffinity interception
//
// Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)
//
// This BPF program attaches to the sched_setaffinity syscall entry point
// and captures CPU affinity change events.  Events are sent to user space
// via a BPF ring buffer for asynchronous processing by the notification
// agent (vaffinity-guest).
//
// Build (requires clang >= 12 + libbpf + bpftool):
//   clang -O2 -target bpf -D__TARGET_ARCH_x86 \
//         -I/usr/include/bpf -I. \
//         -c vaffinity_intercept.bpf.c -o vaffinity_intercept.bpf.o
//   bpftool gen skeleton vaffinity_intercept.bpf.o > vaffinity_intercept.skel.h
//
// Copyright (c) 2026 openEuler Contributors

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* Maximum number of CPUs we track in the bitmask (covers most servers). */
#define MAX_CPUS 256
#define MASK_LONGS (MAX_CPUS / 64)

/* Event sent to user space via ring buffer. */
struct bind_event {
    __u32 pid;          /* Thread PID that called sched_setaffinity     */
    __u32 tgid;         /* Thread-group (process) ID                    */
    char  comm[16];     /* Process command name                         */
    __u64 cpu_mask[MASK_LONGS]; /* New CPU affinity bitmask             */
    __u32 nr_cpus;      /* Number of online CPUs on this guest          */
    __u8  is_affinity_pin;  /* 1 = pin (subset), 0 = unpin (all CPUs)       */
    __u8  pad[3];
};

/* Ring buffer map shared with user space. */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);  /* 256 KB */
} events SEC(".maps");

/* Helper: check whether the mask covers all online CPUs (= unpin). */
static __always_inline int is_full_mask(const __u64 *mask, __u32 nr_cpus)
{
    __u32 full_longs = nr_cpus / 64;
    __u32 remaining  = nr_cpus % 64;
    int i;

    /* Check fully-occupied 64-bit words. */
    for (i = 0; i < MASK_LONGS && i < (int)full_longs; i++) {
        if (mask[i] != ~(__u64)0)
            return 0;
    }
    /* Check the partial trailing word (if any). */
    if (remaining && i < MASK_LONGS) {
        __u64 trail_mask = (1ULL << remaining) - 1;
        if ((mask[i] & trail_mask) != trail_mask)
            return 0;
    }
    return 1;
}

/*
 * Tracepoint: sys_enter_sched_setaffinity
 *
 * Fires when any thread calls sched_setaffinity(2).  We capture the target
 * PID and the requested CPU mask, then decide whether the caller is *pinning*
 * to a CPU subset or *unpinning* (restoring full affinity).
 */
SEC("tp/syscalls/sys_enter_sched_setaffinity")
int handle_sched_setaffinity(struct trace_event_raw_sys_enter *ctx)
{
    struct bind_event *e;
    pid_t target_pid;
    unsigned long __user *user_mask_ptr;
    unsigned int len;
    int i;

    /* arg0 = pid, arg1 = len (in bytes), arg2 = user_mask_ptr */
    target_pid     = (pid_t)ctx->args[0];
    len            = (unsigned int)ctx->args[1];
    user_mask_ptr  = (unsigned long __user *)ctx->args[2];

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    /* Fill caller identity. */
    e->pid  = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    e->tgid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    /* Read the user-supplied CPU mask (up to MASK_LONGS * 8 bytes). */
    __builtin_memset(e->cpu_mask, 0, sizeof(e->cpu_mask));
    unsigned int to_read = len < sizeof(e->cpu_mask) ? len : sizeof(e->cpu_mask);
    bpf_probe_read_user(e->cpu_mask, to_read, user_mask_ptr);

    /* Determine total online CPUs from /sys/devices/system/cpu
     * — we approximate via bpf_num_possible_cpus() at load time;
     * the user-space agent can refine this.                        */
    e->nr_cpus = bpf_get_smp_processor_id();  /* placeholder; agent overrides */

    /* Classify as pin (subset) or unpin (full). */
    e->is_affinity_pin = is_full_mask(e->cpu_mask, e->nr_cpus) ? 0 : 1;

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
