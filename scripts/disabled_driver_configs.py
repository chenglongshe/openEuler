#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""
Extract driver targets guarded by configs set to 'n' in
arch/x86/configs/openeuler_defconfig and emit them as CSV.

Each row contains: CONFIG symbol, Makefile path (relative to repo root),
and the target listed on the matching Makefile line.
"""

import csv
import re
import sys
from pathlib import Path


DEFCONFIG_PATH = Path("arch/x86/configs/openeuler_defconfig")
REPO_ROOT = Path(__file__).resolve().parent.parent
DRIVERS_ROOT = REPO_ROOT / "drivers"

_OBJ_RE = re.compile(
    r"""^\s*obj-\$\((CONFIG_[A-Za-z0-9_]+)\)\s*
         [+:?]?=\s*(.+)$""",
    re.VERBOSE,
)
_NOT_SET_RE = re.compile(r"^#\s+(CONFIG_[A-Za-z0-9_]+)\s+is\s+not\s+set")
_SET_N_RE = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=n")


def _load_disabled_configs(defconfig: Path) -> set[str]:
    disabled: set[str] = set()
    for line in defconfig.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = line.strip()
        match = _NOT_SET_RE.match(line)
        if match:
            disabled.add(match.group(1))
            continue
        match = _SET_N_RE.match(line)
        if match:
            disabled.add(match.group(1))
    return disabled


def _joined_lines(content: str) -> list[str]:
    """Join Makefile lines that end with a backslash."""
    joined: list[str] = []
    buffer = ""
    for raw in content.splitlines():
        line = raw.rstrip()
        if buffer:
            buffer += line
        else:
            buffer = line
        if buffer.endswith("\\"):
            buffer = buffer[:-1] + " "
            continue
        joined.append(buffer)
        buffer = ""
    if buffer:
        joined.append(buffer)
    return joined


def _targets_from_line(line: str) -> tuple[str, list[str]] | None:
    match = _OBJ_RE.match(line)
    if not match:
        return None
    cfg = match.group(1)
    rhs = match.group(2).split("#", 1)[0].strip()
    if not rhs:
        return cfg, []
    tokens = [token for token in re.split(r"\s+", rhs) if token]
    return cfg, tokens


def _find_disabled_targets(
    disabled: set[str], drivers_root: Path
) -> list[tuple[str, Path, str]]:
    seen: set[tuple[str, Path, str]] = set()
    results: list[tuple[str, Path, str]] = []
    for makefile in sorted(drivers_root.rglob("Makefile")):
        content = makefile.read_text(encoding="utf-8", errors="ignore")
        for line in _joined_lines(content):
            parsed = _targets_from_line(line)
            if not parsed:
                continue
            cfg, targets = parsed
            if cfg not in disabled:
                continue
            for target in targets:
                entries: list[tuple[str, Path, str]] = []
                if target.endswith("/"):
                    dir_path = (makefile.parent / target).resolve()
                    try:
                        dir_path.relative_to(REPO_ROOT)
                    except ValueError:
                        dir_path = None

                    if dir_path and dir_path.is_dir():
                        for file_path in sorted(dir_path.rglob("*")):
                            if not file_path.is_file():
                                continue
                            rel = file_path.relative_to(REPO_ROOT).as_posix()
                            entries.append((cfg, makefile, rel))
                    else:
                        entries.append((cfg, makefile, target))
                else:
                    entries.append((cfg, makefile, target))

                for entry in entries:
                    seen_key = (entry[0], entry[1], entry[2])
                    if seen_key in seen:
                        continue
                    seen.add(seen_key)
                    results.append(entry)
    return results


def main(argv: list[str]) -> int:
    defconfig = Path(argv[1]) if len(argv) > 1 else DEFCONFIG_PATH
    drivers_root = Path(argv[2]) if len(argv) > 2 else DRIVERS_ROOT

    if not defconfig.is_file():
        sys.stderr.write(f"defconfig not found: {defconfig}\n")
        return 1
    if not drivers_root.is_dir():
        sys.stderr.write(f"drivers directory not found: {drivers_root}\n")
        return 1

    disabled = _load_disabled_configs(defconfig)
    results = _find_disabled_targets(disabled, drivers_root)

    writer = csv.writer(sys.stdout)
    writer.writerow(["config", "makefile", "target"])
    try:
        for cfg, makefile, target in results:
            writer.writerow([cfg, makefile.as_posix(), target])
    except (BrokenPipeError, OSError):
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
