#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Emit only the target column from disabled_driver_configs.py output,
omitting any rows that end with '.o'.
"""

import csv
import sys
from pathlib import Path

import disabled_driver_configs as base


def _load_targets(
    defconfig: Path, drivers_root: Path
) -> list[str]:
    disabled = base._load_disabled_configs(defconfig)
    rows = base._find_disabled_targets(disabled, drivers_root)
    targets: list[str] = []
    seen: set[str] = set()
    for _, _, target in rows:
        if target.endswith(".o"):
            continue
        if target in seen:
            continue
        seen.add(target)
        targets.append(target)
    return targets


def main(argv: list[str]) -> int:
    defconfig = Path(argv[1]) if len(argv) > 1 else base.DEFCONFIG_PATH
    drivers_root = Path(argv[2]) if len(argv) > 2 else base.DRIVERS_ROOT

    if not defconfig.is_file():
        sys.stderr.write(f"defconfig not found: {defconfig}\n")
        return 1
    if not drivers_root.is_dir():
        sys.stderr.write(f"drivers directory not found: {drivers_root}\n")
        return 1

    targets = _load_targets(defconfig, drivers_root)

    writer = csv.writer(sys.stdout)
    writer.writerow(["target"])
    try:
        for target in targets:
            writer.writerow([target])
    except (BrokenPipeError, OSError):
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
