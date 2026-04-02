#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Remove Makefile entries guarded by configs set to 'n' in
arch/x86/configs/openeuler_defconfig.

For every drivers/**/Makefile:
  - Lines starting with obj-$(CONFIG_*) where CONFIG_* is disabled are removed.
  - Any immediately following continuation lines (ending with '\') are also removed.
Outputs a list of modified Makefiles on stdout.
"""

from __future__ import annotations

import sys
import re
from pathlib import Path

import disabled_driver_configs as base


_OBJ_LINE_RE = re.compile(r"^\s*obj-\$\((CONFIG_[A-Za-z0-9_]+)\)")


def _strip_disabled(makefile: Path, disabled: set[str]) -> bool:
    lines = makefile.read_text(encoding="utf-8", errors="ignore").splitlines(keepends=True)
    new_lines: list[str] = []
    i = 0
    changed = False
    while i < len(lines):
        line = lines[i]
        match = _OBJ_LINE_RE.match(line)
        if match and match.group(1) in disabled:
            changed = True
            i += 1
            # skip continuation lines following the matched line
            while i < len(lines) and lines[i].rstrip().endswith("\\"):
                i += 1
            continue
        new_lines.append(line)
        i += 1

    if changed:
        makefile.write_text("".join(new_lines), encoding="utf-8")
    return changed


def main(argv: list[str]) -> int:
    defconfig = Path(argv[1]) if len(argv) > 1 else base.DEFCONFIG_PATH
    drivers_root = Path(argv[2]) if len(argv) > 2 else base.DRIVERS_ROOT

    if not defconfig.is_file():
        sys.stderr.write(f"defconfig not found: {defconfig}\n")
        return 1
    if not drivers_root.is_dir():
        sys.stderr.write(f"drivers directory not found: {drivers_root}\n")
        return 1

    disabled = base._load_disabled_configs(defconfig)
    modified = []
    for makefile in sorted(drivers_root.rglob("Makefile")):
        if _strip_disabled(makefile, disabled):
            modified.append(makefile)

    for path in modified:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
