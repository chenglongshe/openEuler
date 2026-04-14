#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vm-bindcore: Virtual Machine vCPU CPU-Pinning Management Tool
# Based on patent: 一种虚拟机绑核方法及计算设备 (Inventor: 张海亮)
#
# Copyright (c) 2026 openEuler Contributors

"""
vm-bindcore - A NUMA-aware vCPU pinning management tool for KVM/libvirt VMs.

Provides automatic and manual vCPU-to-pCPU pinning with the following strategies:
  - numa-aware: Pin all vCPUs to the same NUMA node
  - spread:     Distribute vCPUs evenly across NUMA nodes
  - compact:    Pack vCPUs onto fewest physical cores (SMT-aware)
"""

import argparse
import json
import logging
import os
import re
import sys
from pathlib import Path

CONF_DIR = "/etc/vm-bindcore/pinning.d"
LOG_FORMAT = "[%(levelname)s] %(message)s"

logger = logging.getLogger("vm-bindcore")


class NumaTopology:
    """Detects and represents the host NUMA topology."""

    def __init__(self):
        self.nodes = {}       # {node_id: [cpu_list]}
        self.distances = {}   # {node_id: {node_id: distance}}
        self.core_siblings = {}  # {cpu_id: [sibling_cpu_ids]}
        self._detect()

    def _detect(self):
        """Detect NUMA topology from sysfs."""
        numa_base = Path("/sys/devices/system/node")
        if not numa_base.exists():
            # Fallback: single NUMA node with all online CPUs
            cpus = self._get_online_cpus()
            self.nodes[0] = cpus
            self.distances[0] = {0: 10}
            return

        for node_dir in sorted(numa_base.iterdir()):
            match = re.match(r"node(\d+)", node_dir.name)
            if not match:
                continue
            node_id = int(match.group(1))
            cpulist_path = node_dir / "cpulist"
            if cpulist_path.exists():
                cpulist_str = cpulist_path.read_text().strip()
                self.nodes[node_id] = self._parse_cpulist(cpulist_str)

            distance_path = node_dir / "distance"
            if distance_path.exists():
                dists = distance_path.read_text().strip().split()
                self.distances[node_id] = {}
                for i, d in enumerate(dists):
                    self.distances[node_id][i] = int(d)

        self._detect_smt_siblings()

    def _detect_smt_siblings(self):
        """Detect SMT (Hyper-Threading) sibling relationships."""
        for node_cpus in self.nodes.values():
            for cpu in node_cpus:
                sibling_path = Path(
                    f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list"
                )
                if sibling_path.exists():
                    siblings_str = sibling_path.read_text().strip()
                    self.core_siblings[cpu] = self._parse_cpulist(siblings_str)

    @staticmethod
    def _get_online_cpus():
        """Get list of online CPUs."""
        path = Path("/sys/devices/system/cpu/online")
        if path.exists():
            return NumaTopology._parse_cpulist(path.read_text().strip())
        # Fallback: use os.cpu_count()
        return list(range(os.cpu_count() or 1))

    @staticmethod
    def _parse_cpulist(cpulist_str):
        """Parse a CPU list string like '0-3,8-11' into a list of ints."""
        cpus = []
        for part in cpulist_str.split(","):
            part = part.strip()
            if "-" in part:
                start, end = part.split("-", 1)
                cpus.extend(range(int(start), int(end) + 1))
            else:
                cpus.append(int(part))
        return sorted(cpus)

    def get_node_for_cpu(self, cpu):
        """Return the NUMA node ID for a given CPU."""
        for node_id, cpus in self.nodes.items():
            if cpu in cpus:
                return node_id
        return -1

    def get_nearest_nodes(self, node_id):
        """Return nodes sorted by distance from the given node."""
        if node_id not in self.distances:
            return list(self.nodes.keys())
        dists = self.distances[node_id]
        return sorted(dists.keys(), key=lambda n: dists[n])

    def is_smt_enabled(self):
        """Check if SMT (Hyper-Threading) is enabled."""
        for siblings in self.core_siblings.values():
            if len(siblings) > 1:
                return True
        return False

    def to_dict(self):
        """Convert topology to a serializable dictionary."""
        return {
            "nodes": [
                {"id": nid, "cpus": cpus, "online": True}
                for nid, cpus in sorted(self.nodes.items())
            ],
            "distances": [
                [self.distances.get(i, {}).get(j, 0)
                 for j in sorted(self.nodes.keys())]
                for i in sorted(self.nodes.keys())
            ],
            "smt_enabled": self.is_smt_enabled(),
        }

    def print_table(self):
        """Print topology as a human-readable table."""
        print("NUMA Node   CPUs                          Online")
        print("---------   ----                          ------")
        for nid in sorted(self.nodes.keys()):
            cpus = self.nodes[nid]
            if cpus:
                cpu_str = f"{cpus[0]}-{cpus[-1]}" if len(cpus) > 1 else str(cpus[0])
            else:
                cpu_str = "(none)"
            print(f"node{nid:<6}  {cpu_str:<30}yes")

        if self.distances:
            print()
            print("Distance Matrix:")
            header = "        " + "  ".join(
                f"node{n}" for n in sorted(self.nodes.keys())
            )
            print(header)
            for i in sorted(self.nodes.keys()):
                row = f"node{i}   "
                row += "  ".join(
                    f"{self.distances.get(i, {}).get(j, 0):>5}"
                    for j in sorted(self.nodes.keys())
                )
                print(row)


class LibvirtConnector:
    """Interface to libvirt for vCPU pinning operations."""

    def __init__(self):
        self._conn = None

    def _connect(self):
        """Establish libvirt connection."""
        if self._conn is not None:
            return
        try:
            import libvirt  # noqa: F811
            self._conn = libvirt.open("qemu:///system")
            if self._conn is None:
                raise RuntimeError("Failed to connect to qemu:///system")
        except ImportError:
            raise RuntimeError(
                "libvirt Python bindings not found. "
                "Install with: yum install python3-libvirt"
            )

    def get_domain(self, name):
        """Look up a domain by name."""
        self._connect()
        try:
            return self._conn.lookupByName(name)
        except Exception:
            raise RuntimeError(f"Domain '{name}' not found or not running")

    def get_vcpu_count(self, domain):
        """Get the number of vCPUs for a domain."""
        info = domain.info()
        return info[3]  # nrVirtCpu

    def pin_vcpu(self, domain, vcpu_id, cpulist, max_cpus=None):
        """Pin a vCPU to specified physical CPUs."""
        if max_cpus is None:
            max_cpus = os.cpu_count() or 64
        # Build cpumap tuple
        maplen = (max_cpus + 7) // 8
        cpumap = bytearray(maplen)
        for cpu in cpulist:
            byte_idx = cpu // 8
            bit_idx = cpu % 8
            if byte_idx < maplen:
                cpumap[byte_idx] |= 1 << bit_idx
        domain.pinVcpu(vcpu_id, tuple(cpumap))

    def unpin_vcpu(self, domain, vcpu_id, max_cpus=None):
        """Unpin a vCPU (allow all CPUs)."""
        if max_cpus is None:
            max_cpus = os.cpu_count() or 64
        maplen = (max_cpus + 7) // 8
        cpumap = tuple([0xFF] * maplen)
        domain.pinVcpu(vcpu_id, cpumap)

    def get_vcpu_info(self, domain):
        """Get current vCPU pinning info."""
        vcpus = domain.vcpus()
        if vcpus is None:
            return []
        info_list = []
        for i, (vcpu_info, cpumap) in enumerate(zip(vcpus[0], vcpus[1])):
            pinned_cpus = []
            for byte_idx, byte_val in enumerate(cpumap):
                for bit_idx in range(8):
                    if byte_val & (1 << bit_idx):
                        pinned_cpus.append(byte_idx * 8 + bit_idx)
            info_list.append({
                "vcpu": i,
                "state": vcpu_info[1],
                "cpu": vcpu_info[3],
                "pinned_cpus": pinned_cpus,
            })
        return info_list

    def close(self):
        """Close libvirt connection."""
        if self._conn is not None:
            self._conn.close()
            self._conn = None


class PinningConfig:
    """Manages persistent pinning configurations."""

    def __init__(self, conf_dir=CONF_DIR):
        self.conf_dir = Path(conf_dir)

    def save(self, domain_name, strategy, pinning_map):
        """Save pinning configuration to file."""
        self.conf_dir.mkdir(parents=True, exist_ok=True)
        config = {
            "domain": domain_name,
            "strategy": strategy,
            "pinning": pinning_map,
        }
        config_path = self.conf_dir / f"{domain_name}.json"
        with open(config_path, "w") as f:
            json.dump(config, f, indent=2)
        logger.info("Saved pinning config to %s", config_path)

    def load(self, domain_name):
        """Load pinning configuration from file."""
        config_path = self.conf_dir / f"{domain_name}.json"
        if not config_path.exists():
            return None
        with open(config_path) as f:
            return json.load(f)

    def remove(self, domain_name):
        """Remove pinning configuration file."""
        config_path = self.conf_dir / f"{domain_name}.json"
        if config_path.exists():
            config_path.unlink()
            logger.info("Removed persistent config for %s", domain_name)

    def list_all(self):
        """List all saved domain configurations."""
        if not self.conf_dir.exists():
            return []
        return [
            p.stem for p in self.conf_dir.glob("*.json")
        ]


class PinningEngine:
    """Computes vCPU-to-pCPU mappings based on selected strategy."""

    def __init__(self, topology, used_cpus=None):
        self.topo = topology
        self.used_cpus = used_cpus or set()

    def _get_free_cpus(self, node_id):
        """Get free CPUs on a NUMA node."""
        return [c for c in self.topo.nodes.get(node_id, [])
                if c not in self.used_cpus]

    def compute_numa_aware(self, vcpu_count, preferred_node=None):
        """
        Compute NUMA-aware pinning: place all vCPUs on the same NUMA node.
        If the preferred node lacks capacity, spill to nearest neighbor.
        """
        if preferred_node is not None:
            node_order = [preferred_node] + [
                n for n in self.topo.get_nearest_nodes(preferred_node)
                if n != preferred_node
            ]
        else:
            # Pick node with most free CPUs
            node_order = sorted(
                self.topo.nodes.keys(),
                key=lambda n: len(self._get_free_cpus(n)),
                reverse=True,
            )

        mapping = {}
        remaining = vcpu_count
        vcpu_idx = 0

        for node_id in node_order:
            if remaining <= 0:
                break
            free = self._get_free_cpus(node_id)
            allocate = min(remaining, len(free))
            for i in range(allocate):
                mapping[f"vcpu{vcpu_idx}"] = [free[i]]
                vcpu_idx += 1
                remaining -= 1

        if remaining > 0:
            raise RuntimeError(
                f"Not enough free CPUs: need {vcpu_count}, "
                f"only {vcpu_count - remaining} available"
            )
        return mapping

    def compute_spread(self, vcpu_count):
        """
        Compute spread pinning: distribute vCPUs evenly across NUMA nodes.
        """
        node_ids = sorted(self.topo.nodes.keys())
        if not node_ids:
            raise RuntimeError("No NUMA nodes detected")

        node_free = {n: self._get_free_cpus(n) for n in node_ids}
        mapping = {}

        for vcpu_idx in range(vcpu_count):
            target_node = node_ids[vcpu_idx % len(node_ids)]
            free = node_free[target_node]
            if not free:
                raise RuntimeError(
                    f"No free CPUs on node{target_node} for vcpu{vcpu_idx}"
                )
            cpu = free.pop(0)
            mapping[f"vcpu{vcpu_idx}"] = [cpu]

        return mapping

    def compute_compact(self, vcpu_count):
        """
        Compute compact pinning: pack vCPUs onto fewest physical cores,
        preferring SMT siblings on the same core.
        """
        # Group CPUs by physical core using SMT siblings
        core_groups = []
        visited = set()

        for cpu in sorted(self.topo.core_siblings.keys()):
            if cpu in visited:
                continue
            siblings = self.topo.core_siblings.get(cpu, [cpu])
            free_siblings = [c for c in siblings
                             if c not in self.used_cpus and c not in visited]
            if free_siblings:
                core_groups.append(free_siblings)
                visited.update(siblings)

        # If no SMT info, fall back to flat CPU list
        if not core_groups:
            all_free = []
            for cpus in self.topo.nodes.values():
                all_free.extend(c for c in cpus if c not in self.used_cpus)
            core_groups = [[c] for c in sorted(all_free)]

        mapping = {}
        vcpu_idx = 0

        for group in core_groups:
            if vcpu_idx >= vcpu_count:
                break
            for cpu in group:
                if vcpu_idx >= vcpu_count:
                    break
                mapping[f"vcpu{vcpu_idx}"] = [cpu]
                vcpu_idx += 1

        if vcpu_idx < vcpu_count:
            raise RuntimeError(
                f"Not enough free CPUs: need {vcpu_count}, "
                f"only {vcpu_idx} available"
            )
        return mapping

    @staticmethod
    def parse_manual_mapping(mapping_str, max_cpu):
        """
        Parse manual mapping string like 'vcpu0:0-3,vcpu1:4-7'.
        """
        mapping = {}
        for entry in mapping_str.split(","):
            entry = entry.strip()
            if ":" not in entry:
                raise ValueError(f"Invalid mapping format: '{entry}'")
            vcpu_part, cpu_part = entry.split(":", 1)
            vcpu_part = vcpu_part.strip()
            cpu_part = cpu_part.strip()

            cpus = NumaTopology._parse_cpulist(cpu_part)
            for c in cpus:
                if c >= max_cpu:
                    raise ValueError(
                        f"pCPU {c} does not exist on this host "
                        f"(max: {max_cpu - 1})"
                    )
            mapping[vcpu_part] = cpus
        return mapping


def cmd_topology(args):
    """Handle the 'topology' subcommand."""
    topo = NumaTopology()
    if args.format == "json":
        print(json.dumps(topo.to_dict(), indent=2))
    else:
        topo.print_table()


def cmd_pin(args):
    """Handle the 'pin' subcommand."""
    topo = NumaTopology()
    lv = LibvirtConnector()

    try:
        domain = lv.get_domain(args.vm)
        vcpu_count = lv.get_vcpu_count(domain)
        max_cpus = os.cpu_count() or 64

        if args.manual:
            mapping = PinningEngine.parse_manual_mapping(args.manual, max_cpus)
            strategy = "manual"
        else:
            strategy = args.strategy or "numa-aware"
            engine = PinningEngine(topo)

            if strategy == "numa-aware":
                node = getattr(args, "node", None)
                mapping = engine.compute_numa_aware(vcpu_count, node)
            elif strategy == "spread":
                mapping = engine.compute_spread(vcpu_count)
            elif strategy == "compact":
                mapping = engine.compute_compact(vcpu_count)
            else:
                print(f"[ERROR] Unknown strategy: {strategy}", file=sys.stderr)
                return 1

        # Execute pinning
        for vcpu_name, cpulist in sorted(mapping.items()):
            vcpu_id = int(re.search(r"\d+", vcpu_name).group())
            lv.pin_vcpu(domain, vcpu_id, cpulist, max_cpus)
            node_id = topo.get_node_for_cpu(cpulist[0])
            cpu_str = ",".join(str(c) for c in cpulist)
            logger.info("Pinned %s -> pCPU %s (node%d)", vcpu_name, cpu_str, node_id)

        # Persist
        config = PinningConfig()
        pinning_serializable = {k: v for k, v in mapping.items()}
        config.save(args.vm, strategy, pinning_serializable)

        logger.info("Successfully pinned %d vCPUs for %s", len(mapping), args.vm)
        print(f"[OK] Successfully pinned {len(mapping)} vCPUs for {args.vm}")

    finally:
        lv.close()


def cmd_unpin(args):
    """Handle the 'unpin' subcommand."""
    lv = LibvirtConnector()

    try:
        domain = lv.get_domain(args.vm)
        vcpu_count = lv.get_vcpu_count(domain)

        for vcpu_id in range(vcpu_count):
            lv.unpin_vcpu(domain, vcpu_id)
            logger.info("Unpinned vcpu%d", vcpu_id)

        config = PinningConfig()
        config.remove(args.vm)

        logger.info("Successfully unpinned %d vCPUs for %s", vcpu_count, args.vm)
        print(f"[OK] Successfully unpinned {vcpu_count} vCPUs for {args.vm}")

    finally:
        lv.close()


def cmd_show(args):
    """Handle the 'show' subcommand."""
    lv = LibvirtConnector()
    topo = NumaTopology()

    try:
        domain = lv.get_domain(args.vm)
        vcpu_info = lv.get_vcpu_info(domain)

        config = PinningConfig()
        saved = config.load(args.vm)
        strategy = saved.get("strategy", "unknown") if saved else "none"

        if args.format == "json":
            output = {
                "domain": args.vm,
                "vcpu_count": len(vcpu_info),
                "strategy": strategy,
                "vcpus": [],
            }
            for vi in vcpu_info:
                node = topo.get_node_for_cpu(vi["cpu"]) if vi["cpu"] >= 0 else -1
                output["vcpus"].append({
                    "vcpu": vi["vcpu"],
                    "current_cpu": vi["cpu"],
                    "pinned_cpus": vi["pinned_cpus"],
                    "numa_node": node,
                })
            print(json.dumps(output, indent=2))
        else:
            max_cpus = os.cpu_count() or 64
            all_cpus = set(range(max_cpus))

            print(f"VM: {args.vm} ({len(vcpu_info)} vCPUs)")
            print(f"{'vCPU':<8}{'Pinned pCPUs':<24}{'NUMA Node'}")
            print(f"{'----':<8}{'------------':<24}{'---------'}")

            has_pinning = False
            for vi in vcpu_info:
                pinned = vi["pinned_cpus"]
                if set(pinned) != all_cpus:
                    has_pinning = True
                    cpu_str = _format_cpulist(pinned)
                    node = topo.get_node_for_cpu(pinned[0]) if pinned else -1
                    node_str = f"node{node}" if node >= 0 else "N/A"
                else:
                    cpu_str = "(all)"
                    node_str = "N/A"
                print(f"vcpu{vi['vcpu']:<4}{cpu_str:<24}{node_str}")

            if not has_pinning:
                print(f"\nNo pinning configured for {args.vm} "
                      "(using system default scheduling)")
            else:
                print(f"Strategy: {strategy}")

    finally:
        lv.close()


def cmd_restore(args):
    """Handle the 'restore' subcommand."""
    config = PinningConfig()

    if args.vm:
        domains = [args.vm]
    else:
        domains = config.list_all()

    if not domains:
        print("[INFO] No saved pinning configurations found")
        return

    lv = LibvirtConnector()
    topo = NumaTopology()

    try:
        for domain_name in domains:
            saved = config.load(domain_name)
            if not saved:
                continue

            try:
                domain = lv.get_domain(domain_name)
            except RuntimeError:
                logger.warning("Domain %s not found, skipping", domain_name)
                continue

            max_cpus = os.cpu_count() or 64
            pinning = saved.get("pinning", {})

            for vcpu_name, cpulist in pinning.items():
                vcpu_id = int(re.search(r"\d+", vcpu_name).group())
                lv.pin_vcpu(domain, vcpu_id, cpulist, max_cpus)

            logger.info(
                "Restored pinning for %s (%d vCPUs)",
                domain_name, len(pinning)
            )
            print(f"[OK] Restored pinning for {domain_name}")

    finally:
        lv.close()


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


def main():
    """Main entry point."""
    parser = argparse.ArgumentParser(
        prog="vm-bindcore",
        description="VM vCPU CPU-Pinning Management Tool (NUMA-aware)",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Enable verbose output"
    )
    subparsers = parser.add_subparsers(dest="command", help="Sub-command")

    # topology
    p_topo = subparsers.add_parser("topology", help="Show host NUMA topology")
    p_topo.add_argument(
        "--format", choices=["table", "json"], default="table",
        help="Output format (default: table)"
    )

    # pin
    p_pin = subparsers.add_parser("pin", help="Pin vCPUs to physical CPUs")
    p_pin.add_argument("--vm", required=True, help="VM domain name")
    p_pin.add_argument(
        "--strategy", choices=["numa-aware", "spread", "compact"],
        help="Pinning strategy"
    )
    p_pin.add_argument("--node", type=int, help="Preferred NUMA node (numa-aware)")
    p_pin.add_argument(
        "--manual", help="Manual mapping (e.g., vcpu0:0-3,vcpu1:4-7)"
    )
    p_pin.add_argument(
        "--exclusive", action="store_true",
        help="Exclusive mode: reject if target CPUs are already pinned"
    )

    # unpin
    p_unpin = subparsers.add_parser("unpin", help="Remove vCPU pinning")
    p_unpin.add_argument("--vm", required=True, help="VM domain name")

    # show
    p_show = subparsers.add_parser("show", help="Show current vCPU pinning")
    p_show.add_argument("--vm", required=True, help="VM domain name")
    p_show.add_argument(
        "--format", choices=["table", "json"], default="table",
        help="Output format (default: table)"
    )

    # restore
    p_restore = subparsers.add_parser(
        "restore", help="Restore pinning from saved configs"
    )
    p_restore.add_argument("--vm", help="VM domain name (all if omitted)")

    args = parser.parse_args()

    # Setup logging
    level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(format=LOG_FORMAT, level=level)
    logging.getLogger("vm-bindcore").setLevel(level)

    if not args.command:
        parser.print_help()
        return 1

    commands = {
        "topology": cmd_topology,
        "pin": cmd_pin,
        "unpin": cmd_unpin,
        "show": cmd_show,
        "restore": cmd_restore,
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
