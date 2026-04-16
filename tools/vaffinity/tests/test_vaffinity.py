#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# Test suite for vaffinity (Host-side handler + Guest-side interceptor)
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)

"""
Unit and integration tests for vaffinity.

Tests cover the patent's core architecture:
  1. Global CPU Map management
  2. Dynamic 1:1 pinning on guest notification
  3. Automatic un-pin restore
  4. Conflict avoidance
  5. Guest-side eBPF interception simulation
  6. Notification transport (simulated)
  7. End-to-end guest → host notification flow
  8. Pin executor
  9. Event log

Run with: python3 -m pytest tests/test_vaffinity.py -v
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Add source directory to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from vaffinity import (  # noqa: E402
    GlobalCpuMap,
    GuestPinNotification,
    PinExecutor,
    PinMode,
    PinSource,
    VcpuPinState,
    VmPinState,
    VmmPinHandler,
    _format_cpulist,
    _parse_cpulist,
    _parse_virsh_vcpupin,
    _write_event_log,
    main,
)
from vaffinity_guest import (  # noqa: E402
    AffinityEvent,
    EbpfInterceptor,
    NotificationAgent,
    SimulatedTransport,
    VsockTransport,
    VirtioSerialTransport,
)


# ============================================================================
# Test: Utility functions
# ============================================================================

class TestFormatCpulist(unittest.TestCase):
    """Test CPU list formatting."""

    def test_empty(self):
        self.assertEqual(_format_cpulist([]), "(none)")

    def test_single(self):
        self.assertEqual(_format_cpulist([5]), "5")

    def test_range(self):
        self.assertEqual(_format_cpulist([0, 1, 2, 3]), "0-3")

    def test_mixed(self):
        self.assertEqual(_format_cpulist([0, 1, 2, 5, 6, 10]), "0-2,5-6,10")

    def test_unsorted(self):
        self.assertEqual(_format_cpulist([3, 1, 2, 0]), "0-3")


class TestParseCpulist(unittest.TestCase):
    """Test CPU list parsing."""

    def test_single(self):
        self.assertEqual(_parse_cpulist("0"), [0])

    def test_range(self):
        self.assertEqual(_parse_cpulist("0-3"), [0, 1, 2, 3])

    def test_mixed(self):
        self.assertEqual(
            _parse_cpulist("0-3,8,12-15"),
            [0, 1, 2, 3, 8, 12, 13, 14, 15],
        )

    def test_whitespace(self):
        self.assertEqual(
            _parse_cpulist(" 0-3 , 8-11 "),
            [0, 1, 2, 3, 8, 9, 10, 11],
        )


class TestParseVirshVcpupin(unittest.TestCase):
    """Test parsing of virsh vcpupin output."""

    def test_basic_output(self):
        output = """\
 VCPU   CPU Affinity
----------------------
 0      5-47,53-63
 1      5-47,53-63
 2      5-47,53-63
"""
        result = _parse_virsh_vcpupin(output)
        self.assertEqual(len(result), 3)
        self.assertIn(0, result)
        self.assertIn(1, result)
        self.assertIn(2, result)
        # Check vcpu 0's cpuset
        expected = list(range(5, 48)) + list(range(53, 64))
        self.assertEqual(result[0], expected)

    def test_mixed_affinity(self):
        """Test VMs where different vCPUs have different affinities."""
        output = """\
 VCPU   CPU Affinity
----------------------
 0      5-47,53-63
 1      5-47,53-63
 16     0-63
 17     0-63
 18     0-63
"""
        result = _parse_virsh_vcpupin(output)
        self.assertEqual(len(result), 5)
        # vCPU 0 has restricted affinity
        self.assertNotIn(0, result[0])
        self.assertIn(5, result[0])
        # vCPU 16 has full 0-63 affinity
        self.assertEqual(result[16], list(range(0, 64)))
        self.assertEqual(result[17], list(range(0, 64)))

    def test_empty_output(self):
        result = _parse_virsh_vcpupin("")
        self.assertEqual(result, {})

    def test_header_only(self):
        output = """\
 VCPU   CPU Affinity
----------------------
"""
        result = _parse_virsh_vcpupin(output)
        self.assertEqual(result, {})

    def test_single_cpu_affinity(self):
        output = """\
 VCPU   CPU Affinity
----------------------
 0      4
"""
        result = _parse_virsh_vcpupin(output)
        self.assertEqual(result[0], [4])


class TestPerVcpuCpuset(unittest.TestCase):
    """Test per-vCPU cpuset support in VmPinState and GlobalCpuMap."""

    def test_vm_with_per_vcpu_cpusets(self):
        per_vcpu = {
            0: list(range(5, 48)) + list(range(53, 64)),
            1: list(range(5, 48)) + list(range(53, 64)),
            2: list(range(0, 64)),
        }
        vm = VmPinState("testvm", 3, list(range(0, 64)),
                         per_vcpu_cpusets=per_vcpu)
        # vCPU 0 should have restricted cpuset
        self.assertNotIn(0, vm.vcpus[0].cpuset)
        self.assertIn(5, vm.vcpus[0].cpuset)
        # vCPU 2 should have full cpuset
        self.assertIn(0, vm.vcpus[2].cpuset)

    def test_pin_uses_vcpu_cpuset(self):
        """Pin should use per-vCPU cpuset, not the VM-level default."""
        gm = GlobalCpuMap(total_pcpus=64)
        per_vcpu = {
            0: [10, 11, 12],
            1: [20, 21, 22],
        }
        gm.register_vm("vm1", 2, list(range(64)),
                        per_vcpu_cpusets=per_vcpu)

        ok, pcpu, _ = gm.pin_exclusive("vm1", 0)
        self.assertTrue(ok)
        self.assertIn(pcpu, [10, 11, 12])

        ok, pcpu, _ = gm.pin_exclusive("vm1", 1)
        self.assertTrue(ok)
        self.assertIn(pcpu, [20, 21, 22])


# ============================================================================
# Test: VcpuPinState and VmPinState
# ============================================================================

class TestVcpuPinState(unittest.TestCase):
    """Test vCPU pin state data structure."""

    def test_default_state(self):
        state = VcpuPinState(0, [0, 1, 2, 3])
        self.assertEqual(state.vcpu_id, 0)
        self.assertEqual(state.mode, PinMode.RANGE)
        self.assertIsNone(state.pinned_pcpu)
        self.assertEqual(state.source, PinSource.DEFAULT)

    def test_serialize_roundtrip(self):
        state = VcpuPinState(2, [0, 1, 2, 3])
        state.mode = PinMode.EXCLUSIVE
        state.pinned_pcpu = 8
        state.source = PinSource.GUEST_PIN
        state.guest_pid = 1234

        d = state.to_dict()
        restored = VcpuPinState.from_dict(d)
        self.assertEqual(restored.vcpu_id, 2)
        self.assertEqual(restored.mode, PinMode.EXCLUSIVE)
        self.assertEqual(restored.pinned_pcpu, 8)
        self.assertEqual(restored.source, PinSource.GUEST_PIN)
        self.assertEqual(restored.guest_pid, 1234)


class TestVmPinState(unittest.TestCase):
    """Test VM pin state data structure."""

    def test_creation(self):
        vm = VmPinState("test-vm", 4, [0, 1, 2, 3, 4, 5, 6, 7])
        self.assertEqual(vm.domain, "test-vm")
        self.assertEqual(vm.vcpu_count, 4)
        self.assertEqual(len(vm.vcpus), 4)
        for i in range(4):
            self.assertEqual(vm.vcpus[i].mode, PinMode.RANGE)

    def test_serialize_roundtrip(self):
        vm = VmPinState("vm1", 2, [0, 1, 2, 3])
        vm.vcpus[0].mode = PinMode.EXCLUSIVE
        vm.vcpus[0].pinned_pcpu = 2

        d = vm.to_dict()
        restored = VmPinState.from_dict(d)
        self.assertEqual(restored.domain, "vm1")
        self.assertEqual(restored.vcpus[0].mode, PinMode.EXCLUSIVE)
        self.assertEqual(restored.vcpus[0].pinned_pcpu, 2)
        self.assertEqual(restored.vcpus[1].mode, PinMode.RANGE)


# ============================================================================
# Test: GlobalCpuMap — core patent logic
# ============================================================================

class TestGlobalCpuMap(unittest.TestCase):
    """Test the Global CPU Map — central to the patent's VMM-side logic."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=16)

    def test_register_vm(self):
        """AC-SR: VM registration in global map."""
        vm = self.gm.register_vm("vm1", 4, list(range(0, 8)))
        self.assertEqual(vm.domain, "vm1")
        self.assertEqual(vm.vcpu_count, 4)
        self.assertIn("vm1", self.gm.vm_states)

    def test_unregister_vm_releases_exclusive(self):
        """Unregistering a VM releases all its exclusive pCPUs."""
        self.gm.register_vm("vm1", 4, list(range(0, 8)))
        self.gm.pin_exclusive("vm1", 0)
        self.gm.pin_exclusive("vm1", 1)
        self.assertEqual(len(self.gm.exclusive_map), 2)

        self.gm.unregister_vm("vm1")
        self.assertEqual(len(self.gm.exclusive_map), 0)
        self.assertNotIn("vm1", self.gm.vm_states)


class TestDynamic1to1Pinning(unittest.TestCase):
    """
    Test: VMM dynamically 1:1 pins a vCPU when guest app pins.
    This is the core innovation of the patent.
    """

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))

    def test_pin_exclusive_basic(self):
        """Guest app pins → VMM switches vCPU to exclusive 1:1."""
        ok, pcpu, msg = self.gm.pin_exclusive("vm1", 2, guest_pid=5678)
        self.assertTrue(ok)
        self.assertIsNotNone(pcpu)
        self.assertIn(pcpu, range(0, 16))

        vcpu = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu.mode, PinMode.EXCLUSIVE)
        self.assertEqual(vcpu.pinned_pcpu, pcpu)
        self.assertEqual(vcpu.source, PinSource.GUEST_PIN)
        self.assertEqual(vcpu.guest_pid, 5678)

    def test_pin_exclusive_records_in_global_map(self):
        """1:1 pin is recorded in the global exclusive map."""
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 2)
        self.assertTrue(ok)
        self.assertIn(pcpu, self.gm.exclusive_map)
        self.assertEqual(self.gm.exclusive_map[pcpu], ("vm1", 2))

    def test_pin_exclusive_idempotent(self):
        """Pinning an already-exclusive vCPU returns success."""
        ok1, pcpu1, _ = self.gm.pin_exclusive("vm1", 0)
        ok2, pcpu2, _ = self.gm.pin_exclusive("vm1", 0)
        self.assertTrue(ok1)
        self.assertTrue(ok2)
        self.assertEqual(pcpu1, pcpu2)

    def test_pin_multiple_vcpus_different_pcpus(self):
        """Multiple vCPUs get different pCPUs."""
        results = []
        for i in range(4):
            ok, pcpu, _ = self.gm.pin_exclusive("vm1", i)
            self.assertTrue(ok)
            results.append(pcpu)
        self.assertEqual(len(set(results)), 4,
                         "Each vCPU must get a unique pCPU")

    def test_pin_unknown_vm(self):
        ok, pcpu, msg = self.gm.pin_exclusive("nonexistent", 0)
        self.assertFalse(ok)
        self.assertIn("not registered", msg)

    def test_pin_unknown_vcpu(self):
        ok, pcpu, msg = self.gm.pin_exclusive("vm1", 99)
        self.assertFalse(ok)
        self.assertIn("not found", msg)


class TestAutoUnpinRestore(unittest.TestCase):
    """Test: VMM auto-restores range pinning when guest app un-pins."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))

    def test_unpin_restore_basic(self):
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 2, guest_pid=5678)
        self.assertTrue(ok)

        ok, msg = self.gm.unpin_restore("vm1", 2)
        self.assertTrue(ok)

        vcpu = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu.mode, PinMode.RANGE)
        self.assertIsNone(vcpu.pinned_pcpu)
        self.assertEqual(vcpu.source, PinSource.DEFAULT)
        self.assertIsNone(vcpu.guest_pid)

    def test_unpin_restore_releases_pcpu(self):
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 2)
        self.assertIn(pcpu, self.gm.exclusive_map)

        self.gm.unpin_restore("vm1", 2)
        self.assertNotIn(pcpu, self.gm.exclusive_map)

    def test_unpin_already_range(self):
        ok, msg = self.gm.unpin_restore("vm1", 0)
        self.assertTrue(ok)
        self.assertIn("already in range", msg)

    def test_unpin_all(self):
        for i in range(4):
            self.gm.pin_exclusive("vm1", i)
        self.assertEqual(len(self.gm.exclusive_map), 4)

        ok, results = self.gm.unpin_all("vm1")
        self.assertTrue(ok)
        self.assertEqual(len(self.gm.exclusive_map), 0)

        for vcpu in self.gm.vm_states["vm1"].vcpus.values():
            self.assertEqual(vcpu.mode, PinMode.RANGE)


class TestConflictAvoidance(unittest.TestCase):
    """Test: VMM avoids assigning the same pCPU to multiple vCPUs."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.gm.register_vm("vm2", 4, list(range(0, 16)))

    def test_cross_vm_conflict_avoidance(self):
        ok1, pcpu1, _ = self.gm.pin_exclusive("vm1", 0)
        ok2, pcpu2, _ = self.gm.pin_exclusive("vm2", 0)

        self.assertTrue(ok1)
        self.assertTrue(ok2)
        self.assertNotEqual(pcpu1, pcpu2,
                            "Different VMs must get different pCPUs")

    def test_exhaust_cpuset(self):
        self.gm.register_vm("tiny-vm", 4, [0, 1])

        ok1, _, _ = self.gm.pin_exclusive("tiny-vm", 0)
        ok2, _, _ = self.gm.pin_exclusive("tiny-vm", 1)
        self.assertTrue(ok1)
        self.assertTrue(ok2)

        ok3, _, msg = self.gm.pin_exclusive("tiny-vm", 2)
        self.assertFalse(ok3, "Should fail when no free pCPUs")
        self.assertIn("No free pCPU", msg)

    def test_released_pcpu_reusable(self):
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 0)
        self.gm.unpin_restore("vm1", 0)

        ok2, pcpu2, _ = self.gm.pin_exclusive("vm2", 0)
        self.assertTrue(ok2)


# ============================================================================
# Test: Persistence
# ============================================================================

class TestPersistence(unittest.TestCase):
    """Test global CPU map persistence."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.map_file = os.path.join(self.tmpdir, "global_cpu_map.json")

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_save_and_load(self):
        gm = GlobalCpuMap(total_pcpus=16)
        gm.register_vm("vm1", 4, list(range(0, 8)))
        gm.pin_exclusive("vm1", 2, guest_pid=1234)
        gm.save(self.map_file)

        gm2 = GlobalCpuMap.load(self.map_file)
        self.assertEqual(gm2.total_pcpus, 16)
        self.assertIn("vm1", gm2.vm_states)
        self.assertEqual(
            gm2.vm_states["vm1"].vcpus[2].mode, PinMode.EXCLUSIVE
        )
        self.assertEqual(gm2.vm_states["vm1"].vcpus[2].guest_pid, 1234)

    def test_load_nonexistent(self):
        gm = GlobalCpuMap.load("/nonexistent/path/map.json")
        self.assertEqual(len(gm.vm_states), 0)

    def test_config_file_valid_json(self):
        gm = GlobalCpuMap(total_pcpus=8)
        gm.register_vm("vm1", 2, [0, 1, 2, 3])
        gm.save(self.map_file)

        with open(self.map_file) as f:
            data = json.load(f)
        self.assertIn("total_pcpus", data)
        self.assertIn("exclusive_map", data)
        self.assertIn("vm_states", data)


# ============================================================================
# Test: PinExecutor
# ============================================================================

class TestPinExecutor(unittest.TestCase):
    """Test the pin executor (dry-run mode)."""

    def setUp(self):
        self.executor = PinExecutor(dry_run=True)

    def test_pin_vcpu(self):
        ok = self.executor.pin_vcpu("vm1", 2, 8)
        self.assertTrue(ok)
        self.assertEqual(len(self.executor.history), 1)
        self.assertEqual(self.executor.history[0], ("pin", "vm1", 2, 8))

    def test_restore_vcpu(self):
        ok = self.executor.restore_vcpu("vm1", 2, [0, 1, 2, 3])
        self.assertTrue(ok)
        self.assertEqual(len(self.executor.history), 1)
        self.assertEqual(
            self.executor.history[0], ("restore", "vm1", 2, "0-3")
        )


# ============================================================================
# Test: VMM Pin Handler
# ============================================================================

class TestVmmPinHandler(unittest.TestCase):
    """Test the VMM-side notification handler."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.executor = PinExecutor(dry_run=True)
        self.handler = VmmPinHandler(self.gm, executor=self.executor)

    def test_handle_pin_notification(self):
        notif = GuestPinNotification("vm1", 2, "pin", guest_pid=5678)
        ok, msg = self.handler.handle_notification(notif)
        self.assertTrue(ok)
        self.assertEqual(
            self.gm.vm_states["vm1"].vcpus[2].mode, PinMode.EXCLUSIVE
        )
        # Executor was called
        self.assertEqual(len(self.executor.history), 1)
        self.assertEqual(self.executor.history[0][0], "pin")

    def test_handle_unpin_notification(self):
        self.handler.handle_notification(
            GuestPinNotification("vm1", 2, "pin", guest_pid=5678)
        )
        notif = GuestPinNotification("vm1", 2, "unpin")
        ok, msg = self.handler.handle_notification(notif)
        self.assertTrue(ok)
        self.assertEqual(
            self.gm.vm_states["vm1"].vcpus[2].mode, PinMode.RANGE
        )
        self.assertEqual(len(self.executor.history), 2)
        self.assertEqual(self.executor.history[1][0], "restore")

    def test_handle_batch(self):
        notifications = [
            GuestPinNotification("vm1", 0, "pin", guest_pid=100),
            GuestPinNotification("vm1", 1, "pin", guest_pid=200),
            GuestPinNotification("vm1", 2, "pin", guest_pid=300),
        ]
        results = self.handler.handle_batch(notifications)
        self.assertEqual(len(results), 3)
        for r in results:
            self.assertTrue(r["ok"])

    def test_handle_unknown_action(self):
        notif = GuestPinNotification("vm1", 0, "invalid")
        ok, msg = self.handler.handle_notification(notif)
        self.assertFalse(ok)
        self.assertIn("Unknown action", msg)


class TestGuestPinNotification(unittest.TestCase):
    """Test guest notification serialization."""

    def test_serialize_roundtrip(self):
        notif = GuestPinNotification(
            "vm1", 2, "pin", guest_pid=1234, target_vcpus=[2]
        )
        d = notif.to_dict()
        restored = GuestPinNotification.from_dict(d)
        self.assertEqual(restored.domain, "vm1")
        self.assertEqual(restored.vcpu_id, 2)
        self.assertEqual(restored.action, "pin")
        self.assertEqual(restored.guest_pid, 1234)


# ============================================================================
# Test: Guest-side eBPF interceptor
# ============================================================================

class TestEbpfInterceptor(unittest.TestCase):
    """Test eBPF-based asynchronous interceptor."""

    def test_is_pin_action(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        self.assertTrue(interceptor.is_pin_action([2]))
        self.assertTrue(interceptor.is_pin_action([0, 1]))
        self.assertFalse(interceptor.is_pin_action([0, 1, 2, 3]))

    def test_callback_invoked(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        events = []
        interceptor.register_callback(lambda e: events.append(e))

        interceptor.intercept(pid=1234, comm="myapp", cpu_mask=[2])
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].pid, 1234)
        self.assertTrue(events[0].is_affinity_pin)

    def test_async_ring_buffer(self):
        interceptor = EbpfInterceptor(total_vcpus=4)

        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        self.assertEqual(len(interceptor.ring_buffer), 2)

    def test_drain_ring_buffer(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        events = interceptor.drain_ring_buffer()
        self.assertEqual(len(events), 2)
        self.assertEqual(len(interceptor.ring_buffer), 0)

    def test_unpin_detection(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        event = interceptor.intercept(
            pid=100, comm="app", cpu_mask=[0, 1, 2, 3]
        )
        self.assertFalse(event.is_affinity_pin)

    def test_attach_detach(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        self.assertFalse(interceptor.is_attached)
        interceptor.attach()
        self.assertTrue(interceptor.is_attached)
        interceptor.detach()
        self.assertFalse(interceptor.is_attached)


# ============================================================================
# Test: Notification transport
# ============================================================================

class TestSimulatedTransport(unittest.TestCase):
    """Test simulated notification transport."""

    def test_send(self):
        transport = SimulatedTransport()
        transport.connect()
        transport.send({"pid": 123, "is_affinity_pin": True})
        transport.send({"pid": 456, "is_affinity_pin": False})
        self.assertEqual(len(transport.sent_messages), 2)
        self.assertEqual(transport.sent_messages[0]["pid"], 123)
        transport.close()


# ============================================================================
# Test: Notification agent
# ============================================================================

class TestNotificationAgent(unittest.TestCase):
    """Test the user-space notification agent."""

    def test_process_events_with_transport(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        transport = SimulatedTransport()
        transport.connect()
        agent = NotificationAgent(interceptor, transport=transport)

        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        results = agent.process_events()
        self.assertEqual(len(results), 2)
        self.assertEqual(len(transport.sent_messages), 2)

    def test_process_events_with_callback(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        results = []
        agent = NotificationAgent(
            interceptor, vmm_callback=lambda e: results.append(e)
        )

        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        agent.process_events()
        self.assertEqual(len(results), 1)

    def test_empty_buffer(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(interceptor)
        results = agent.process_events()
        self.assertEqual(len(results), 0)


# ============================================================================
# Test: End-to-end flow (Guest interception → Host handling)
# ============================================================================

class TestEndToEndFlow(unittest.TestCase):
    """
    Integration test: full flow from guest app pinning to host 1:1 binding.
    This validates the complete patent architecture.
    """

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.executor = PinExecutor(dry_run=True)
        self.handler = VmmPinHandler(self.gm, executor=self.executor)

    def _make_vmm_callback(self, domain):
        """Create a VMM callback that processes guest events."""
        def callback(event):
            if event.is_affinity_pin:
                vcpu_id = event.cpu_mask[0] if event.cpu_mask else 0
                notif = GuestPinNotification(
                    domain=domain,
                    vcpu_id=vcpu_id,
                    action="pin",
                    guest_pid=event.pid,
                )
                return self.handler.handle_notification(notif)
            else:
                vm = self.gm.get_vm_status(domain)
                results = []
                if vm:
                    for vid, vs in vm.vcpus.items():
                        if (vs.mode == PinMode.EXCLUSIVE
                                and vs.guest_pid == event.pid):
                            notif = GuestPinNotification(
                                domain=domain,
                                vcpu_id=vid,
                                action="unpin",
                            )
                            results.append(
                                self.handler.handle_notification(notif)
                            )
                return results[-1] if results else (True, "No vCPUs to unpin")
        return callback

    def test_ebpf_full_flow_pin(self):
        """
        Full flow: App pins → eBPF intercepts → ring buffer → agent →
        VMM handler → 1:1 exclusive pin.
        """
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm1")
        )

        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[2])
        results = agent.process_events()

        self.assertEqual(len(results), 1)
        ok, msg = results[0]
        self.assertTrue(ok)

        vcpu2 = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu2.mode, PinMode.EXCLUSIVE)
        self.assertIsNotNone(vcpu2.pinned_pcpu)

        # Executor was called
        self.assertTrue(
            any(h[0] == "pin" for h in self.executor.history)
        )

    def test_ebpf_full_flow_pin_then_unpin(self):
        """Full flow: App pins → 1:1 → App un-pins → restore range."""
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm1")
        )

        # Pin
        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[2])
        agent.process_events()

        vcpu2 = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu2.mode, PinMode.EXCLUSIVE)
        saved_pcpu = vcpu2.pinned_pcpu

        # Unpin
        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[0, 1, 2, 3])
        agent.process_events()

        self.assertNotIn(saved_pcpu, self.gm.exclusive_map)

    def test_ebpf_flow_with_simulated_transport(self):
        """Test eBPF → SimulatedTransport path (no VMM callback)."""
        interceptor = EbpfInterceptor(total_vcpus=4)
        transport = SimulatedTransport()
        transport.connect()
        agent = NotificationAgent(interceptor, transport=transport)

        interceptor.intercept(pid=100, comm="app", cpu_mask=[1])
        results = agent.process_events()

        self.assertEqual(len(results), 1)
        self.assertEqual(len(transport.sent_messages), 1)
        msg = transport.sent_messages[0]
        self.assertEqual(msg["pid"], 100)
        self.assertTrue(msg["is_affinity_pin"])

    def test_multi_vm_isolation(self):
        """Multiple VMs with overlapping cpusets maintain isolation."""
        self.gm.register_vm("vm2", 2, list(range(0, 16)))
        interceptor = EbpfInterceptor(total_vcpus=4)

        agent1 = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm1")
        )
        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        agent1.process_events()

        agent2 = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm2")
        )
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[0])
        agent2.process_events()

        pcpu1 = self.gm.vm_states["vm1"].vcpus[0].pinned_pcpu
        pcpu2 = self.gm.vm_states["vm2"].vcpus[0].pinned_pcpu

        self.assertNotEqual(pcpu1, pcpu2,
                            "Different VMs must get different pCPUs")


# ============================================================================
# Test: Event log
# ============================================================================

class TestEventLog(unittest.TestCase):
    """Test event log writing."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.log_file = os.path.join(self.tmpdir, "events.log")
        # Monkey-patch LOG_FILE
        import vaffinity
        self._orig_log_file = vaffinity.LOG_FILE
        vaffinity.LOG_FILE = self.log_file

    def tearDown(self):
        import shutil
        import vaffinity
        vaffinity.LOG_FILE = self._orig_log_file
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_write_event_log(self):
        _write_event_log("PIN", "vm1", 2, "pcpu=8 source=guest-pin pid=1234")
        _write_event_log("UNPIN", "vm1", 2, "cpuset=0-15 source=guest-unpin")

        with open(self.log_file) as f:
            lines = f.readlines()
        self.assertEqual(len(lines), 2)
        self.assertIn("[PIN]", lines[0])
        self.assertIn("[UNPIN]", lines[1])
        self.assertIn("vm=vm1", lines[0])


# ============================================================================
# Test: CLI parsing
# ============================================================================

class TestCLIParsing(unittest.TestCase):
    """Test CLI argument parsing."""

    def test_no_command(self):
        from unittest.mock import patch
        import io
        with patch("sys.argv", ["vaffinity"]):
            from contextlib import redirect_stdout, redirect_stderr
            with redirect_stdout(io.StringIO()), \
                 redirect_stderr(io.StringIO()):
                ret = main()
        self.assertEqual(ret, 1)


# ============================================================================
# Test: pCPU status query
# ============================================================================

class TestPcpuStatus(unittest.TestCase):
    """Test global pCPU status queries."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=8)
        self.gm.register_vm("vm1", 2, [0, 1, 2, 3])
        self.gm.register_vm("vm2", 2, [2, 3, 4, 5])

    def test_get_pcpu_status_free(self):
        mode, domain, vcpu_id = self.gm.get_pcpu_status(6)
        self.assertEqual(mode, PinMode.RANGE)
        self.assertIsNone(domain)

    def test_get_pcpu_status_exclusive(self):
        self.gm.pin_exclusive("vm1", 0)
        pcpu = self.gm.vm_states["vm1"].vcpus[0].pinned_pcpu
        mode, domain, vcpu_id = self.gm.get_pcpu_status(pcpu)
        self.assertEqual(mode, PinMode.EXCLUSIVE)
        self.assertEqual(domain, "vm1")
        self.assertEqual(vcpu_id, 0)

    def test_get_all_pcpu_status(self):
        self.gm.pin_exclusive("vm1", 0)
        all_status = self.gm.get_all_pcpu_status()
        self.assertEqual(len(all_status), 8)

        for pcpu in [0, 1, 2, 3]:
            info = all_status[pcpu]
            if info["mode"] == PinMode.RANGE:
                self.assertIn("vm1", info["sharing_vms"])


# ============================================================================
# Test: Edge cases
# ============================================================================

class TestEdgeCases(unittest.TestCase):
    """Test edge cases and boundary conditions."""

    def test_single_vcpu_vm(self):
        gm = GlobalCpuMap(total_pcpus=8)
        gm.register_vm("tiny", 1, [0])
        ok, pcpu, _ = gm.pin_exclusive("tiny", 0)
        self.assertTrue(ok)
        self.assertEqual(pcpu, 0)

    def test_large_cpuset(self):
        gm = GlobalCpuMap(total_pcpus=128)
        gm.register_vm("big", 64, list(range(128)))
        for i in range(64):
            ok, pcpu, _ = gm.pin_exclusive("big", i)
            self.assertTrue(ok)
        self.assertEqual(len(gm.exclusive_map), 64)

    def test_register_same_vm_twice(self):
        gm = GlobalCpuMap(total_pcpus=8)
        gm.register_vm("vm1", 2, [0, 1])
        gm.register_vm("vm1", 2, [0, 1])
        self.assertEqual(len(gm.vm_states), 1)

    def test_unregister_nonexistent(self):
        gm = GlobalCpuMap(total_pcpus=8)
        gm.unregister_vm("nonexistent")  # Should not raise


if __name__ == "__main__":
    unittest.main()
