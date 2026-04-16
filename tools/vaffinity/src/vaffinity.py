#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vaffinity: VMM-side handler for transparent in-VM pinning passthrough
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)
#
# Copyright (c) 2026 openEuler Contributors

"""
vaffinity — VMM-side transparent vCPU pinning optimization tool.

This package runs on the **host** (hypervisor).  It listens for pinning
notifications from guest VMs, maintains a global CPU map of all pCPU
assignments, and dynamically switches individual vCPUs between range-pinning
and 1:1 exclusive pinning.

VM vCPU pinning information is auto-discovered from libvirt using
``virsh vcpupin <domain>``, so no manual registration is required.

Architecture
============

  Guest VM  ──VSOCK/virtio-serial──▶  vaffinity listener daemon (host)
                                           │
                                     ┌─────▼──────────┐
                                     │ Notification    │
                                     │ receiver        │
                                     └─────┬──────────┘
                                           │ parse pin/unpin intent
                                     ┌─────▼──────────┐    ┌──────────────┐
                                     │ Global CPU Map  │───>│ Pin executor │
                                     │ + per-vCPU      │    │ (virsh/      │
                                     │   cpuset        │    │  cgroup)     │
                                     └────────────────┘    └──────────────┘

RPM: vaffinity

Sub-commands
------------
  discover    Auto-discover all running VMs and their vCPU pinning
  notify      Manually inject a pin/unpin notification (testing)
  status      Show vCPU pinning status
  map         Show global CPU map
  unpin-all   Restore all vCPUs of a VM to range mode
  log         Show recent pinning event log
  listen      Run the notification listener daemon
"""

import argparse
import json
import logging
import os
import re
import select
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

CONF_DIR = "/etc/vaffinity"
GLOBAL_MAP_FILE = os.path.join(CONF_DIR, "global_cpu_map.json")
LOG_FILE = os.path.join(CONF_DIR, "events.log")
PID_FILE = "/run/vaffinity-listener.pid"
VSOCK_PORT = 11200                  # Same as guest side
LOG_FORMAT = "[%(levelname)s] %(message)s"

logger = logging.getLogger("vaffinity")


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

    def __init__(self, domain, vcpu_count, cpuset, per_vcpu_cpusets=None):
        self.domain = domain
        self.vcpu_count = vcpu_count
        self.cpuset = list(cpuset)           # Default/fallback cpuset
        # per_vcpu_cpusets: {vcpu_id: [pcpu_list]} — from virsh vcpupin
        if per_vcpu_cpusets:
            self.vcpus = {
                i: VcpuPinState(i, per_vcpu_cpusets.get(i, cpuset))
                for i in range(vcpu_count)
            }
        else:
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

    def register_vm(self, domain, vcpu_count, cpuset,
                    per_vcpu_cpusets=None):
        """Register a VM with its vCPU count and cpuset.

        Args:
            domain: libvirt domain name
            vcpu_count: number of vCPUs
            cpuset: default/fallback pCPU list
            per_vcpu_cpusets: optional {vcpu_id: [pcpu_list]} from virsh vcpupin
        """
        if domain not in self.vm_states:
            self.vm_states[domain] = VmPinState(
                domain, vcpu_count, cpuset,
                per_vcpu_cpusets=per_vcpu_cpusets,
            )
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

        Consults the VM's cpuset and the global exclusive_map to select a
        free pCPU.  On success the vCPU is 1:1 pinned and the pCPU is
        marked as occupied in the global map.

        Returns (success: bool, pcpu: int or None, message: str)
        """
        vm = self.vm_states.get(domain)
        if vm is None:
            return False, None, f"VM '{domain}' not registered"

        vcpu = vm.vcpus.get(vcpu_id)
        if vcpu is None:
            return False, None, f"vCPU {vcpu_id} not found in '{domain}'"

        if vcpu.mode == PinMode.EXCLUSIVE:
            return True, vcpu.pinned_pcpu, (
                f"vCPU {vcpu_id} already exclusive on pCPU {vcpu.pinned_pcpu}"
            )

        # Find free pCPU in this vCPU's own cpuset (per-vCPU affinity)
        pcpu = self.find_free_pcpu(vcpu.cpuset)
        if pcpu is None:
            msg = (
                f"No free pCPU in cpuset {_format_cpulist(vcpu.cpuset)} "
                f"for {domain}:vcpu{vcpu_id} — keeping range mode"
            )
            logger.warning(msg)
            return False, None, msg

        # Record exclusive pin
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
            domain, vcpu_id, _format_cpulist(vcpu.cpuset),
        )
        return True, (
            f"Restored vcpu{vcpu_id} to range mode "
            f"(cpuset {_format_cpulist(vcpu.cpuset)})"
        )

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
# Pin executor  (calls virsh vcpupin / sched_setaffinity on vCPU threads)
# ---------------------------------------------------------------------------

class PinExecutor:
    """
    Performs the actual OS-level pinning of vCPU threads.

    In production this calls ``virsh vcpupin <domain> <vcpu> <pcpu>`` or
    directly modifies the cgroup cpuset of the QEMU vCPU thread.  For
    testing, the executor records operations without side-effects.
    """

    def __init__(self, dry_run=False):
        self.dry_run = dry_run
        self.history = []           # (action, domain, vcpu, pcpu/cpuset)

    def pin_vcpu(self, domain, vcpu_id, pcpu):
        """Pin a vCPU thread to a single pCPU (1:1 exclusive)."""
        self.history.append(("pin", domain, vcpu_id, pcpu))
        if self.dry_run:
            logger.info(
                "[DRY-RUN] virsh vcpupin %s %d %d", domain, vcpu_id, pcpu,
            )
            return True
        return self._virsh_vcpupin(domain, vcpu_id, str(pcpu))

    def restore_vcpu(self, domain, vcpu_id, cpuset):
        """Restore a vCPU thread to a cpuset range."""
        cpuset_str = _format_cpulist(cpuset)
        self.history.append(("restore", domain, vcpu_id, cpuset_str))
        if self.dry_run:
            logger.info(
                "[DRY-RUN] virsh vcpupin %s %d %s", domain, vcpu_id, cpuset_str,
            )
            return True
        return self._virsh_vcpupin(domain, vcpu_id, cpuset_str)

    @staticmethod
    def _virsh_vcpupin(domain, vcpu_id, cpulist):
        try:
            subprocess.run(
                ["virsh", "vcpupin", domain, str(vcpu_id), cpulist],
                capture_output=True, text=True, timeout=10, check=True,
            )
            return True
        except (subprocess.CalledProcessError, FileNotFoundError,
                subprocess.TimeoutExpired) as exc:
            logger.error("virsh vcpupin failed: %s", exc)
            return False


# ---------------------------------------------------------------------------
# Guest-side pinning notification  (message received from guest)
# ---------------------------------------------------------------------------

class GuestPinNotification:
    """
    Represents a pinning notification from the Guest to the VMM.

    In production, this arrives via VSOCK or virtio-serial from the
    vaffinity-guest agent running inside the VM.
    """
    ACTION_PIN = "pin"
    ACTION_UNPIN = "unpin"

    def __init__(self, domain, vcpu_id, action, guest_pid=None,
                 target_vcpus=None):
        self.domain = domain
        self.vcpu_id = vcpu_id           # Which vCPU the app pinned to
        self.action = action             # "pin" or "unpin"
        self.guest_pid = guest_pid       # PID of app inside guest
        self.target_vcpus = target_vcpus # Bitmask of target vCPUs (info only)

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


# ---------------------------------------------------------------------------
# VMM pin handler  (processes notifications and updates global map)
# ---------------------------------------------------------------------------

class VmmPinHandler:
    """
    VMM-side handler that processes guest pinning notifications.

    For each notification it:
      1. Looks up the VM in the global CPU map
      2. Consults the VM cpuset + exclusive_map for a free pCPU
      3. Calls the PinExecutor to perform the OS-level pinning
      4. Updates the global map and persists it
      5. Writes a structured event log entry
    """

    def __init__(self, global_map, executor=None, persist_path=None):
        self.global_map = global_map
        self.executor = executor or PinExecutor(dry_run=True)
        self.persist_path = persist_path or GLOBAL_MAP_FILE

    def handle_notification(self, notification):
        """
        Process a single guest pinning notification.
        Returns (success: bool, message: str).
        """
        if notification.action == GuestPinNotification.ACTION_PIN:
            ok, pcpu, msg = self.global_map.pin_exclusive(
                notification.domain,
                notification.vcpu_id,
                guest_pid=notification.guest_pid,
            )
            if ok and pcpu is not None:
                self.executor.pin_vcpu(
                    notification.domain, notification.vcpu_id, pcpu,
                )
                _write_event_log(
                    "PIN", notification.domain, notification.vcpu_id,
                    f"pcpu={pcpu} source=guest-pin pid={notification.guest_pid}",
                )
            return ok, msg

        elif notification.action == GuestPinNotification.ACTION_UNPIN:
            vm = self.global_map.get_vm_status(notification.domain)
            ok, msg = self.global_map.unpin_restore(
                notification.domain,
                notification.vcpu_id,
            )
            if ok and vm:
                vcpu = vm.vcpus.get(notification.vcpu_id)
                restore_cpuset = vcpu.cpuset if vcpu else vm.cpuset
                self.executor.restore_vcpu(
                    notification.domain, notification.vcpu_id, restore_cpuset,
                )
                _write_event_log(
                    "UNPIN", notification.domain, notification.vcpu_id,
                    f"cpuset={_format_cpulist(restore_cpuset)} source=guest-unpin",
                )
            return ok, msg

        else:
            return False, f"Unknown action: {notification.action}"

    def handle_batch(self, notifications):
        """Process a batch of notifications."""
        results = []
        for notif in notifications:
            ok, msg = self.handle_notification(notif)
            results.append({
                "notification": notif.to_dict(), "ok": ok, "msg": msg,
            })
        return results


# ---------------------------------------------------------------------------
# Notification listener daemon  (VSOCK)
# ---------------------------------------------------------------------------

_listener_running = True


def _listener_signal_handler(signum, frame):
    global _listener_running
    _listener_running = False


def _resolve_domain_from_cid(cid):
    """
    Map a VSOCK CID to a libvirt domain name.

    virsh uses CID = <some integer> for each VM.  We query libvirt to find
    the domain that owns this CID.  Falls back to "vm-cid-<N>".
    """
    try:
        output = subprocess.check_output(
            ["virsh", "list", "--name", "--state-running"],
            text=True, timeout=5,
        )
        for name in output.strip().split("\n"):
            name = name.strip()
            if not name:
                continue
            try:
                xml = subprocess.check_output(
                    ["virsh", "dumpxml", name],
                    text=True, timeout=5,
                )
                # Look for <cid auto='yes' value='N'/>
                match = re.search(r"<cid[^>]+value=['\"](\d+)['\"]", xml)
                if match and int(match.group(1)) == cid:
                    return name
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                continue
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired):
        pass
    return f"vm-cid-{cid}"


def _handle_client(conn, addr, handler, global_map, persist_path):
    """Handle a single guest connection (one VM, streaming JSON lines)."""
    cid = addr[0] if isinstance(addr, tuple) else 0
    domain = _resolve_domain_from_cid(cid)
    logger.info("Guest connected: cid=%s domain=%s", cid, domain)

    # Auto-discover VM pinning if not yet registered
    if domain not in global_map.vm_states:
        info = discover_vm(domain)
        if info:
            vcpu_count, default_cpuset, per_vcpu = info
            global_map.register_vm(
                domain, vcpu_count, default_cpuset,
                per_vcpu_cpusets=per_vcpu,
            )
            global_map.save(persist_path)
            logger.info(
                "Auto-discovered VM '%s': %d vCPUs, cpuset %s",
                domain, vcpu_count, _format_cpulist(default_cpuset),
            )
        else:
            logger.warning(
                "Could not auto-discover VM '%s' — notifications may fail",
                domain,
            )

    buf = b""
    try:
        while _listener_running:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    logger.warning("Invalid JSON from %s: %s", domain, line)
                    continue

                # Build notification from guest event
                cpu_mask = msg.get("cpu_mask", [])
                is_pin = msg.get("is_affinity_pin", True)
                vcpu_id = cpu_mask[0] if cpu_mask and is_pin else 0

                notif = GuestPinNotification(
                    domain=domain,
                    vcpu_id=vcpu_id,
                    action="pin" if is_pin else "unpin",
                    guest_pid=msg.get("pid"),
                    target_vcpus=cpu_mask,
                )
                ok, result_msg = handler.handle_notification(notif)
                global_map.save(persist_path)
                logger.info(
                    "Processed %s from %s: ok=%s msg=%s",
                    notif.action, domain, ok, result_msg,
                )
    except (ConnectionResetError, BrokenPipeError):
        logger.info("Guest disconnected: %s", domain)
    finally:
        conn.close()


def listener_daemon(persist_path=GLOBAL_MAP_FILE, dry_run=False):
    """
    Main listener daemon loop.

    Binds a VSOCK socket, accepts connections from guest VMs, and
    processes pinning notifications.
    """
    global _listener_running
    _listener_running = True

    signal.signal(signal.SIGTERM, _listener_signal_handler)
    signal.signal(signal.SIGINT, _listener_signal_handler)

    global_map = GlobalCpuMap.load(persist_path)

    # Auto-discover all running VMs on startup
    for domain, vcpu_count, cpuset, per_vcpu in discover_all_vms():
        if domain not in global_map.vm_states:
            global_map.register_vm(
                domain, vcpu_count, cpuset,
                per_vcpu_cpusets=per_vcpu,
            )
            logger.info(
                "Auto-discovered VM '%s': %d vCPUs, cpuset %s",
                domain, vcpu_count, _format_cpulist(cpuset),
            )
    global_map.save(persist_path)

    executor = PinExecutor(dry_run=dry_run)
    handler = VmmPinHandler(global_map, executor=executor,
                            persist_path=persist_path)

    af_vsock = getattr(socket, "AF_VSOCK", 40)
    server = socket.socket(af_vsock, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((socket.VMADDR_CID_ANY if hasattr(socket, "VMADDR_CID_ANY")
                 else 0xFFFFFFFF, VSOCK_PORT))
    server.listen(16)
    server.settimeout(1.0)

    # Write PID file
    os.makedirs(os.path.dirname(PID_FILE), exist_ok=True)
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))

    logger.info("Listener started on VSOCK port %d (dry_run=%s)",
                VSOCK_PORT, dry_run)

    try:
        while _listener_running:
            try:
                conn, addr = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            t = threading.Thread(
                target=_handle_client,
                args=(conn, addr, handler, global_map, persist_path),
                daemon=True,
            )
            t.start()
    finally:
        server.close()
        try:
            os.remove(PID_FILE)
        except OSError:
            pass
        global_map.save(persist_path)
        logger.info("Listener stopped")


# ---------------------------------------------------------------------------
# Event log
# ---------------------------------------------------------------------------

def _write_event_log(action, domain, vcpu_id, details):
    """Append a structured event to the log file."""
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    line = f"{ts} [{action}] vm={domain} vcpu={vcpu_id} {details}\n"
    try:
        os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
        with open(LOG_FILE, "a") as f:
            f.write(line)
    except OSError:
        logger.warning("Failed to write event log")


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
# Auto-discovery via virsh vcpupin
# ---------------------------------------------------------------------------

def _parse_virsh_vcpupin(output):
    """
    Parse the output of ``virsh vcpupin <domain>`` into per-vCPU cpusets.

    Example input::

         VCPU   CPU Affinity
        ----------------------
         0      5-47,53-63
         1      5-47,53-63
         16     0-63

    Returns:
        dict {vcpu_id: [pcpu_list]}
    """
    per_vcpu = {}
    for line in output.strip().splitlines():
        line = line.strip()
        # Skip header and separator lines
        if not line or line.startswith("VCPU") or line.startswith("---"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        try:
            vcpu_id = int(parts[0])
        except ValueError:
            continue
        cpulist_str = parts[1].strip()
        per_vcpu[vcpu_id] = _parse_cpulist(cpulist_str)
    return per_vcpu


def discover_vm(domain):
    """
    Auto-discover a VM's vCPU pinning by calling ``virsh vcpupin <domain>``.

    Returns:
        (vcpu_count, default_cpuset, per_vcpu_cpusets) or None on failure.
        - vcpu_count: int
        - default_cpuset: union of all per-vCPU cpusets
        - per_vcpu_cpusets: {vcpu_id: [pcpu_list]}
    """
    try:
        result = subprocess.run(
            ["virsh", "vcpupin", domain],
            capture_output=True, text=True, timeout=10, check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired) as exc:
        logger.error("Failed to discover VM '%s': %s", domain, exc)
        return None

    per_vcpu = _parse_virsh_vcpupin(result.stdout)
    if not per_vcpu:
        logger.warning("No vCPU info found for VM '%s'", domain)
        return None

    vcpu_count = max(per_vcpu.keys()) + 1
    # Default cpuset = union of all per-vCPU cpusets
    all_cpus = set()
    for cpus in per_vcpu.values():
        all_cpus.update(cpus)
    default_cpuset = sorted(all_cpus)

    return vcpu_count, default_cpuset, per_vcpu


def discover_all_vms():
    """
    Auto-discover all running VMs and their vCPU pinning.

    Returns:
        list of (domain, vcpu_count, default_cpuset, per_vcpu_cpusets)
    """
    try:
        result = subprocess.run(
            ["virsh", "list", "--name", "--state-running"],
            capture_output=True, text=True, timeout=10, check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired) as exc:
        logger.error("Failed to list running VMs: %s", exc)
        return []

    vms = []
    for name in result.stdout.strip().split("\n"):
        name = name.strip()
        if not name:
            continue
        info = discover_vm(name)
        if info:
            vcpu_count, default_cpuset, per_vcpu = info
            vms.append((name, vcpu_count, default_cpuset, per_vcpu))
    return vms


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_discover(args):
    """Auto-discover running VMs and their vCPU pinning from libvirt."""
    gm = GlobalCpuMap.load()

    if args.vm:
        # Discover a specific VM
        info = discover_vm(args.vm)
        if info is None:
            print(f"[ERROR] Could not discover VM '{args.vm}'",
                  file=sys.stderr)
            return 1
        vcpu_count, default_cpuset, per_vcpu = info
        # Re-register (overwrite) to refresh pinning info
        gm.vm_states.pop(args.vm, None)
        gm.register_vm(
            args.vm, vcpu_count, default_cpuset,
            per_vcpu_cpusets=per_vcpu,
        )
        gm.save()
        print(f"[OK] Discovered {args.vm}: {vcpu_count} vCPUs")
        for vid in sorted(per_vcpu.keys()):
            print(f"  vcpu{vid:<4} cpuset: {_format_cpulist(per_vcpu[vid])}")
    else:
        # Discover all running VMs
        vms = discover_all_vms()
        if not vms:
            print("No running VMs found (or virsh not available)")
            return 0
        for domain, vcpu_count, cpuset, per_vcpu in vms:
            gm.vm_states.pop(domain, None)
            gm.register_vm(
                domain, vcpu_count, cpuset,
                per_vcpu_cpusets=per_vcpu,
            )
            print(f"[OK] Discovered {domain}: {vcpu_count} vCPUs, "
                  f"cpuset {_format_cpulist(cpuset)}")
        gm.save()
        print(f"\nTotal: {len(vms)} VM(s) discovered")


def cmd_notify(args):
    """Simulate receiving a guest pinning notification (for testing)."""
    gm = GlobalCpuMap.load()
    executor = PinExecutor(dry_run=True)
    handler = VmmPinHandler(gm, executor=executor)

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

    print(f"VM: {vm.domain} ({vm.vcpu_count} vCPUs, "
          f"cpuset: {_format_cpulist(vm.cpuset)})")
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
    limit = min(gm.total_pcpus, args.limit or gm.total_pcpus)
    for pcpu in range(limit):
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


def cmd_log(args):
    """Show recent pinning event log."""
    if not os.path.exists(LOG_FILE):
        print("No event log found")
        return 0

    with open(LOG_FILE) as f:
        lines = f.readlines()

    if args.vm:
        lines = [l for l in lines if f"vm={args.vm}" in l]

    tail = args.tail or 50
    for line in lines[-tail:]:
        print(line.rstrip())


def cmd_listen(args):
    """Run the notification listener daemon."""
    listener_daemon(
        persist_path=GLOBAL_MAP_FILE,
        dry_run=args.dry_run,
    )


# ---------------------------------------------------------------------------
# Main entry point
# ---------------------------------------------------------------------------

def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        prog="vaffinity",
        description=(
            "VMM-side transparent vCPU pinning optimization tool.\n"
            "Manages dynamic 1:1 pinning based on in-VM app pinning "
            "notifications."
        ),
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose output"
    )
    subparsers = parser.add_subparsers(dest="command", help="Sub-command")

    # discover
    p_disc = subparsers.add_parser(
        "discover",
        help="Auto-discover running VMs and their vCPU pinning from libvirt"
    )
    p_disc.add_argument(
        "--vm",
        help="Discover a specific VM (all running VMs if omitted)"
    )

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
    p_notify.add_argument("--pid", type=int,
                          help="Guest-side PID of the app")

    # status
    p_status = subparsers.add_parser(
        "status", help="Show vCPU pinning status"
    )
    p_status.add_argument("--vm",
                          help="VM domain name (all VMs if omitted)")
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

    # log
    p_log = subparsers.add_parser(
        "log", help="Show recent pinning event log"
    )
    p_log.add_argument("--vm", help="Filter by VM domain name")
    p_log.add_argument("--tail", type=int, default=50,
                       help="Number of recent entries to show")

    # listen
    p_listen = subparsers.add_parser(
        "listen", help="Run the notification listener daemon"
    )
    p_listen.add_argument(
        "--dry-run", action="store_true",
        help="Do not actually pin vCPU threads (log only)"
    )

    args = parser.parse_args()

    # Setup logging
    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(format=LOG_FORMAT, level=level)
    logging.getLogger("vaffinity").setLevel(level)

    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "discover": cmd_discover,
        "notify": cmd_notify,
        "status": cmd_status,
        "map": cmd_map,
        "unpin-all": cmd_unpin_all,
        "log": cmd_log,
        "listen": cmd_listen,
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

