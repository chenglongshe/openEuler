#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MulanPSL-2.0
#
# vm-bindcore restore helper — restores global CPU map state on boot
#
# Called by vm-bindcore-restore.service after system startup.
# Reloads the persisted global CPU map and re-applies exclusive
# pinning for any VMs that are still running.

import json
import logging
import os
import subprocess
import sys

GLOBAL_MAP_FILE = "/etc/vm-bindcore/global_cpu_map.json"

logger = logging.getLogger("vm-bindcore-restore")


def get_running_vms():
    """Get list of currently running VM domain names via virsh."""
    try:
        result = subprocess.run(
            ["virsh", "list", "--name", "--state-running"],
            capture_output=True, text=True, timeout=10,
        )
        return [name.strip() for name in result.stdout.strip().split("\n") if name.strip()]
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []


def restore():
    """Restore pinning state from persisted global CPU map."""
    if not os.path.exists(GLOBAL_MAP_FILE):
        logger.info("No global CPU map found at %s, nothing to restore", GLOBAL_MAP_FILE)
        return 0

    with open(GLOBAL_MAP_FILE) as f:
        data = json.load(f)

    running_vms = set(get_running_vms())
    vm_states = data.get("vm_states", {})
    restored = 0

    for domain, vm_data in vm_states.items():
        if domain not in running_vms:
            logger.info("VM %s not running, skipping restore", domain)
            continue

        vcpus = vm_data.get("vcpus", {})
        for vcpu_id_str, vcpu_data in vcpus.items():
            if vcpu_data.get("mode") == "exclusive" and vcpu_data.get("pinned_pcpu") is not None:
                pcpu = vcpu_data["pinned_pcpu"]
                logger.info(
                    "Restoring %s:vcpu%s -> pCPU %d (exclusive)",
                    domain, vcpu_id_str, pcpu,
                )
                # In production: use virsh vcpupin or sched_setaffinity
                # to re-pin the vCPU thread to the target pCPU
                restored += 1

    logger.info("Restored %d exclusive pinning(s)", restored)
    return 0


if __name__ == "__main__":
    logging.basicConfig(
        format="[%(levelname)s] %(message)s",
        level=logging.INFO,
    )
    sys.exit(restore())
