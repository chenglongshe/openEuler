#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Generate a CSV mapping of disabled CONFIG options in
arch/x86/configs/openeuler_defconfig to driver targets declared in
drivers/*/Makefile files.
"""

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Iterable, Iterator, List, Set, Tuple


NOT_SET_RE = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+)\s+is\s+not\s+set\s*$")
# Matches conditional Makefile assignments such as:
# obj-$(CONFIG_FOO) += driver.o
ASSIGN_RE = re.compile(
    r"(?:(?P<prefix>[-+A-Za-z0-9_./]+)-)?\$\((?P<config>CONFIG_[A-Za-z0-9_]+)\)"
    r"\s*(?P<operator>[:+?]?=)\s*(?P<rhs>.+)"
)
SOURCE_SUFFIXES = (".c", ".S", ".s")
IGNORED_TOKEN_PREFIXES = ("#", "$", "-")
TARGET_TOKEN_HINTS = (".o", ".ko", "/")


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def collect_disabled_configs(defconfig: Path) -> Set[str]:
    disabled: Set[str] = set()
    for line in defconfig.read_text(encoding="utf-8").splitlines():
        match = NOT_SET_RE.match(line.strip())
        if match:
            disabled.add(match.group(1))
    return disabled


def collapsed_lines(lines: Iterable[str]) -> Iterator[str]:
    buffer = ""
    for raw in lines:
        line = raw.rstrip()
        if line.endswith("\\"):
            buffer += line[:-1]
            continue
        if buffer:
            line = buffer + line
            buffer = ""
        if line:
            yield line
    if buffer:
        yield buffer


def looks_like_target(token: str) -> bool:
    if not token or token.startswith(IGNORED_TOKEN_PREFIXES):
        return False
    return any(token.endswith(hint) for hint in TARGET_TOKEN_HINTS) or "/" in token


def normalize_target(token: str, base: Path, root: Path) -> str:
    raw_path = base / token
    resolved_path = raw_path.resolve()
    try:
        resolved_path.relative_to(root)
        target_path = resolved_path
    except ValueError:
        # If resolution escapes the repository root (for example via ".."),
        # fall back to the unresolved path to avoid emitting unexpected locations.
        target_path = raw_path
    if token.endswith(".o"):
        for suffix in SOURCE_SUFFIXES:
            candidate = target_path.with_suffix(suffix)
            if candidate.exists():
                target_path = candidate
                break
    try:
        return str(target_path.relative_to(root))
    except ValueError:
        return str(target_path)


def scan_makefile(
    makefile: Path, disabled: Set[str], root: Path
) -> List[Tuple[str, str, str]]:
    entries: List[Tuple[str, str, str]] = []
    with makefile.open(encoding="utf-8") as handle:
        for line in collapsed_lines(handle):
            for match in ASSIGN_RE.finditer(line):
                config = match.group("config")
                if config not in disabled:
                    continue
                # Treat everything after an unescaped '#' as a Makefile comment.
                rhs = match.group("rhs").split("#", 1)[0].strip()
                if not rhs:
                    continue
                for token in rhs.split():
                    if not looks_like_target(token):
                        continue
                    target = normalize_target(token, makefile.parent, root)
                    entries.append(
                        (config, target, str(makefile.relative_to(root)))
                    )
    return entries


def main(argv: List[str]) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(
        description=(
            "Output CSV mapping of disabled CONFIG entries to driver "
            "targets referenced in drivers/*/Makefile files."
        )
    )
    parser.add_argument(
        "--defconfig",
        type=Path,
        default=root / "arch/x86/configs/openeuler_defconfig",
        help="Path to defconfig to scan (default: arch/x86/configs/openeuler_defconfig)",
    )
    parser.add_argument(
        "--drivers-dir",
        type=Path,
        default=root / "drivers",
        help="Root drivers directory to scan (default: drivers)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write CSV output to file instead of stdout",
    )

    args = parser.parse_args(argv)
    defconfig_path = args.defconfig.resolve()
    drivers_dir = args.drivers_dir.resolve()

    if not defconfig_path.exists():
        parser.error(f"defconfig file not found: {defconfig_path}")
    if not drivers_dir.is_dir():
        parser.error(f"drivers directory not found: {drivers_dir}")

    disabled = collect_disabled_configs(defconfig_path)

    rows: List[Tuple[str, str, str]] = []
    for makefile in drivers_dir.rglob("Makefile"):
        rows.extend(scan_makefile(makefile, disabled, root))

    rows.sort(key=lambda item: (item[0], item[1], item[2]))

    if args.output is not None:
        with args.output.open("w", newline="", encoding="utf-8") as output_stream:
            writer = csv.writer(output_stream)
            writer.writerow(["CONFIG", "driver_path", "makefile"])
            writer.writerows(rows)
    else:
        writer = csv.writer(sys.stdout)
        writer.writerow(["CONFIG", "driver_path", "makefile"])
        writer.writerows(rows)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
