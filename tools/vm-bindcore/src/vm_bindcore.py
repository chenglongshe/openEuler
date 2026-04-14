#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vm-bindcore: VMM-side handler for transparent in-VM pinning passthrough
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)
#
# Copyright (c) 2026 openEuler Contributors

"""
vm-bindcore — VMM-side transparent vCPU pinning optimization tool.

Architecture (from patent):
  1. Guest app calls sched_setaffinity() to pin to specific vCPU(s)
  2. Guest-side interceptor (kprobe/eBPF) captures the pinning action
  3. Interceptor notifies VMM via hypercall/wrmsr/emulated-device
  4. VMM handler (this tool) receives notification and:
     - Looks up Global CPU Map + VM cpuset
     - Dynamically switches the vCPU from range-pinning to 1:1 pinning
  5. When guest app un-pins, VMM restores range-pinning for the vCPU

This module implements the VMM-side logic: global CPU map management,
dynamic 1:1 pinning/un-pinning, conflict avoidance, and status reporting.
"""

import argparse
import json
import logging
import os
import re
import sys
from pathlib import Path

CONF_DIR = "/etc/vm-bindcore"
GLOBAL_MAP_FILE = os.path.join(CONF_DIR, "global_cpu_map.json")
LOG_FORMAT = "[%(levelname)s] %(message)s"
JOURNAL_FORMAT = (
    "%(asctime)s %(hostname)s vm-bindcore[%(process)d]: "
    "[%(action)s] vm=%(vm)s vcpu=%(vcpu)s %(details)s"
)

logger = logging.getLogger("vm-bindcore")


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

class PinMode:
    """vCPU pinning modes on the Host side."""
    RANGE = "range"          # vCPU scheduled across cpuset (default)
    EXCLUSIVE = "exclusive"  # vCPU 1:1 pinned to a single pCPU


class PinSource:
    """Source of pinning action."""
    DEFAULT = "default"      # System default range pinning
    GUEST_PIN = "guest-pin"  # Triggered by in-VM app sched_setaffinity
    MANUAL = "manual"        # Manually set by admin


class VcpuPinState:
    """Tracks the pinning state of a single vCPU."""

    def __init__(self, vcpu_id, cpuset):
        self.vcpu_id = vcpu_id
        self.mode = PinMode.RANGE
        self.cpuset = list(cpuset)   # Allowed pCPU range
        self.pinned_pcpu = None      # pCPU for 1:1 mode, None for range
        self.source = PinSource.DEFAULT
        self.guest_pid = None        # PID of the app inside guest (if known)

    def to_dict(self):
        return {
            "vcpu_id": self.vcpu_id,
            "mode": self.mode,
            "cpuset": self.cpuset,
            "pinned_pcpu": self.pinned_pcpu,
            "source": self.source,
            "guest_pid": self.guest_pid,
        }

    @classmethod
    def from_dict(cls, d):
        state = cls(d["vcpu_id"], d["cpuset"])
        state.mode = d.get("mode", PinMode.RANGE)
        state.pinned_pcpu = d.get("pinned_pcpu")
        state.source = d.get("source", PinSource.DEFAULT)
        state.guest_pid = d.get("guest_pid")
        return state


class VmPinState:
    """Tracks the pinning state of all vCPUs in a VM."""

    def __init__(self, domain, vcpu_count, cpuset):
        self.domain = domain
        self.vcpu_count = vcpu_count
        self.cpuset = list(cpuset)
        self.vcpus = {
            i: VcpuPinState(i, cpuset) for i in range(vcpu_count)
        }

    def to_dict(self):
        return {
            "domain": self.domain,
            "vcpu_count": self.vcpu_count,
            "cpuset": self.cpuset,
            "vcpus": {str(k): v.to_dict() for k, v in self.vcpus.items()},
        }

    @classmethod
    def from_dict(cls, d):
        vm = cls(d["domain"], d["vcpu_count"], d["cpuset"])
        vm.vcpus = {
            int(k): VcpuPinState.from_dict(v)
            for k, v in d.get("vcpus", {}).items()
        }
        return vm


# ---------------------------------------------------------------------------
# Global CPU Map — tracks which pCPUs are exclusively occupied
# ---------------------------------------------------------------------------

class GlobalCpuMap:
    """
    Maintains a global map of all pCPU assignments across all VMs.

    Key data:
      exclusive_map: {pcpu_id: (vm_domain, vcpu_id)}  — 1:1 occupied pCPUs
      vm_states:     {vm_domain: VmPinState}           — per-VM pinning info
    """

    def __init__(self, total_pcpus=None):
        self.total_pcpus = total_pcpus or os.cpu_count() or 64
        self.exclusive_map = {}   # {pcpu: (domain, vcpu_id)}
        self.vm_states = {}       # {domain: VmPinState}

    # --- Persistence ---

    def save(self, path=GLOBAL_MAP_FILE):
        """Persist the global CPU map to disk."""
        data = {
            "total_pcpus": self.total_pcpus,
            "exclusive_map": {
                str(k): list(v) for k, v in self.exclusive_map.items()
            },
            "vm_states": {
                k: v.to_dict() for k, v in self.vm_states.items()
            },
        }
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            json.dump(data, f, indent=2)

    @classmethod
    def load(cls, path=GLOBAL_MAP_FILE):
        """Load the global CPU map from disk."""
        if not os.path.exists(path):
            return cls()
        with open(path) as f:
            data = json.load(f)
        gm = cls(total_pcpus=data.get("total_pcpus"))
        gm.exclusive_map = {
            int(k): tuple(v)
            for k, v in data.get("exclusive_map", {}).items()
        }
        gm.vm_states = {
            k: VmPinState.from_dict(v)
            for k, v in data.get("vm_states", {}).items()
        }
        return gm

    # --- VM registration ---

    def register_vm(self, domain, vcpu_count, cpuset):
        """Register a VM with its vCPU count and cpuset."""
        if domain not in self.vm_states:
            self.vm_states[domain] = VmPinState(domain, vcpu_count, cpuset)
        return self.vm_states[domain]

    def unregister_vm(self, domain):
        """Remove a VM and release all its exclusive pCPUs."""
        vm = self.vm_states.pop(domain, None)
        if vm:
            for vcpu_state in vm.vcpus.values():
                if vcpu_state.pinned_pcpu is not None:
                    self.exclusive_map.pop(vcpu_state.pinned_pcpu, None)

    # --- Core pinning logic ---

    def find_free_pcpu(self, cpuset):
        """
        Find a free (non-exclusively-occupied) pCPU within the given cpuset.
        Returns pCPU id or None if all are occupied.
        """
        for pcpu in cpuset:
            if pcpu not in self.exclusive_map:
                return pcpu
        return None

    def pin_exclusive(self, domain, vcpu_id, guest_pid=None):
        """
        Switch a vCPU from range mode to exclusive 1:1 mode.
        Called when Guest app binds to a specific vCPU.

        Returns (success: bool, pcpu: int or None, message: str)
        """
        vm = self.vm_states.get(domain)
        if vm is None:
            return False, None, f"VM '{domain}' not registered"

        vcpu = vm.vcpus.get(vcpu_id)
        if vcpu is None:
            return False, None, f"vCPU {vcpu_id} not found in '{domain}'"

        if vcpu.mode == PinMode.EXCLUSIVE:
            return True, vcpu.pinned_pcpu, f"vCPU {vcpu_id} already exclusive on pCPU {vcpu.pinned_pcpu}"

        # Find free pCPU in this VM's cpuset
        pcpu = self.find_free_pcpu(vm.cpuset)
        if pcpu is None:
            msg = (
                f"No free pCPU in cpuset {_format_cpulist(vm.cpuset)} "
                f"for {domain}:vcpu{vcpu_id} — keeping range mode"
            )
            logger.warning(msg)
            return False, None, msg

        # Execute 1:1 pinning
        vcpu.mode = PinMode.EXCLUSIVE
        vcpu.pinned_pcpu = pcpu
        vcpu.source = PinSource.GUEST_PIN
        vcpu.guest_pid = guest_pid
        self.exclusive_map[pcpu] = (domain, vcpu_id)

        logger.info(
            "[PIN] vm=%s vcpu=%d action=exclusive pcpu=%d source=guest-pin pid=%s",
            domain, vcpu_id, pcpu, guest_pid,
        )
        return True, pcpu, f"Pinned vcpu{vcpu_id} -> pCPU {pcpu}"

    def unpin_restore(self, domain, vcpu_id):
        """
        Restore a vCPU from exclusive 1:1 mode back to range mode.
        Called when Guest app un-pins (restores to all vCPUs).

        Returns (success: bool, message: str)
        """
        vm = self.vm_states.get(domain)
        if vm is None:
            return False, f"VM '{domain}' not registered"

        vcpu = vm.vcpus.get(vcpu_id)
        if vcpu is None:
            return False, f"vCPU {vcpu_id} not found in '{domain}'"

        if vcpu.mode != PinMode.EXCLUSIVE:
            return True, f"vCPU {vcpu_id} already in range mode"

        old_pcpu = vcpu.pinned_pcpu
        # Release exclusive pCPU
        if old_pcpu is not None:
            self.exclusive_map.pop(old_pcpu, None)

        vcpu.mode = PinMode.RANGE
        vcpu.pinned_pcpu = None
        vcpu.source = PinSource.DEFAULT
        vcpu.guest_pid = None

        logger.info(
            "[UNPIN] vm=%s vcpu=%d action=restore cpuset=%s source=guest-unpin",
            domain, vcpu_id, _format_cpulist(vm.cpuset),
        )
        return True, f"Restored vcpu{vcpu_id} to range mode (cpuset {_format_cpulist(vm.cpuset)})"

    def unpin_all(self, domain):
        """Restore all vCPUs of a VM to range mode."""
        vm = self.vm_states.get(domain)
        if vm is None:
            return False, f"VM '{domain}' not registered"
        results = []
        for vcpu_id in sorted(vm.vcpus.keys()):
            ok, msg = self.unpin_restore(domain, vcpu_id)
            results.append(msg)
        return True, results

    # --- Query ---

    def get_vm_status(self, domain):
        """Return the pinning status of a VM."""
        return self.vm_states.get(domain)

    def get_pcpu_status(self, pcpu):
        """Return the status of a specific pCPU."""
        if pcpu in self.exclusive_map:
            domain, vcpu_id = self.exclusive_map[pcpu]
            return PinMode.EXCLUSIVE, domain, vcpu_id
        return PinMode.RANGE, None, None

    def get_all_pcpu_status(self):
        """Return status of all pCPUs."""
        result = {}
        for pcpu in range(self.total_pcpus):
            mode, domain, vcpu_id = self.get_pcpu_status(pcpu)
            sharing_vms = set()
            if mode == PinMode.RANGE:
                for vm in self.vm_states.values():
                    if pcpu in vm.cpuset:
                        sharing_vms.add(vm.domain)
            result[pcpu] = {
                "mode": mode,
                "domain": domain,
                "vcpu_id": vcpu_id,
                "sharing_vms": sorted(sharing_vms),
            }
        return result


# ---------------------------------------------------------------------------
# Guest-side pinning notification (simulates what KVM/QEMU would receive)
# ---------------------------------------------------------------------------

class GuestPinNotification:
    """
    Represents a pinning notification from the Guest to the VMM.
    In production, this comes via hypercall/wrmsr/device-write causing VM-Exit.
    Here we model it as a structured message for the VMM handler.
    """
    ACTION_PIN = "pin"
    ACTION_UNPIN = "unpin"

    def __init__(self, domain, vcpu_id, action, guest_pid=None, target_vcpus=None):
        self.domain = domain
        self.vcpu_id = vcpu_id          # Which vCPU the app pinned to
        self.action = action            # "pin" or "unpin"
        self.guest_pid = guest_pid      # PID of app inside guest
        self.target_vcpus = target_vcpus  # Bitmask of target vCPUs (for info)

    def to_dict(self):
        return {
            "domain": self.domain,
            "vcpu_id": self.vcpu_id,
            "action": self.action,
            "guest_pid": self.guest_pid,
            "target_vcpus": self.target_vcpus,
        }

    @classmethod
    def from_dict(cls, d):
        return cls(
            domain=d["domain"],
            vcpu_id=d["vcpu_id"],
            action=d["action"],
            guest_pid=d.get("guest_pid"),
            target_vcpus=d.get("target_vcpus"),
        )


class VmmPinHandler:
    """
    VMM-side handler that processes guest pinning notifications.
    This is the core logic unit described in the patent's architecture.
    """

    def __init__(self, global_map):
        self.global_map = global_map

    def handle_notification(self, notification):
        """
        Process a single guest pinning notification.
        Returns (success, message) tuple.
        """
        if notification.action == GuestPinNotification.ACTION_PIN:
            ok, pcpu, msg = self.global_map.pin_exclusive(
                notification.domain,
                notification.vcpu_id,
                guest_pid=notification.guest_pid,
            )
            return ok, msg
        elif notification.action == GuestPinNotification.ACTION_UNPIN:
            ok, msg = self.global_map.unpin_restore(
                notification.domain,
                notification.vcpu_id,
            )
            return ok, msg
        else:
            return False, f"Unknown action: {notification.action}"

    def handle_batch(self, notifications):
        """Process a batch of notifications."""
        results = []
        for notif in notifications:
            ok, msg = self.handle_notification(notif)
            results.append({"notification": notif.to_dict(), "ok": ok, "msg": msg})
        return results


# ---------------------------------------------------------------------------
# Utility functions
# ---------------------------------------------------------------------------

def _format_cpulist(cpus):
    """Format a list of CPUs into a compact range string."""
    if not cpus:
        return "(none)"
    cpus = sorted(cpus)
    ranges = []
    start = cpus[0]
    end = cpus[0]
    for c in cpus[1:]:
        if c == end + 1:
            end = c
        else:
            ranges.append(f"{start}-{end}" if start != end else str(start))
            start = c
            end = c
    ranges.append(f"{start}-{end}" if start != end else str(start))
    return ",".join(ranges)


def _parse_cpulist(cpulist_str):
    """Parse a CPU list string like '0-3,8-11' into a sorted list of ints."""
    cpus = []
    for part in cpulist_str.split(","):
        part = part.strip()
        if "-" in part:
            start, end = part.split("-", 1)
            cpus.extend(range(int(start), int(end) + 1))
        else:
            cpus.append(int(part))
    return sorted(cpus)


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_register(args):
    """Register a VM in the global CPU map."""
    gm = GlobalCpuMap.load()
    cpuset = _parse_cpulist(args.cpuset)
    gm.register_vm(args.vm, args.vcpus, cpuset)
    gm.save()
    print(f"[OK] Registered {args.vm}: {args.vcpus} vCPUs, cpuset {_format_cpulist(cpuset)}")


def cmd_unregister(args):
    """Unregister a VM from the global CPU map."""
    gm = GlobalCpuMap.load()
    gm.unregister_vm(args.vm)
    gm.save()
    print(f"[OK] Unregistered {args.vm}")


def cmd_notify(args):
    """Simulate receiving a guest pinning notification (for testing/manual use)."""
    gm = GlobalCpuMap.load()
    handler = VmmPinHandler(gm)

    notification = GuestPinNotification(
        domain=args.vm,
        vcpu_id=args.vcpu,
        action=args.action,
        guest_pid=args.pid,
    )
    ok, msg = handler.handle_notification(notification)
    gm.save()

    if ok:
        print(f"[OK] {msg}")
    else:
        print(f"[ERROR] {msg}", file=sys.stderr)
        return 1


def cmd_status(args):
    """Show pinning status for a VM or all VMs."""
    gm = GlobalCpuMap.load()

    if args.vm:
        vm = gm.get_vm_status(args.vm)
        if vm is None:
            print(f"VM '{args.vm}' not found in global map")
            return 1
        _print_vm_status(vm, args.format)
    else:
        if not gm.vm_states:
            print("No active VMs found")
            return 0
        print(f"Host: {gm.total_pcpus} pCPUs")
        print()
        for domain in sorted(gm.vm_states.keys()):
            _print_vm_status(gm.vm_states[domain], args.format)
            print()


def _print_vm_status(vm, fmt="table"):
    """Print a single VM's pinning status."""
    if fmt == "json":
        print(json.dumps(vm.to_dict(), indent=2))
        return

    print(f"VM: {vm.domain} ({vm.vcpu_count} vCPUs, cpuset: {_format_cpulist(vm.cpuset)})")
    print(f"{'vCPU':<8}{'Mode':<12}{'pCPU':<16}{'Source'}")
    print(f"{'----':<8}{'----':<12}{'----':<16}{'------'}")

    for vcpu_id in sorted(vm.vcpus.keys()):
        vs = vm.vcpus[vcpu_id]
        if vs.mode == PinMode.EXCLUSIVE:
            pcpu_str = f"pCPU {vs.pinned_pcpu}"
            source_str = f"{vs.source}"
            if vs.guest_pid:
                source_str += f" (PID {vs.guest_pid})"
        else:
            pcpu_str = f"pCPU {_format_cpulist(vs.cpuset)}"
            source_str = vs.source

        print(f"vcpu{vcpu_id:<4}{vs.mode:<12}{pcpu_str:<16}{source_str}")


def cmd_map(args):
    """Show the global CPU map."""
    gm = GlobalCpuMap.load()

    if args.format == "json":
        all_status = gm.get_all_pcpu_status()
        print(json.dumps(all_status, indent=2, default=str))
        return

    print(f"Global CPU Pinning Map ({gm.total_pcpus} pCPUs)")
    print(f"{'pCPU':<8}{'Status':<12}{'Owner'}")
    print(f"{'----':<8}{'------':<12}{'-----'}")

    all_status = gm.get_all_pcpu_status()
    for pcpu in range(min(gm.total_pcpus, args.limit or gm.total_pcpus)):
        info = all_status[pcpu]
        if info["mode"] == PinMode.EXCLUSIVE:
            status = "exclusive"
            owner = f"{info['domain']}:vcpu{info['vcpu_id']} (guest-pin)"
        else:
            status = "shared"
            if info["sharing_vms"]:
                owner = f"({', '.join(info['sharing_vms'])} range)"
            else:
                owner = "(free)"
        print(f"{pcpu:<8}{status:<12}{owner}")


def cmd_unpin_all(args):
    """Restore all vCPUs of a VM to range mode."""
    gm = GlobalCpuMap.load()
    ok, results = gm.unpin_all(args.vm)
    gm.save()

    if not ok:
        print(f"[ERROR] {results}", file=sys.stderr)
        return 1

    for msg in results:
        print(f"[INFO] {msg}")
    print(f"[OK] Restored all vCPUs for {args.vm} to range mode")


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        prog="vm-bindcore",
        description=(
            "VMM-side transparent vCPU pinning optimization tool.\n"
            "Manages dynamic 1:1 pinning based on in-VM app pinning notifications."
        ),
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose output"
    )
    subparsers = parser.add_subparsers(dest="command", help="Sub-command")

    # register
    p_reg = subparsers.add_parser(
        "register", help="Register a VM in the global CPU map"
    )
    p_reg.add_argument("--vm", required=True, help="VM domain name")
    p_reg.add_argument("--vcpus", required=True, type=int, help="Number of vCPUs")
    p_reg.add_argument("--cpuset", required=True, help="Allowed pCPU range (e.g. 0-15)")

    # unregister
    p_unreg = subparsers.add_parser(
        "unregister", help="Unregister a VM from the global CPU map"
    )
    p_unreg.add_argument("--vm", required=True, help="VM domain name")

    # notify (simulate guest notification)
    p_notify = subparsers.add_parser(
        "notify", help="Process a guest pinning notification"
    )
    p_notify.add_argument("--vm", required=True, help="VM domain name")
    p_notify.add_argument("--vcpu", required=True, type=int, help="vCPU id")
    p_notify.add_argument(
        "--action", required=True, choices=["pin", "unpin"],
        help="pin = guest app pinned, unpin = guest app unpinned"
    )
    p_notify.add_argument("--pid", type=int, help="Guest-side PID of the app")

    # status
    p_status = subparsers.add_parser(
        "status", help="Show vCPU pinning status"
    )
    p_status.add_argument("--vm", help="VM domain name (all VMs if omitted)")
    p_status.add_argument(
        "--format", choices=["table", "json"], default="table",
        help="Output format"
    )

    # map
    p_map = subparsers.add_parser(
        "map", help="Show global CPU pinning map"
    )
    p_map.add_argument(
        "--format", choices=["table", "json"], default="table",
        help="Output format"
    )
    p_map.add_argument(
        "--limit", type=int, help="Limit number of pCPUs shown"
    )

    # unpin-all
    p_unpin = subparsers.add_parser(
        "unpin-all", help="Restore all vCPUs of a VM to range mode"
    )
    p_unpin.add_argument("--vm", required=True, help="VM domain name")

    args = parser.parse_args()

    # Setup logging
    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(format=LOG_FORMAT, level=level)
    logging.getLogger("vm-bindcore").setLevel(level)

    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "register": cmd_register,
        "unregister": cmd_unregister,
        "notify": cmd_notify,
        "status": cmd_status,
        "map": cmd_map,
        "unpin-all": cmd_unpin_all,
    }

    func = commands.get(args.command)
    if func:
        try:
            return func(args) or 0
        except RuntimeError as e:
            print(f"[ERROR] {e}", file=sys.stderr)
            return 1
        except Exception as e:
            logger.exception("Unexpected error")
            print(f"[ERROR] {e}", file=sys.stderr)
            return 2
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())

