#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# Test suite for vm-bindcore
# Tests cover: NumaTopology, PinningEngine, PinningConfig, CLI parsing

"""
Unit and integration tests for vm-bindcore.

Run with: python3 -m pytest tests/test_vm_bindcore.py -v
"""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add source directory to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from vm_bindcore import (  # noqa: E402
    NumaTopology,
    PinningConfig,
    PinningEngine,
    _format_cpulist,
    main,
)


class TestNumaTopologyParseCpulist(unittest.TestCase):
    """Test CPU list string parsing."""

    def test_single_cpu(self):
        result = NumaTopology._parse_cpulist("0")
        self.assertEqual(result, [0])

    def test_cpu_range(self):
        result = NumaTopology._parse_cpulist("0-3")
        self.assertEqual(result, [0, 1, 2, 3])

    def test_mixed_range_and_single(self):
        result = NumaTopology._parse_cpulist("0-3,8,12-15")
        self.assertEqual(result, [0, 1, 2, 3, 8, 12, 13, 14, 15])

    def test_single_large_cpu(self):
        result = NumaTopology._parse_cpulist("127")
        self.assertEqual(result, [127])

    def test_multiple_ranges(self):
        result = NumaTopology._parse_cpulist("0-1,4-5,8-9")
        self.assertEqual(result, [0, 1, 4, 5, 8, 9])

    def test_whitespace_handling(self):
        result = NumaTopology._parse_cpulist(" 0-3 , 8-11 ")
        self.assertEqual(result, [0, 1, 2, 3, 8, 9, 10, 11])


class TestFormatCpulist(unittest.TestCase):
    """Test CPU list formatting."""

    def test_empty_list(self):
        self.assertEqual(_format_cpulist([]), "(none)")

    def test_single_cpu(self):
        self.assertEqual(_format_cpulist([5]), "5")

    def test_contiguous_range(self):
        self.assertEqual(_format_cpulist([0, 1, 2, 3]), "0-3")

    def test_mixed(self):
        self.assertEqual(_format_cpulist([0, 1, 2, 5, 6, 10]), "0-2,5-6,10")

    def test_unsorted_input(self):
        self.assertEqual(_format_cpulist([3, 1, 2, 0]), "0-3")


class MockNumaTopology:
    """Create a mock NUMA topology for testing."""

    @staticmethod
    def two_node_no_smt():
        """2 NUMA nodes, 8 CPUs each, no SMT."""
        topo = NumaTopology.__new__(NumaTopology)
        topo.nodes = {
            0: list(range(0, 8)),
            1: list(range(8, 16)),
        }
        topo.distances = {
            0: {0: 10, 1: 21},
            1: {0: 21, 1: 10},
        }
        topo.core_siblings = {i: [i] for i in range(16)}
        return topo

    @staticmethod
    def two_node_with_smt():
        """2 NUMA nodes, 4 cores * 2 threads each = 16 CPUs."""
        topo = NumaTopology.__new__(NumaTopology)
        topo.nodes = {
            0: [0, 1, 2, 3, 8, 9, 10, 11],   # cores 0-3, threads on 8-11
            1: [4, 5, 6, 7, 12, 13, 14, 15],  # cores 4-7, threads on 12-15
        }
        topo.distances = {
            0: {0: 10, 1: 21},
            1: {0: 21, 1: 10},
        }
        topo.core_siblings = {
            0: [0, 8], 8: [0, 8],
            1: [1, 9], 9: [1, 9],
            2: [2, 10], 10: [2, 10],
            3: [3, 11], 11: [3, 11],
            4: [4, 12], 12: [4, 12],
            5: [5, 13], 13: [5, 13],
            6: [6, 14], 14: [6, 14],
            7: [7, 15], 15: [7, 15],
        }
        return topo

    @staticmethod
    def four_node():
        """4 NUMA nodes, 8 CPUs each = 32 CPUs."""
        topo = NumaTopology.__new__(NumaTopology)
        topo.nodes = {
            0: list(range(0, 8)),
            1: list(range(8, 16)),
            2: list(range(16, 24)),
            3: list(range(24, 32)),
        }
        topo.distances = {
            0: {0: 10, 1: 21, 2: 31, 3: 31},
            1: {0: 21, 1: 10, 2: 31, 3: 31},
            2: {0: 31, 1: 31, 2: 10, 3: 21},
            3: {0: 31, 1: 31, 2: 21, 3: 10},
        }
        topo.core_siblings = {i: [i] for i in range(32)}
        return topo


class TestPinningEngineNumaAware(unittest.TestCase):
    """Test NUMA-aware pinning strategy."""

    def test_basic_numa_aware(self):
        """All vCPUs should be on the same NUMA node."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_numa_aware(4)

        # All CPUs should be from the same node
        all_cpus = []
        for cpulist in mapping.values():
            all_cpus.extend(cpulist)

        nodes = set()
        for cpu in all_cpus:
            nodes.add(topo.get_node_for_cpu(cpu))

        self.assertEqual(len(nodes), 1, "All vCPUs should be on one NUMA node")
        self.assertEqual(len(mapping), 4)

    def test_numa_aware_preferred_node(self):
        """vCPUs should be placed on the preferred node."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_numa_aware(4, preferred_node=1)

        for cpulist in mapping.values():
            for cpu in cpulist:
                self.assertEqual(
                    topo.get_node_for_cpu(cpu), 1,
                    f"CPU {cpu} should be on node 1"
                )

    def test_numa_aware_spillover(self):
        """When preferred node is full, spill to nearest neighbor."""
        topo = MockNumaTopology.two_node_no_smt()
        # Use 6 of 8 CPUs on node 0
        used = set(range(0, 6))
        engine = PinningEngine(topo, used_cpus=used)
        mapping = engine.compute_numa_aware(4, preferred_node=0)

        # Should get 2 from node 0, 2 from node 1
        self.assertEqual(len(mapping), 4)
        node0_count = sum(
            1 for cpulist in mapping.values()
            for cpu in cpulist if topo.get_node_for_cpu(cpu) == 0
        )
        node1_count = sum(
            1 for cpulist in mapping.values()
            for cpu in cpulist if topo.get_node_for_cpu(cpu) == 1
        )
        self.assertEqual(node0_count, 2)
        self.assertEqual(node1_count, 2)

    def test_numa_aware_not_enough_cpus(self):
        """Should raise error when not enough CPUs available."""
        topo = MockNumaTopology.two_node_no_smt()
        used = set(range(0, 16))  # All CPUs used
        engine = PinningEngine(topo, used_cpus=used)

        with self.assertRaises(RuntimeError) as ctx:
            engine.compute_numa_aware(4)
        self.assertIn("Not enough free CPUs", str(ctx.exception))

    def test_numa_aware_four_nodes(self):
        """NUMA-aware with 4 nodes should prefer the best node."""
        topo = MockNumaTopology.four_node()
        engine = PinningEngine(topo)
        mapping = engine.compute_numa_aware(4)

        self.assertEqual(len(mapping), 4)
        # All should be on one node
        nodes = set()
        for cpulist in mapping.values():
            for cpu in cpulist:
                nodes.add(topo.get_node_for_cpu(cpu))
        self.assertEqual(len(nodes), 1)


class TestPinningEngineSpread(unittest.TestCase):
    """Test spread pinning strategy."""

    def test_even_spread_two_nodes(self):
        """4 vCPUs spread across 2 nodes = 2 per node."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_spread(4)

        self.assertEqual(len(mapping), 4)

        node_counts = {0: 0, 1: 0}
        for cpulist in mapping.values():
            for cpu in cpulist:
                node_id = topo.get_node_for_cpu(cpu)
                node_counts[node_id] += 1

        self.assertEqual(node_counts[0], 2)
        self.assertEqual(node_counts[1], 2)

    def test_spread_four_nodes(self):
        """8 vCPUs spread across 4 nodes = 2 per node."""
        topo = MockNumaTopology.four_node()
        engine = PinningEngine(topo)
        mapping = engine.compute_spread(8)

        self.assertEqual(len(mapping), 8)

        node_counts = {0: 0, 1: 0, 2: 0, 3: 0}
        for cpulist in mapping.values():
            for cpu in cpulist:
                node_id = topo.get_node_for_cpu(cpu)
                node_counts[node_id] += 1

        for nid in range(4):
            self.assertEqual(node_counts[nid], 2)

    def test_spread_uneven(self):
        """5 vCPUs across 2 nodes = 3+2 distribution."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_spread(5)

        self.assertEqual(len(mapping), 5)

        node_counts = {0: 0, 1: 0}
        for cpulist in mapping.values():
            for cpu in cpulist:
                node_id = topo.get_node_for_cpu(cpu)
                node_counts[node_id] += 1

        self.assertEqual(node_counts[0], 3)
        self.assertEqual(node_counts[1], 2)

    def test_spread_not_enough_cpus(self):
        """Should raise error when a node runs out of CPUs."""
        topo = MockNumaTopology.two_node_no_smt()
        used = set(range(0, 8))  # node 0 fully used
        engine = PinningEngine(topo, used_cpus=used)

        with self.assertRaises(RuntimeError):
            engine.compute_spread(4)


class TestPinningEngineCompact(unittest.TestCase):
    """Test compact pinning strategy."""

    def test_compact_with_smt(self):
        """2 vCPUs should use SMT siblings on the same core."""
        topo = MockNumaTopology.two_node_with_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_compact(2)

        self.assertEqual(len(mapping), 2)
        cpus = []
        for cpulist in mapping.values():
            cpus.extend(cpulist)

        # Both CPUs should be siblings
        self.assertEqual(len(cpus), 2)
        cpu0, cpu1 = cpus
        self.assertIn(cpu1, topo.core_siblings.get(cpu0, []),
                       "Both vCPUs should be on SMT siblings")

    def test_compact_more_than_smt(self):
        """4 vCPUs should use 2 physical cores."""
        topo = MockNumaTopology.two_node_with_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_compact(4)

        self.assertEqual(len(mapping), 4)

        # Count physical cores used
        cores_used = set()
        for cpulist in mapping.values():
            for cpu in cpulist:
                # Find the primary sibling (lowest CPU ID in group)
                siblings = topo.core_siblings.get(cpu, [cpu])
                cores_used.add(min(siblings))

        self.assertLessEqual(len(cores_used), 2,
                             "Should use at most 2 physical cores for 4 vCPUs with SMT")

    def test_compact_no_smt(self):
        """Without SMT, compact should still pack onto first available CPUs."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)
        mapping = engine.compute_compact(4)

        self.assertEqual(len(mapping), 4)
        cpus = sorted(
            cpu for cpulist in mapping.values() for cpu in cpulist
        )
        # Should use first 4 available CPUs
        self.assertEqual(cpus, [0, 1, 2, 3])

    def test_compact_not_enough_cpus(self):
        """Should raise error when not enough CPUs."""
        topo = MockNumaTopology.two_node_no_smt()
        used = set(range(0, 16))
        engine = PinningEngine(topo, used_cpus=used)

        with self.assertRaises(RuntimeError):
            engine.compute_compact(4)


class TestManualMapping(unittest.TestCase):
    """Test manual mapping parsing."""

    def test_basic_mapping(self):
        mapping = PinningEngine.parse_manual_mapping(
            "vcpu0:0-3,vcpu1:4-7", max_cpu=64
        )
        self.assertEqual(mapping["vcpu0"], [0, 1, 2, 3])
        self.assertEqual(mapping["vcpu1"], [4, 5, 6, 7])

    def test_single_cpu_mapping(self):
        mapping = PinningEngine.parse_manual_mapping(
            "vcpu0:5", max_cpu=64
        )
        self.assertEqual(mapping["vcpu0"], [5])

    def test_invalid_format(self):
        with self.assertRaises(ValueError):
            PinningEngine.parse_manual_mapping("vcpu0", max_cpu=64)

    def test_cpu_out_of_range(self):
        with self.assertRaises(ValueError) as ctx:
            PinningEngine.parse_manual_mapping(
                "vcpu0:256", max_cpu=64
            )
        self.assertIn("does not exist", str(ctx.exception))

    def test_multiple_vcpus(self):
        mapping = PinningEngine.parse_manual_mapping(
            "vcpu0:0,vcpu1:1,vcpu2:2,vcpu3:3", max_cpu=64
        )
        self.assertEqual(len(mapping), 4)
        for i in range(4):
            self.assertEqual(mapping[f"vcpu{i}"], [i])


class TestPinningConfig(unittest.TestCase):
    """Test persistent pinning configuration."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.config = PinningConfig(conf_dir=self.tmpdir)

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_save_and_load(self):
        pinning = {"vcpu0": [0, 1], "vcpu1": [2, 3]}
        self.config.save("test-vm", "numa-aware", pinning)

        loaded = self.config.load("test-vm")
        self.assertIsNotNone(loaded)
        self.assertEqual(loaded["domain"], "test-vm")
        self.assertEqual(loaded["strategy"], "numa-aware")
        self.assertEqual(loaded["pinning"]["vcpu0"], [0, 1])

    def test_load_nonexistent(self):
        loaded = self.config.load("nonexistent-vm")
        self.assertIsNone(loaded)

    def test_remove(self):
        self.config.save("test-vm", "manual", {"vcpu0": [0]})
        self.config.remove("test-vm")
        loaded = self.config.load("test-vm")
        self.assertIsNone(loaded)

    def test_remove_nonexistent(self):
        # Should not raise
        self.config.remove("nonexistent-vm")

    def test_list_all(self):
        self.config.save("vm1", "spread", {"vcpu0": [0]})
        self.config.save("vm2", "compact", {"vcpu0": [1]})
        domains = self.config.list_all()
        self.assertIn("vm1", domains)
        self.assertIn("vm2", domains)

    def test_list_all_empty(self):
        domains = self.config.list_all()
        self.assertEqual(domains, [])

    def test_config_file_format(self):
        """Verify the JSON config file is properly formatted."""
        pinning = {"vcpu0": [4], "vcpu1": [5]}
        self.config.save("format-test", "numa-aware", pinning)

        config_path = Path(self.tmpdir) / "format-test.json"
        with open(config_path) as f:
            data = json.load(f)

        self.assertIn("domain", data)
        self.assertIn("strategy", data)
        self.assertIn("pinning", data)
        self.assertEqual(data["domain"], "format-test")


class TestNumaTopologyMethods(unittest.TestCase):
    """Test NumaTopology utility methods."""

    def test_get_node_for_cpu(self):
        topo = MockNumaTopology.two_node_no_smt()
        self.assertEqual(topo.get_node_for_cpu(0), 0)
        self.assertEqual(topo.get_node_for_cpu(7), 0)
        self.assertEqual(topo.get_node_for_cpu(8), 1)
        self.assertEqual(topo.get_node_for_cpu(15), 1)
        self.assertEqual(topo.get_node_for_cpu(99), -1)

    def test_get_nearest_nodes(self):
        topo = MockNumaTopology.four_node()
        nearest = topo.get_nearest_nodes(0)
        self.assertEqual(nearest[0], 0)  # Self is nearest
        self.assertEqual(nearest[1], 1)  # Node 1 is next (dist 21)

    def test_is_smt_enabled_true(self):
        topo = MockNumaTopology.two_node_with_smt()
        self.assertTrue(topo.is_smt_enabled())

    def test_is_smt_enabled_false(self):
        topo = MockNumaTopology.two_node_no_smt()
        self.assertFalse(topo.is_smt_enabled())

    def test_to_dict(self):
        topo = MockNumaTopology.two_node_no_smt()
        d = topo.to_dict()
        self.assertEqual(len(d["nodes"]), 2)
        self.assertEqual(d["nodes"][0]["id"], 0)
        self.assertEqual(d["nodes"][0]["cpus"], list(range(8)))
        self.assertEqual(d["smt_enabled"], False)
        self.assertEqual(len(d["distances"]), 2)

    def test_print_table(self, ):
        """Ensure print_table runs without error."""
        topo = MockNumaTopology.two_node_no_smt()
        # Just verify it doesn't crash
        import io
        from contextlib import redirect_stdout
        f = io.StringIO()
        with redirect_stdout(f):
            topo.print_table()
        output = f.getvalue()
        self.assertIn("node0", output)
        self.assertIn("node1", output)
        self.assertIn("Distance Matrix", output)


class TestCLIParsing(unittest.TestCase):
    """Test CLI argument parsing and main entry point."""

    @patch("vm_bindcore.NumaTopology")
    def test_topology_command(self, mock_topo_cls):
        """Test topology subcommand dispatching."""
        mock_topo = MockNumaTopology.two_node_no_smt()
        mock_topo_cls.return_value = mock_topo

        with patch("sys.argv", ["vm-bindcore", "topology"]):
            # Should not raise
            import io
            from contextlib import redirect_stdout
            f = io.StringIO()
            with redirect_stdout(f):
                try:
                    main()
                except SystemExit:
                    pass

    def test_no_command(self):
        """No subcommand should print help and return 1."""
        with patch("sys.argv", ["vm-bindcore"]):
            import io
            from contextlib import redirect_stdout, redirect_stderr
            f_out = io.StringIO()
            f_err = io.StringIO()
            with redirect_stdout(f_out), redirect_stderr(f_err):
                ret = main()
            self.assertEqual(ret, 1)

    def test_pin_requires_vm(self):
        """pin without --vm should fail."""
        with patch("sys.argv", ["vm-bindcore", "pin"]):
            with self.assertRaises(SystemExit) as ctx:
                main()
            self.assertNotEqual(ctx.exception.code, 0)


class TestEdgeCases(unittest.TestCase):
    """Test edge cases and boundary conditions."""

    def test_single_node_numa_aware(self):
        """NUMA-aware on single node should work."""
        topo = NumaTopology.__new__(NumaTopology)
        topo.nodes = {0: list(range(8))}
        topo.distances = {0: {0: 10}}
        topo.core_siblings = {i: [i] for i in range(8)}

        engine = PinningEngine(topo)
        mapping = engine.compute_numa_aware(4)
        self.assertEqual(len(mapping), 4)

    def test_single_node_spread(self):
        """Spread on single node should still work (all on one node)."""
        topo = NumaTopology.__new__(NumaTopology)
        topo.nodes = {0: list(range(8))}
        topo.distances = {0: {0: 10}}
        topo.core_siblings = {i: [i] for i in range(8)}

        engine = PinningEngine(topo)
        mapping = engine.compute_spread(4)
        self.assertEqual(len(mapping), 4)

    def test_one_vcpu(self):
        """Pinning a single vCPU should work for all strategies."""
        topo = MockNumaTopology.two_node_no_smt()
        engine = PinningEngine(topo)

        for strategy in ["numa_aware", "spread", "compact"]:
            method = getattr(engine, f"compute_{strategy}")
            mapping = method(1)
            self.assertEqual(len(mapping), 1)

    def test_all_cpus_used_except_needed(self):
        """Exact number of free CPUs matches request."""
        topo = MockNumaTopology.two_node_no_smt()
        used = set(range(4, 16))  # Use all except 0-3
        engine = PinningEngine(topo, used_cpus=used)
        mapping = engine.compute_numa_aware(4)
        self.assertEqual(len(mapping), 4)

    def test_large_vcpu_count(self):
        """Test with large number of vCPUs across 4 NUMA nodes."""
        topo = MockNumaTopology.four_node()
        engine = PinningEngine(topo)
        mapping = engine.compute_spread(32)
        self.assertEqual(len(mapping), 32)


class TestConflictDetection(unittest.TestCase):
    """Test CPU conflict detection scenarios."""

    def test_used_cpus_excluded(self):
        """Pinning engine should not assign already-used CPUs."""
        topo = MockNumaTopology.two_node_no_smt()
        used = {0, 1, 2, 3}
        engine = PinningEngine(topo, used_cpus=used)
        mapping = engine.compute_numa_aware(4)

        assigned_cpus = set()
        for cpulist in mapping.values():
            assigned_cpus.update(cpulist)

        self.assertTrue(assigned_cpus.isdisjoint(used),
                        "Should not assign already-used CPUs")

    def test_partial_node_used(self):
        """Partial usage of a node should only use remaining CPUs."""
        topo = MockNumaTopology.two_node_no_smt()
        used = {0, 1}  # First 2 CPUs of node 0
        engine = PinningEngine(topo, used_cpus=used)
        mapping = engine.compute_numa_aware(4, preferred_node=0)

        assigned = set()
        for cpulist in mapping.values():
            assigned.update(cpulist)

        self.assertNotIn(0, assigned)
        self.assertNotIn(1, assigned)


if __name__ == "__main__":
    unittest.main()
