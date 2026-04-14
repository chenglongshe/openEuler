#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# Test suite for vm-bindcore (VMM-side handler + Guest-side interceptor)
# Based on patent: 一种优化虚拟机内业务绑核性能的方法 (Inventor: 张海亮)

"""
Unit and integration tests for vm-bindcore.

Tests cover the patent's core architecture:
  1. Global CPU Map management
  2. Dynamic 1:1 pinning on guest notification
  3. Automatic un-pin restore
  4. Conflict avoidance
  5. Guest-side interception (kprobe/eBPF simulation)
  6. End-to-end guest→VMM notification flow

Run with: python3 -m pytest tests/test_vm_bindcore.py -v
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Add source directory to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from vm_bindcore import (  # noqa: E402
    GlobalCpuMap,
    GuestPinNotification,
    PinMode,
    PinSource,
    VcpuPinState,
    VmPinState,
    VmmPinHandler,
    _format_cpulist,
    _parse_cpulist,
    main,
)
from vm_bindcore_guest import (  # noqa: E402
    AffinityEvent,
    EbpfInterceptor,
    InterceptorBase,
    KprobeInterceptor,
    NotificationAgent,
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
        self.assertEqual(_parse_cpulist("0-3,8,12-15"), [0, 1, 2, 3, 8, 12, 13, 14, 15])

    def test_whitespace(self):
        self.assertEqual(_parse_cpulist(" 0-3 , 8-11 "), [0, 1, 2, 3, 8, 9, 10, 11])


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
    Test AC-SR-03: VMM dynamically 1:1 pins a vCPU when guest app pins.
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
        self.assertIn(pcpu, range(0, 16))  # Must be within VM's cpuset

        # Verify state
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
        """Pinning an already-exclusive vCPU returns success without change."""
        ok1, pcpu1, _ = self.gm.pin_exclusive("vm1", 0)
        ok2, pcpu2, _ = self.gm.pin_exclusive("vm1", 0)
        self.assertTrue(ok1)
        self.assertTrue(ok2)
        self.assertEqual(pcpu1, pcpu2)

    def test_pin_multiple_vcpus_different_pcpus(self):
        """AC-US-03-03: Multiple vCPUs get different pCPUs."""
        results = []
        for i in range(4):
            ok, pcpu, _ = self.gm.pin_exclusive("vm1", i)
            self.assertTrue(ok)
            results.append(pcpu)

        # All pCPUs must be unique
        self.assertEqual(len(set(results)), 4, "Each vCPU must get a unique pCPU")

    def test_pin_unknown_vm(self):
        """Pinning for unknown VM returns error."""
        ok, pcpu, msg = self.gm.pin_exclusive("nonexistent", 0)
        self.assertFalse(ok)
        self.assertIn("not registered", msg)

    def test_pin_unknown_vcpu(self):
        """Pinning for unknown vCPU returns error."""
        ok, pcpu, msg = self.gm.pin_exclusive("vm1", 99)
        self.assertFalse(ok)
        self.assertIn("not found", msg)


class TestAutoUnpinRestore(unittest.TestCase):
    """
    Test AC-SR-04: VMM auto-restores range pinning when guest app un-pins.
    """

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))

    def test_unpin_restore_basic(self):
        """Guest app un-pins → VMM restores range mode."""
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
        """Un-pin releases the pCPU from the exclusive map."""
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 2)
        self.assertIn(pcpu, self.gm.exclusive_map)

        self.gm.unpin_restore("vm1", 2)
        self.assertNotIn(pcpu, self.gm.exclusive_map)

    def test_unpin_already_range(self):
        """Un-pinning a vCPU already in range mode is a no-op success."""
        ok, msg = self.gm.unpin_restore("vm1", 0)
        self.assertTrue(ok)
        self.assertIn("already in range", msg)

    def test_unpin_all(self):
        """Unpin all vCPUs of a VM."""
        for i in range(4):
            self.gm.pin_exclusive("vm1", i)
        self.assertEqual(len(self.gm.exclusive_map), 4)

        ok, results = self.gm.unpin_all("vm1")
        self.assertTrue(ok)
        self.assertEqual(len(self.gm.exclusive_map), 0)

        for vcpu in self.gm.vm_states["vm1"].vcpus.values():
            self.assertEqual(vcpu.mode, PinMode.RANGE)


class TestConflictAvoidance(unittest.TestCase):
    """
    Test AC-SR-05: VMM avoids assigning the same pCPU to multiple vCPUs.
    This is a key patent requirement.
    """

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.gm.register_vm("vm2", 4, list(range(0, 16)))  # Overlapping cpuset

    def test_cross_vm_conflict_avoidance(self):
        """vm1 and vm2 with overlapping cpusets don't get the same pCPU."""
        ok1, pcpu1, _ = self.gm.pin_exclusive("vm1", 0)
        ok2, pcpu2, _ = self.gm.pin_exclusive("vm2", 0)

        self.assertTrue(ok1)
        self.assertTrue(ok2)
        self.assertNotEqual(pcpu1, pcpu2, "Different VMs must get different pCPUs")

    def test_exhaust_cpuset(self):
        """When all pCPUs in cpuset are occupied, return failure (graceful degradation)."""
        # Small cpuset
        self.gm.register_vm("tiny-vm", 4, [0, 1])

        ok1, pcpu1, _ = self.gm.pin_exclusive("tiny-vm", 0)
        ok2, pcpu2, _ = self.gm.pin_exclusive("tiny-vm", 1)
        self.assertTrue(ok1)
        self.assertTrue(ok2)

        # Now try to pin vcpu2 — but only 2 pCPUs and both are taken
        ok3, pcpu3, msg = self.gm.pin_exclusive("tiny-vm", 2)
        self.assertFalse(ok3, "Should fail when no free pCPUs")
        self.assertIn("No free pCPU", msg)

    def test_released_pcpu_reusable(self):
        """After un-pin, the released pCPU can be reused by another vCPU."""
        ok, pcpu, _ = self.gm.pin_exclusive("vm1", 0)
        self.gm.unpin_restore("vm1", 0)

        # Same pCPU should be available again
        ok2, pcpu2, _ = self.gm.pin_exclusive("vm2", 0)
        self.assertTrue(ok2)
        # pcpu2 could be the same as pcpu (now it's free)


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
        self.assertEqual(gm2.vm_states["vm1"].vcpus[2].mode, PinMode.EXCLUSIVE)
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
# Test: VMM Pin Handler (processes guest notifications)
# ============================================================================

class TestVmmPinHandler(unittest.TestCase):
    """Test the VMM-side notification handler."""

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.handler = VmmPinHandler(self.gm)

    def test_handle_pin_notification(self):
        """Process a 'pin' notification from guest."""
        notif = GuestPinNotification("vm1", 2, "pin", guest_pid=5678)
        ok, msg = self.handler.handle_notification(notif)
        self.assertTrue(ok)
        self.assertEqual(self.gm.vm_states["vm1"].vcpus[2].mode, PinMode.EXCLUSIVE)

    def test_handle_unpin_notification(self):
        """Process an 'unpin' notification from guest."""
        # First pin
        self.handler.handle_notification(
            GuestPinNotification("vm1", 2, "pin", guest_pid=5678)
        )
        # Then unpin
        notif = GuestPinNotification("vm1", 2, "unpin")
        ok, msg = self.handler.handle_notification(notif)
        self.assertTrue(ok)
        self.assertEqual(self.gm.vm_states["vm1"].vcpus[2].mode, PinMode.RANGE)

    def test_handle_batch(self):
        """Process a batch of notifications."""
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
        """Unknown action should return error."""
        notif = GuestPinNotification("vm1", 0, "invalid")
        ok, msg = self.handler.handle_notification(notif)
        self.assertFalse(ok)
        self.assertIn("Unknown action", msg)


class TestGuestPinNotification(unittest.TestCase):
    """Test guest notification serialization."""

    def test_serialize_roundtrip(self):
        notif = GuestPinNotification("vm1", 2, "pin", guest_pid=1234, target_vcpus=[2])
        d = notif.to_dict()
        restored = GuestPinNotification.from_dict(d)
        self.assertEqual(restored.domain, "vm1")
        self.assertEqual(restored.vcpu_id, 2)
        self.assertEqual(restored.action, "pin")
        self.assertEqual(restored.guest_pid, 1234)


# ============================================================================
# Test: Guest-side interceptor
# ============================================================================

class TestInterceptorBase(unittest.TestCase):
    """Test base interceptor logic."""

    def test_is_pin_action(self):
        interceptor = InterceptorBase(total_vcpus=4)
        self.assertTrue(interceptor.is_pin_action([2]))       # Pin to single vCPU
        self.assertTrue(interceptor.is_pin_action([0, 1]))    # Pin to subset
        self.assertFalse(interceptor.is_pin_action([0, 1, 2, 3]))  # All CPUs = unpin

    def test_callback_invoked(self):
        interceptor = InterceptorBase(total_vcpus=4)
        events = []
        interceptor.register_callback(lambda e: events.append(e))

        interceptor.intercept(pid=1234, comm="myapp", cpu_mask=[2])
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0].pid, 1234)
        self.assertTrue(events[0].is_bindcore)


class TestKprobeInterceptor(unittest.TestCase):
    """Test kprobe-based synchronous interceptor."""

    def test_synchronous_interception(self):
        interceptor = KprobeInterceptor(total_vcpus=4)
        events = []
        interceptor.register_callback(lambda e: events.append(e))

        event = interceptor.intercept(pid=100, comm="app", cpu_mask=[1])
        self.assertTrue(event.is_bindcore)
        self.assertEqual(interceptor.mode, "kprobe")
        self.assertEqual(len(events), 1)


class TestEbpfInterceptor(unittest.TestCase):
    """Test eBPF-based asynchronous interceptor."""

    def test_async_ring_buffer(self):
        """Events are queued to ring buffer for async processing."""
        interceptor = EbpfInterceptor(total_vcpus=4)

        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        self.assertEqual(len(interceptor.ring_buffer), 2)

    def test_drain_ring_buffer(self):
        """Draining returns all events and clears the buffer."""
        interceptor = EbpfInterceptor(total_vcpus=4)
        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        events = interceptor.drain_ring_buffer()
        self.assertEqual(len(events), 2)
        self.assertEqual(len(interceptor.ring_buffer), 0)  # Buffer cleared

    def test_unpin_detection(self):
        """Restoring to all CPUs is detected as unpin."""
        interceptor = EbpfInterceptor(total_vcpus=4)
        event = interceptor.intercept(pid=100, comm="app", cpu_mask=[0, 1, 2, 3])
        self.assertFalse(event.is_bindcore)  # All CPUs = unpin


class TestNotificationAgent(unittest.TestCase):
    """Test the user-space notification agent (eBPF async path)."""

    def test_process_events(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        interceptor.intercept(pid=100, comm="app1", cpu_mask=[0])
        interceptor.intercept(pid=200, comm="app2", cpu_mask=[1])

        results = []
        agent = NotificationAgent(interceptor, vmm_callback=lambda e: results.append(e))
        agent.process_events()

        self.assertEqual(len(results), 2)

    def test_empty_buffer(self):
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(interceptor)
        results = agent.process_events()
        self.assertEqual(len(results), 0)


# ============================================================================
# Test: End-to-end flow (Guest interception → VMM handling)
# ============================================================================

class TestEndToEndFlow(unittest.TestCase):
    """
    Integration test: full flow from guest app pinning to VMM 1:1 binding.
    This validates the complete patent architecture.
    """

    def setUp(self):
        self.gm = GlobalCpuMap(total_pcpus=64)
        self.gm.register_vm("vm1", 4, list(range(0, 16)))
        self.handler = VmmPinHandler(self.gm)

    def _make_vmm_callback(self, domain):
        """Create a VMM callback that processes guest events."""
        def callback(event):
            if event.is_bindcore:
                # Pin: the target vCPU is the single CPU in the mask
                vcpu_id = event.cpu_mask[0] if event.cpu_mask else 0
                notif = GuestPinNotification(
                    domain=domain,
                    vcpu_id=vcpu_id,
                    action="pin",
                    guest_pid=event.pid,
                )
                return self.handler.handle_notification(notif)
            else:
                # Unpin: restore ALL vCPUs that were pinned by this PID
                vm = self.gm.get_vm_status(domain)
                results = []
                if vm:
                    for vid, vs in vm.vcpus.items():
                        if vs.mode == PinMode.EXCLUSIVE and vs.guest_pid == event.pid:
                            notif = GuestPinNotification(
                                domain=domain,
                                vcpu_id=vid,
                                action="unpin",
                            )
                            results.append(self.handler.handle_notification(notif))
                return results[-1] if results else (True, "No vCPUs to unpin")
        return callback

    def test_ebpf_full_flow_pin(self):
        """
        Full flow: App pins via sched_setaffinity → eBPF intercepts →
        ring buffer → agent → VMM handler → 1:1 exclusive pin.
        """
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm1")
        )

        # Guest app pins to vCPU 2
        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[2])
        results = agent.process_events()

        self.assertEqual(len(results), 1)
        ok, msg = results[0]
        self.assertTrue(ok)

        # Verify VMM state
        vcpu2 = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu2.mode, PinMode.EXCLUSIVE)
        self.assertIsNotNone(vcpu2.pinned_pcpu)

    def test_ebpf_full_flow_pin_then_unpin(self):
        """
        Full flow: App pins → VMM 1:1 → App un-pins → VMM restores range.
        """
        interceptor = EbpfInterceptor(total_vcpus=4)
        agent = NotificationAgent(
            interceptor, vmm_callback=self._make_vmm_callback("vm1")
        )

        # Step 1: App pins to vCPU 2
        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[2])
        agent.process_events()

        vcpu2 = self.gm.vm_states["vm1"].vcpus[2]
        self.assertEqual(vcpu2.mode, PinMode.EXCLUSIVE)
        saved_pcpu = vcpu2.pinned_pcpu

        # Step 2: App un-pins (restore to all vCPUs)
        interceptor.intercept(pid=5678, comm="myapp", cpu_mask=[0, 1, 2, 3])
        results = agent.process_events()

        # Since all CPUs = unpin, agent should send unpin notification
        # The callback uses cpu_mask[0] as vcpu_id for unpin
        # Verify the pCPU is released
        self.assertNotIn(saved_pcpu, self.gm.exclusive_map)

    def test_kprobe_full_flow(self):
        """Full flow using kprobe synchronous path."""
        interceptor = KprobeInterceptor(total_vcpus=4)

        # Kprobe is synchronous — callback fires immediately
        interceptor.register_callback(self._make_vmm_callback("vm1"))
        interceptor.intercept(pid=1234, comm="dbapp", cpu_mask=[1])

        vcpu1 = self.gm.vm_states["vm1"].vcpus[1]
        self.assertEqual(vcpu1.mode, PinMode.EXCLUSIVE)

    def test_multi_vm_isolation(self):
        """Multiple VMs with overlapping cpusets maintain isolation."""
        self.gm.register_vm("vm2", 2, list(range(0, 16)))
        interceptor = EbpfInterceptor(total_vcpus=4)

        # Pin vm1:vcpu0 and vm2:vcpu0
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

        self.assertNotEqual(pcpu1, pcpu2, "Different VMs must get different pCPUs")


# ============================================================================
# Test: CLI parsing
# ============================================================================

class TestCLIParsing(unittest.TestCase):
    """Test CLI argument parsing."""

    def test_no_command(self):
        """No subcommand returns 1."""
        from unittest.mock import patch
        import io
        with patch("sys.argv", ["vm-bindcore"]):
            from contextlib import redirect_stdout, redirect_stderr
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
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

        # Check shared pCPUs
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
        gm.register_vm("vm1", 2, [0, 1])  # Should be idempotent
        self.assertEqual(len(gm.vm_states), 1)

    def test_unregister_nonexistent(self):
        gm = GlobalCpuMap(total_pcpus=8)
        gm.unregister_vm("nonexistent")  # Should not raise


if __name__ == "__main__":
    unittest.main()
