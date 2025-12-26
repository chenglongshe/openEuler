#!/usr/bin/env python3
"""
Generate a CSV list of driver source files guarded by CONFIG options that are
disabled (set to n) in arch/x86/configs/openeuler_defconfig.

Behavior:
1. Parse defconfig to find CONFIG_* entries marked as not set.
2. Walk drivers/ Makefiles, honoring line continuations, to find obj-$(CONFIG_*)
   assignments matching disabled configs.
3. Extract referenced .o entries; if a corresponding .c file exists, output the
   .c path, otherwise keep the .o suffix.
4. Print a single-column CSV with header '驱动文件路径' (Driver File Path),
   retained as required by the task.
"""

from __future__ import annotations

import os
import re
import sys
from typing import Iterable, List, Set

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
DEFCONFIG_PATH = os.path.join(REPO_ROOT, "arch", "x86", "configs", "openeuler_defconfig")
DRIVERS_ROOT = os.path.join(REPO_ROOT, "drivers")
NOT_SET_RE = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+)\s+is not set")
EXPLICIT_N_RE = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=n")
OBJ_ASSIGN_RE = re.compile(r"obj-\$\((CONFIG_[A-Za-z0-9_]+)\)\s*[:+]?=\s*(.*)")


def load_disabled_configs(defconfig_path: str) -> Set[str]:
    """Return CONFIG_* symbols marked as not set in defconfig."""
    disabled: Set[str] = set()

    with open(defconfig_path, "r", encoding="utf-8") as f:
        for line in f:
            if match := NOT_SET_RE.match(line):
                disabled.add(match.group(1))
            elif match := EXPLICIT_N_RE.match(line):
                disabled.add(match.group(1))
    return disabled


def normalize_make_lines(lines: Iterable[str]) -> List[str]:
    """Merge Makefile lines with continuation backslashes."""
    merged: List[str] = []
    buffer = ""
    for raw in lines:
        line = raw.rstrip("\n")
        if line.endswith("\\"):
            buffer += line[:-1].rstrip() + " "
            continue
        buffer += line
        merged.append(buffer)
        buffer = ""
    if buffer:
        merged.append(buffer)
    return merged


def iter_obj_entries(makefile_path: str, disabled_configs: Set[str]) -> Iterable[str]:
    """Yield object tokens referenced by disabled CONFIG entries in a Makefile."""
    with open(makefile_path, "r", encoding="utf-8") as f:
        for line in normalize_make_lines(f):
            stripped = line.split("#", 1)[0].strip()
            if not stripped:
                continue
            match = OBJ_ASSIGN_RE.match(stripped)
            if not match:
                continue
            config = match.group(1)
            if config not in disabled_configs:
                continue
            tail = match.group(2).strip()
            if not tail:
                continue
            for token in re.split(r"\s+", tail):
                token = token.strip()
                if not token or token.startswith("$(") or token.endswith("/"):
                    continue
                if token.endswith(".o"):
                    yield token


def resolve_source_path(makefile_dir: str, obj_token: str) -> str:
    """Return relative driver path for an object token."""
    obj_path = os.path.normpath(os.path.join(makefile_dir, obj_token))
    c_path = os.path.splitext(obj_path)[0] + ".c"
    if os.path.exists(c_path):
        return os.path.relpath(c_path, REPO_ROOT)
    return os.path.relpath(obj_path, REPO_ROOT)


def collect_disabled_driver_sources() -> List[str]:
    disabled_configs = load_disabled_configs(DEFCONFIG_PATH)
    results: Set[str] = set()
    for root, _, files in os.walk(DRIVERS_ROOT):
        for name in files:
            if name != "Makefile":
                continue
            makefile_path = os.path.join(root, name)
            makefile_dir = os.path.dirname(makefile_path)
            for token in iter_obj_entries(makefile_path, disabled_configs):
                results.add(resolve_source_path(makefile_dir, token))
    return sorted(results)


def main() -> int:
    if not os.path.exists(DEFCONFIG_PATH):
        sys.stderr.write(f"Defconfig not found: {DEFCONFIG_PATH}\n")
        return 1
    sources = collect_disabled_driver_sources()
    print("驱动文件路径")
    for src in sources:
        print(src.replace(os.sep, "/"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
