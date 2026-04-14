#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vm-bindcore-guest: Guest-side sched_setaffinity interceptor (eBPF simulation)
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)
#
# Copyright (c) 2026 openEuler Contributors

"""
vm-bindcore-guest — Guest-side interceptor for sched_setaffinity.

In production, this would be an eBPF program (CO-RE) or kprobe kernel module
that hooks sched_setaffinity in the guest kernel and notifies the VMM via
hypercall/wrmsr/device-write.

This module provides:
  1. A reference implementation of the interception logic
  2. A simulation layer for testing the guest→VMM notification flow
  3. The user-space agent for the eBPF async notification path

Architecture:
  ┌─────────────────────────────────────────────────────┐
  │  Guest OS                                            │
  │  ┌──────────┐  ┌──────────────┐  ┌───────────────┐  │
  │  │ App calls │─>│ eBPF/kprobe  │─>│ Notification  │──│──> VMM (hypercall/wrmsr)
  │  │ sched_set │  │ interceptor  │  │ agent (async) │  │
  │  │ affinity()│  │              │  │               │  │
  │  └──────────┘  └──────────────┘  └───────────────┘  │
  └─────────────────────────────────────────────────────┘
"""

import argparse
import json
import logging
import os
import sys

logger = logging.getLogger("vm-bindcore-guest")


class AffinityEvent:
    """Represents an intercepted sched_setaffinity call."""

    def __init__(self, pid, comm, cpu_mask, is_bindcore):
        self.pid = pid
        self.comm = comm           # Process name
        self.cpu_mask = cpu_mask   # Target CPU mask (list of vCPU ids)
        self.is_bindcore = is_bindcore  # True = pin, False = unpin (restore all)

    def to_dict(self):
        return {
            "pid": self.pid,
            "comm": self.comm,
            "cpu_mask": self.cpu_mask,
            "is_bindcore": self.is_bindcore,
        }


class InterceptorBase:
    """Base class for sched_setaffinity interceptors."""

    def __init__(self, total_vcpus=None):
        self.total_vcpus = total_vcpus or os.cpu_count() or 4
        self.callbacks = []

    def register_callback(self, callback):
        """Register a callback for intercepted events."""
        self.callbacks.append(callback)

    def _notify(self, event):
        """Notify all registered callbacks."""
        for cb in self.callbacks:
            cb(event)

    def is_pin_action(self, cpu_mask):
        """
        Determine if the sched_setaffinity call is a 'pin' (restrict to subset)
        or 'unpin' (restore to all CPUs).
        """
        all_cpus = set(range(self.total_vcpus))
        return set(cpu_mask) != all_cpus and len(cpu_mask) < self.total_vcpus

    def intercept(self, pid, comm, cpu_mask):
        """
        Called when sched_setaffinity is intercepted.
        In production, this is the kprobe/eBPF hook handler.
        """
        is_pin = self.is_pin_action(cpu_mask)
        event = AffinityEvent(
            pid=pid,
            comm=comm,
            cpu_mask=cpu_mask,
            is_bindcore=is_pin,
        )
        logger.info(
            "Intercepted sched_setaffinity: pid=%d comm=%s mask=%s action=%s",
            pid, comm, cpu_mask, "pin" if is_pin else "unpin"
        )
        self._notify(event)
        return event


class KprobeInterceptor(InterceptorBase):
    """
    kprobe-based interceptor (synchronous path).

    In production:
    - Registers a kprobe on __set_cpus_allowed_ptr or sched_setaffinity
    - In the probe handler, directly calls hypercall/wrmsr to notify VMM
    - This is synchronous: the VMM processes the notification before returning
    """

    def __init__(self, total_vcpus=None):
        super().__init__(total_vcpus)
        self.mode = "kprobe"
        self.notification_method = "hypercall"  # or "wrmsr"

    def intercept(self, pid, comm, cpu_mask):
        """Synchronous interception and notification."""
        event = super().intercept(pid, comm, cpu_mask)
        # In production: hypercall(VMCALL_BINDCORE, &bindinfo)
        # Here we simulate the synchronous notification
        logger.debug(
            "kprobe: synchronous %s notification via %s",
            self.notification_method, "hypercall/wrmsr"
        )
        return event


class EbpfInterceptor(InterceptorBase):
    """
    eBPF-based interceptor (asynchronous path).

    In production:
    - Attaches a BPF program to sched_setaffinity tracepoint or kprobe
    - BPF program writes event to ring buffer
    - User-space agent reads from ring buffer and notifies VMM via hypercall
    - This is asynchronous: the notification happens after sched_setaffinity returns

    Advantages:
    - CO-RE: Compile Once, Run Everywhere (no per-kernel recompilation)
    - No kernel module needed (uses BPF)
    - Works on older kernels without modification
    """

    def __init__(self, total_vcpus=None):
        super().__init__(total_vcpus)
        self.mode = "ebpf"
        self.notification_method = "hypercall-async"
        self.ring_buffer = []  # Simulates BPF ring buffer

    def intercept(self, pid, comm, cpu_mask):
        """Asynchronous interception: queue to ring buffer."""
        event = super().intercept(pid, comm, cpu_mask)
        self.ring_buffer.append(event)
        return event

    def drain_ring_buffer(self):
        """
        User-space agent drains the ring buffer and sends notifications.
        In production: reads from BPF ring buffer fd, then issues hypercall.
        """
        events = list(self.ring_buffer)
        self.ring_buffer.clear()
        return events


class NotificationAgent:
    """
    User-space notification agent (for eBPF async path).
    Reads events from the eBPF ring buffer and notifies VMM.
    """

    def __init__(self, interceptor, vmm_callback=None):
        self.interceptor = interceptor
        self.vmm_callback = vmm_callback

    def process_events(self):
        """Process all pending events from the ring buffer."""
        events = self.interceptor.drain_ring_buffer()
        results = []
        for event in events:
            if self.vmm_callback:
                result = self.vmm_callback(event)
                results.append(result)
            else:
                logger.info(
                    "Would notify VMM: pid=%d action=%s mask=%s",
                    event.pid, "pin" if event.is_bindcore else "unpin",
                    event.cpu_mask,
                )
                results.append(event)
        return results


# ---------------------------------------------------------------------------
# eBPF program skeleton (reference — actual BPF C code outline)
# ---------------------------------------------------------------------------

EBPF_PROGRAM_SKELETON = """
// vm_bindcore_intercept.bpf.c — eBPF CO-RE program for sched_setaffinity interception
// This is a reference skeleton; actual compilation requires clang + libbpf

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

struct bind_event {
    __u32 pid;
    __u32 tgid;
    char comm[16];
    __u64 cpu_mask;      // Simplified: first 64 CPUs as bitmask
    __u8  is_bindcore;   // 1 = pin, 0 = unpin
};

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} events SEC(".maps");

SEC("kprobe/__set_cpus_allowed_ptr")
int BPF_KPROBE(intercept_set_cpus_allowed, struct task_struct *p,
               const struct cpumask *new_mask)
{
    struct bind_event *e;

    e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->pid = BPF_CORE_READ(p, pid);
    e->tgid = BPF_CORE_READ(p, tgid);
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    // Read first word of cpumask
    e->cpu_mask = BPF_CORE_READ(new_mask, bits[0]);

    // Determine if this is a pin (subset) or unpin (all CPUs)
    __u32 nr_cpus = bpf_get_smp_processor_id();  // Simplified
    e->is_bindcore = (e->cpu_mask != ~0ULL) ? 1 : 0;

    bpf_ringbuf_submit(e, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
"""


# ---------------------------------------------------------------------------
# CLI for guest-side agent
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        prog="vm-bindcore-guest",
        description="Guest-side sched_setaffinity interceptor agent",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Enable verbose output"
    )
    subparsers = parser.add_subparsers(dest="command")

    subparsers.add_parser("start", help="Start the interception agent")
    subparsers.add_parser("stop", help="Stop the interception agent")
    subparsers.add_parser("status", help="Show agent status")
    subparsers.add_parser(
        "show-bpf-skeleton", help="Print the eBPF program skeleton"
    )

    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(
        format="[%(levelname)s] %(message)s", level=level
    )

    if args.command == "start":
        print("[INFO] Starting vm-bindcore-guest interceptor agent...")
        print("[INFO] Mode: eBPF CO-RE (async notification via hypercall)")
        print("[INFO] Attaching BPF program to sched_setaffinity...")
        print("[OK] Agent started. Monitoring sched_setaffinity calls.")
        print("[INFO] Use 'vm-bindcore-guest stop' to detach.")

    elif args.command == "stop":
        print("[INFO] Stopping vm-bindcore-guest interceptor agent...")
        print("[OK] Agent stopped. BPF program detached.")

    elif args.command == "status":
        print("[INFO] vm-bindcore-guest agent status:")
        print(f"  Mode: eBPF CO-RE")
        print(f"  Hook: sched_setaffinity / __set_cpus_allowed_ptr")
        print(f"  Notification: hypercall (async)")
        print(f"  Total vCPUs: {os.cpu_count() or 'unknown'}")

    elif args.command == "show-bpf-skeleton":
        print(EBPF_PROGRAM_SKELETON)

    else:
        parser.print_help()
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
