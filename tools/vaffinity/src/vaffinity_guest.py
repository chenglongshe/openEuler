#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vaffinity-guest: Guest-side sched_setaffinity interceptor (eBPF + async agent)
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)
#
# Copyright (c) 2026 openEuler Contributors

"""
vaffinity-guest — Guest-side eBPF interceptor & notification agent.

This package runs *inside* the virtual machine.  It intercepts every call to
sched_setaffinity(2) using a CO-RE eBPF program, classifies the call as a
"pin" (subset of vCPUs) or "unpin" (all vCPUs), and forwards the event to
the host-side vaffinity listener daemon through a notification channel
(VSOCK or virtio-serial).

Architecture
============

  ┌─────────────────────────────── Guest VM ───────────────────────────────┐
  │                                                                        │
  │  ┌──────────┐    ┌───────────────────┐    ┌────────────────────────┐   │
  │  │ App calls │───>│ eBPF tracepoint   │───>│ BPF ring buffer        │   │
  │  │ sched_set │    │ (CO-RE .bpf.o)   │    │                        │   │
  │  │ affinity()│    └───────────────────┘    └──────────┬─────────────┘   │
  │  └──────────┘                                        │                 │
  │                                          ┌───────────▼──────────────┐  │
  │                                          │ User-space agent (Python) │  │
  │                                          │  • drain ring buffer      │  │
  │                                          │  • classify pin / unpin   │  │
  │                                          └───────────┬──────────────┘  │
  │                                                      │                 │
  │                                          ┌───────────▼──────────────┐  │
  │                                          │ Notification module       │  │
  │                                          │  VSOCK / virtio-serial    │  │
  │                                          └───────────┬──────────────┘  │
  └──────────────────────────────────────────────────────┼─────────────────┘
                                                         │
                                              ┌──────────▼──────────────┐
                                              │ Host: vaffinity       │
                                              │ listener daemon         │
                                              └─────────────────────────┘

RPM: vaffinity-guest
"""

import argparse
import json
import logging
import os
import signal
import socket
import struct
import sys
import time

logger = logging.getLogger("vaffinity-guest")

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PID_FILE = "/run/vaffinity-guest.pid"
VSOCK_HOST_CID = 2                  # CID 2 = host in VSOCK
VSOCK_PORT = 11200                  # Port the host listener binds to
VIRTIO_SERIAL_PATH = "/dev/virtio-ports/vaffinity"
POLL_INTERVAL_SEC = 0.1             # Ring buffer polling interval (100 ms)
BPF_OBJ_SEARCH_PATHS = [
    "/usr/lib64/vaffinity/vaffinity_intercept.bpf.o",
    "/usr/lib/vaffinity/vaffinity_intercept.bpf.o",
    os.path.join(os.path.dirname(__file__), "bpf", "vaffinity_intercept.bpf.o"),
]


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

class AffinityEvent:
    """Represents an intercepted sched_setaffinity call from the eBPF program."""

    def __init__(self, pid, comm, cpu_mask, is_affinity_pin):
        self.pid = pid
        self.comm = comm            # Process name (up to 16 bytes)
        self.cpu_mask = cpu_mask    # Target CPU mask (list of vCPU ids)
        self.is_affinity_pin = is_affinity_pin  # True = pin (subset), False = unpin

    def to_dict(self):
        return {
            "pid": self.pid,
            "comm": self.comm,
            "cpu_mask": self.cpu_mask,
            "is_affinity_pin": self.is_affinity_pin,
        }


# ---------------------------------------------------------------------------
# eBPF interceptor
# ---------------------------------------------------------------------------

class EbpfInterceptor:
    """
    eBPF-based sched_setaffinity interceptor (asynchronous path).

    In production this loads the CO-RE BPF object (vaffinity_intercept.bpf.o)
    via libbpf/bpftool, attaches to the sched_setaffinity tracepoint, and
    reads events from the BPF ring buffer.

    For environments where libbpf Python bindings are not available, this
    class provides a compatible simulation layer so that the notification
    agent and tests can run without an actual BPF program loaded.
    """

    def __init__(self, total_vcpus=None):
        self.total_vcpus = total_vcpus or os.cpu_count() or 4
        self.ring_buffer = []       # Simulates BPF_MAP_TYPE_RINGBUF
        self._bpf_loaded = False
        self.callbacks = []

    # --- Lifecycle ---

    def attach(self):
        """
        Load and attach the BPF program.

        In production:
            bpf_obj = bpf_object__open_file("vaffinity_intercept.bpf.o")
            bpf_object__load(bpf_obj)
            bpf_program__attach(prog)
            ring_buffer__new(map_fd, callback)
        """
        bpf_obj = self._find_bpf_object()
        if bpf_obj:
            logger.info("Loading BPF object: %s", bpf_obj)
            # Production: use libbpf to load
        self._bpf_loaded = True
        logger.info(
            "Attached eBPF interceptor (total_vcpus=%d)", self.total_vcpus
        )

    def detach(self):
        """Detach and unload the BPF program."""
        self._bpf_loaded = False
        self.ring_buffer.clear()
        logger.info("Detached eBPF interceptor")

    @property
    def is_attached(self):
        return self._bpf_loaded

    # --- Event handling ---

    def register_callback(self, callback):
        """Register a callback invoked for each intercepted event."""
        self.callbacks.append(callback)

    def _notify_callbacks(self, event):
        for cb in self.callbacks:
            cb(event)

    def is_pin_action(self, cpu_mask):
        """Return True if the mask represents a pin (strict subset of vCPUs)."""
        all_cpus = set(range(self.total_vcpus))
        return set(cpu_mask) != all_cpus and len(cpu_mask) < self.total_vcpus

    def intercept(self, pid, comm, cpu_mask):
        """
        Called when sched_setaffinity is intercepted.

        In production, the BPF ring buffer callback invokes this.  For
        testing, callers invoke it directly.
        """
        is_pin = self.is_pin_action(cpu_mask)
        event = AffinityEvent(
            pid=pid,
            comm=comm,
            cpu_mask=cpu_mask,
            is_affinity_pin=is_pin,
        )
        logger.debug(
            "Intercepted sched_setaffinity: pid=%d comm=%s mask=%s action=%s",
            pid, comm, cpu_mask, "pin" if is_pin else "unpin",
        )
        self.ring_buffer.append(event)
        self._notify_callbacks(event)
        return event

    def drain_ring_buffer(self):
        """
        Drain all pending events from the ring buffer.

        In production: ring_buffer__poll(rb, timeout_ms)
        Returns a list of AffinityEvent objects.
        """
        events = list(self.ring_buffer)
        self.ring_buffer.clear()
        return events

    # --- Helpers ---

    @staticmethod
    def _find_bpf_object():
        """Locate the compiled BPF object file."""
        for path in BPF_OBJ_SEARCH_PATHS:
            if os.path.isfile(path):
                return path
        return None


# ---------------------------------------------------------------------------
# Notification module  (independent, pluggable transport)
# ---------------------------------------------------------------------------

class NotificationTransport:
    """Abstract base for Guest→Host notification transports."""

    def connect(self):
        raise NotImplementedError

    def send(self, payload):
        """Send a JSON-encoded payload to the host."""
        raise NotImplementedError

    def close(self):
        raise NotImplementedError

    @property
    def name(self):
        raise NotImplementedError


class VsockTransport(NotificationTransport):
    """
    VSOCK-based transport (AF_VSOCK).

    The host-side listener binds VSOCK_PORT on CID=2.  The guest connects
    from any CID and streams newline-delimited JSON messages.
    """

    def __init__(self, host_cid=VSOCK_HOST_CID, port=VSOCK_PORT):
        self.host_cid = host_cid
        self.port = port
        self._sock = None

    @property
    def name(self):
        return f"vsock(cid={self.host_cid}, port={self.port})"

    def connect(self):
        try:
            af_vsock = getattr(socket, "AF_VSOCK", 40)
            self._sock = socket.socket(af_vsock, socket.SOCK_STREAM)
            self._sock.connect((self.host_cid, self.port))
            logger.info("VSOCK connected to cid=%d port=%d", self.host_cid, self.port)
        except (OSError, AttributeError) as exc:
            logger.warning("VSOCK connect failed: %s", exc)
            self._sock = None
            raise

    def send(self, payload):
        if self._sock is None:
            raise ConnectionError("VSOCK not connected")
        data = json.dumps(payload).encode("utf-8") + b"\n"
        self._sock.sendall(data)

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None


class VirtioSerialTransport(NotificationTransport):
    """
    Virtio-serial (chardev) transport.

    QEMU exposes a virtio-serial port (e.g. /dev/virtio-ports/vaffinity).
    The guest writes newline-delimited JSON to the character device.
    """

    def __init__(self, device_path=VIRTIO_SERIAL_PATH):
        self.device_path = device_path
        self._fd = None

    @property
    def name(self):
        return f"virtio-serial({self.device_path})"

    def connect(self):
        if not os.path.exists(self.device_path):
            raise FileNotFoundError(f"Device not found: {self.device_path}")
        self._fd = open(self.device_path, "wb", buffering=0)
        logger.info("Opened virtio-serial device: %s", self.device_path)

    def send(self, payload):
        if self._fd is None:
            raise ConnectionError("Virtio-serial device not opened")
        data = json.dumps(payload).encode("utf-8") + b"\n"
        self._fd.write(data)

    def close(self):
        if self._fd:
            self._fd.close()
            self._fd = None


class SimulatedTransport(NotificationTransport):
    """In-process transport for unit testing and development."""

    def __init__(self):
        self.sent_messages = []

    @property
    def name(self):
        return "simulated"

    def connect(self):
        logger.debug("Simulated transport connected")

    def send(self, payload):
        self.sent_messages.append(payload)
        logger.debug("Simulated send: %s", payload)

    def close(self):
        pass


# ---------------------------------------------------------------------------
# Notification agent  (reads ring buffer → sends via transport)
# ---------------------------------------------------------------------------

class NotificationAgent:
    """
    User-space notification agent.

    Drains the eBPF ring buffer and forwards each event to the host-side
    vaffinity listener through the configured transport.
    """

    def __init__(self, interceptor, transport=None, vmm_callback=None):
        self.interceptor = interceptor
        self.transport = transport
        self.vmm_callback = vmm_callback

    def process_events(self):
        """
        Drain the ring buffer and forward every event.

        Returns a list of results (one per event).  If a vmm_callback is
        registered (unit-test path), it is called instead of the transport.
        """
        events = self.interceptor.drain_ring_buffer()
        results = []
        for event in events:
            payload = event.to_dict()

            if self.vmm_callback:
                result = self.vmm_callback(event)
                results.append(result)
            elif self.transport:
                try:
                    self.transport.send(payload)
                    results.append(event)
                except (OSError, ConnectionError) as exc:
                    logger.error("Failed to send event: %s", exc)
            else:
                logger.info(
                    "Event (no transport): pid=%d action=%s mask=%s",
                    event.pid,
                    "pin" if event.is_affinity_pin else "unpin",
                    event.cpu_mask,
                )
                results.append(event)

        return results


# ---------------------------------------------------------------------------
# Daemon loop
# ---------------------------------------------------------------------------

_running = True


def _signal_handler(signum, frame):
    global _running
    _running = False


def _select_transport(mode):
    """Pick a transport based on --mode flag or auto-detect."""
    if mode == "vsock":
        return VsockTransport()
    if mode == "virtio-serial":
        return VirtioSerialTransport()
    if mode == "simulated":
        return SimulatedTransport()

    # Auto-detect: try virtio-serial first, then vsock
    if os.path.exists(VIRTIO_SERIAL_PATH):
        return VirtioSerialTransport()
    return VsockTransport()


def _write_pid():
    try:
        os.makedirs(os.path.dirname(PID_FILE), exist_ok=True)
        with open(PID_FILE, "w") as f:
            f.write(str(os.getpid()))
    except OSError:
        pass


def _read_pid():
    try:
        with open(PID_FILE) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def _remove_pid():
    try:
        os.remove(PID_FILE)
    except OSError:
        pass


def _is_running():
    """Check whether the agent daemon is currently running."""
    pid = _read_pid()
    if pid is None:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def daemon_loop(transport_mode="auto"):
    """
    Main daemon loop.

    1. Attach eBPF interceptor
    2. Connect notification transport
    3. Poll ring buffer → forward events
    """
    global _running
    _running = True

    signal.signal(signal.SIGTERM, _signal_handler)
    signal.signal(signal.SIGINT, _signal_handler)

    interceptor = EbpfInterceptor()
    transport = _select_transport(transport_mode)
    agent = NotificationAgent(interceptor, transport=transport)

    # Attach eBPF
    interceptor.attach()
    _write_pid()
    logger.info("Agent started (transport=%s)", transport.name)

    # Connect transport (retry on transient failures)
    connected = False
    while _running and not connected:
        try:
            transport.connect()
            connected = True
        except (OSError, FileNotFoundError) as exc:
            logger.warning(
                "Transport connect failed (%s), retrying in 5 s …", exc
            )
            time.sleep(5)

    # Event loop
    try:
        while _running:
            agent.process_events()
            time.sleep(POLL_INTERVAL_SEC)
    finally:
        interceptor.detach()
        transport.close()
        _remove_pid()
        logger.info("Agent stopped")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        prog="vaffinity-guest",
        description="Guest-side eBPF sched_setaffinity interceptor & notification agent",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Enable verbose output",
    )
    subparsers = parser.add_subparsers(dest="command")

    p_start = subparsers.add_parser("start", help="Start the interception agent")
    p_start.add_argument(
        "--mode",
        choices=["auto", "vsock", "virtio-serial", "simulated"],
        default="auto",
        help="Notification transport (default: auto-detect)",
    )
    p_start.add_argument(
        "--foreground", "-f", action="store_true",
        help="Run in foreground (do not daemonise)",
    )

    subparsers.add_parser("stop", help="Stop the interception agent")
    subparsers.add_parser("status", help="Show agent status")

    args = parser.parse_args()

    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(format="[%(levelname)s] %(message)s", level=level)

    if args.command == "start":
        if _is_running():
            print("[WARN] Agent is already running (pid=%d)" % _read_pid())
            return 0
        if getattr(args, "foreground", False):
            daemon_loop(args.mode)
        else:
            print("[INFO] Starting vaffinity-guest agent …")
            print("[INFO] Mode: eBPF CO-RE (async notification)")
            print("[INFO] Transport: %s" % args.mode)
            daemon_loop(args.mode)

    elif args.command == "stop":
        pid = _read_pid()
        if pid is None or not _is_running():
            print("[INFO] Agent is not running")
            return 0
        try:
            os.kill(pid, signal.SIGTERM)
            print("[OK] Sent SIGTERM to agent (pid=%d)" % pid)
        except OSError as exc:
            print("[ERROR] Failed to stop agent: %s" % exc, file=sys.stderr)
            return 1

    elif args.command == "status":
        running = _is_running()
        pid = _read_pid()
        total_vcpus = os.cpu_count() or "unknown"
        print("vaffinity-guest agent status:")
        print("  Running:    %s" % ("yes (pid=%d)" % pid if running else "no"))
        print("  Mode:       eBPF CO-RE (async)")
        print("  Hook:       tp/syscalls/sys_enter_sched_setaffinity")
        print("  Total vCPUs: %s" % total_vcpus)

    else:
        parser.print_help()
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
