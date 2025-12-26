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
from functools import lru_cache
from pathlib import Path
from typing import Iterable, List, Optional, Set


def find_repo_root(start: str) -> str:
    """Ascend from start to find the repository root that contains drivers/."""
    origin = os.path.abspath(start)
    path = origin
    target = os.path.join("arch", "x86", "configs", "openeuler_defconfig")
    while True:
        if (
            os.path.isfile(os.path.join(path, target))
            and os.path.isdir(os.path.join(path, "drivers"))
        ):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            raise FileNotFoundError(
                f"Repository root not found when searching upward from {origin}"
            )
        path = parent


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NOT_SET_RE = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set")
EXPLICIT_N_RE = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=n$")
OBJ_ASSIGN_RE = re.compile(r"obj-\$\((CONFIG_[A-Za-z0-9_]+)\)\s*[+:]?=\s*(.*)")
OBJECT_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_./+-]*\.o\b")
MACRO_SUB_RE = re.compile(r"\$\([^)]+\)")
MACRO_PLACEHOLDER = ""


@lru_cache(maxsize=1)
def get_paths() -> tuple[Optional[str], Optional[str], Optional[str], Optional[str]]:
    """Return repo_root, defconfig_path, drivers_root, error."""
    try:
        repo_root = find_repo_root(SCRIPT_DIR)
    except FileNotFoundError as exc:
        return None, None, None, str(exc)
    defconfig_path = os.path.join(repo_root, "arch", "x86", "configs", "openeuler_defconfig")
    drivers_root = os.path.join(repo_root, "drivers")
    return repo_root, defconfig_path, drivers_root, None


def load_disabled_configs(defconfig_path: str) -> Set[str]:
    """Return CONFIG_* symbols marked as not set in defconfig."""
    disabled: Set[str] = set()

    with open(defconfig_path, "r", encoding="utf-8") as f:
        for line in f:
            match = NOT_SET_RE.match(line)
            if match:
                disabled.add(match.group(1))
                continue
            match = EXPLICIT_N_RE.match(line)
            if match:
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


def strip_make_comment(line: str) -> str:
    """Remove Make-style comments, honoring simple escaping."""
    for idx, ch in enumerate(line):
        if ch == "#" and (idx == 0 or line[idx - 1] != "\\"):
            return line[:idx]
    return line


def extract_object_tokens(token: str) -> Iterable[str]:
    """Extract .o entries from a raw Makefile token, tolerating simple macros.

    Note: this only strips straightforward $(...) substitutions and it does not
    attempt to parse nested Make constructs.
    """
    stripped = token.strip()
    if not stripped:
        return []
    cleaned = MACRO_SUB_RE.sub(MACRO_PLACEHOLDER, stripped).strip("/")
    if not cleaned or cleaned.endswith("/"):
        return []
    return OBJECT_RE.findall(cleaned)


def iter_obj_entries(makefile_path: str, disabled_configs: Set[str]) -> Iterable[str]:
    """Yield object tokens referenced by disabled CONFIG entries in a Makefile."""
    with open(makefile_path, "r", encoding="utf-8") as f:
        for line in normalize_make_lines(f):
            stripped = strip_make_comment(line).strip()
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
                for obj in extract_object_tokens(token):
                    yield obj


def resolve_source_path(
    makefile_dir: str, obj_token: str, repo_root: str, drivers_root: str
) -> Optional[str]:
    """Return relative driver path for an object token."""
    obj_path = (Path(makefile_dir) / obj_token).resolve()
    drivers_root_abs = Path(drivers_root).resolve()
    try:
        obj_path.relative_to(drivers_root_abs)
    except ValueError:
        return None
    c_path = obj_path.with_suffix(".c")
    if not c_path.exists():
        return None
    target = c_path
    try:
        return target.relative_to(Path(repo_root).resolve()).as_posix()
    except ValueError:
        return None


def collect_disabled_driver_sources() -> List[str]:
    repo_root, defconfig_path, drivers_root, error = get_paths()
    if error:
        sys.stderr.write(f"{error}\n")
        return []
    if not defconfig_path:
        sys.stderr.write("Defconfig path could not be determined; cannot collect sources.\n")
        return []
    if not drivers_root:
        sys.stderr.write("Drivers directory could not be determined; cannot collect sources.\n")
        return []
    disabled_configs = load_disabled_configs(defconfig_path)
    results: Set[str] = set()
    for root, _, files in os.walk(drivers_root):
        for name in files:
            if name != "Makefile":
                continue
            makefile_path = os.path.join(root, name)
            makefile_dir = os.path.dirname(makefile_path)
            for token in iter_obj_entries(makefile_path, disabled_configs):
                resolved = resolve_source_path(makefile_dir, token, repo_root, drivers_root)
                if resolved:
                    results.add(resolved)
    return sorted(results)


def main() -> int:
    repo_root, defconfig_path, drivers_root, error = get_paths()
    if error or repo_root is None:
        sys.stderr.write(f"{error or 'Repository root not found.'}\n")
        return 1
    if not defconfig_path or not os.path.exists(defconfig_path):
        sys.stderr.write(f"Defconfig not found: {defconfig_path}\n")
        return 1
    if not drivers_root or not os.path.isdir(drivers_root):
        sys.stderr.write(f"Drivers directory not found: {drivers_root}\n")
        return 1
    sources = collect_disabled_driver_sources()
    print("驱动文件路径")
    for src in sources:
        print(src)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
