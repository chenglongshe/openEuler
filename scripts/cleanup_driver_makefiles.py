#!/usr/bin/env python3
"""
Remove disabled driver Makefile entries based on openeuler_defconfig.

The script collects all CONFIG options set to `n` (or marked as "is not set")
from arch/x86/configs/openeuler_defconfig, then scans Makefiles under the
drivers directory and drops assignment fragments that select objects with the
disabled configs (e.g. `obj-$(CONFIG_FOO) += foo.o`).

Usage:
    python3 scripts/cleanup_driver_makefiles.py
    python3 scripts/cleanup_driver_makefiles.py --dry-run
"""

from __future__ import annotations

import argparse
import pathlib
import re
import signal
from typing import Iterable


DEFCONFIG_PATH = pathlib.Path("arch/x86/configs/openeuler_defconfig")
DRIVERS_DIR = pathlib.Path("drivers")

# Matches object selection assignments such as:
#   obj-$(CONFIG_FOO) += foo.o
#   foo-$(CONFIG_BAR) := bar.o
# The trailing lookahead stops the match before the next assignment selector,
# an inline comment, or the end of the line so multiple assignments on a single
# line are handled independently.
ASSIGNMENT_PATTERN = re.compile(
    r"""
    \b\S*-\$\(\s*(?P<config>CONFIG_[A-Za-z0-9_]+)\s*\)   # selector using CONFIG_FOO
    \s*[+:]?=                                           # assignment operator
    \s*                                                 # optional whitespace
    [^#\n]*?                                            # minimal assignment body
    (?=                                                 # stop when we hit...
        (?:\s+\S*-\$\(\s*CONFIG_[A-Za-z0-9_]+)          #   another selector
        | \s*#                                          #   a comment
        | $                                             #   or end of line
    )
    """,
    re.VERBOSE,
)
# Examples matched by ASSIGNMENT_PATTERN:
#   obj-$(CONFIG_FOO) += foo.o
#   foo-$(CONFIG_BAR) := bar.o
#   obj-$(CONFIG_FOO) += foo.o obj-$(CONFIG_BAR) += bar.o
# It deliberately stops before inline comments such as:
#   obj-$(CONFIG_FOO) += foo.o # comment

# Exit cleanly when the output is piped through tools like `head`.
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

def collect_disabled_configs(defconfig: pathlib.Path) -> set[str]:
    disabled: set[str] = set()
    not_set = re.compile(r"^#\s*(CONFIG_[A-Za-z0-9_]+)\s+is not set")
    set_to_n = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=n\b")

    with defconfig.open(encoding="utf-8") as f:
        for line in f:
            if match := not_set.match(line):
                disabled.add(match.group(1))
            elif match := set_to_n.match(line):
                disabled.add(match.group(1))

    return disabled


def strip_disabled_assignments(
    line: str, disabled_configs: set[str]
) -> tuple[str, bool]:
    changed = False
    new_line_parts = []
    last_index = 0

    # We remove selectors tied to disabled configs to preserve the rest of the
    # original line formatting.
    for match in ASSIGNMENT_PATTERN.finditer(line):
        if match.group("config") in disabled_configs:
            changed = True
            new_line_parts.append(line[last_index:match.start()])
            last_index = match.end()

    if not changed:
        return line, False

    new_line_parts.append(line[last_index:])
    new_line = "".join(new_line_parts)

    if not new_line.strip():
        return "", True

    # Preserve leading indentation while collapsing extra whitespace that may
    # appear after removing assignment selectors.
    leading_ws = len(new_line) - len(new_line.lstrip(" \t"))
    prefix = new_line[:leading_ws]
    body = new_line[leading_ws:]
    body = re.sub(r"[ \t]{2,}", " ", body)

    return prefix + body.rstrip(" "), True


def process_makefile(path: pathlib.Path, disabled_configs: set[str]) -> bool:
    new_lines, changed = transform_makefile_lines(
        path.read_text(encoding="utf-8").splitlines(), disabled_configs
    )
    if changed:
        path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")
    return changed


def would_change(path: pathlib.Path, disabled_configs: set[str]) -> bool:
    _, changed = transform_makefile_lines(
        path.read_text(encoding="utf-8").splitlines(), disabled_configs
    )
    return changed


def transform_makefile_lines(
    lines: list[str], disabled_configs: set[str]
) -> tuple[list[str], bool]:
    new_lines: list[str] = []
    changed = False
    skip_continuation = False

    for line in lines:
        if skip_continuation:
            # Skip continuation lines that belonged to a removed assignment.
            changed = True
            if not line.rstrip().endswith("\\"):
                skip_continuation = False
            continue

        if line.lstrip().startswith("#"):
            new_lines.append(line)
            continue

        stripped_line, line_changed = strip_disabled_assignments(
            line, disabled_configs
        )
        if line_changed:
            changed = True
            removed_line = not stripped_line.strip()
            original_had_backslash = line.rstrip().endswith("\\")
            if removed_line:
                if original_had_backslash:
                    skip_continuation = True
                continue
            new_lines.append(stripped_line)
            continue

        new_lines.append(line)

    return new_lines, changed


def iter_makefiles(drivers_root: pathlib.Path) -> Iterable[pathlib.Path]:
    yield from drivers_root.rglob("Makefile")


def _print_paths(header: str, paths: list[pathlib.Path]) -> None:
    try:
        print(header)
        for path in paths:
            print(f" - {path}")
    except BrokenPipeError:
        return


def main() -> None:
    parser = argparse.ArgumentParser(description="Remove disabled driver Makefile entries.")
    parser.add_argument(
        "--defconfig",
        type=pathlib.Path,
        default=DEFCONFIG_PATH,
        help="Path to openeuler_defconfig (default: %(default)s)",
    )
    parser.add_argument(
        "--drivers-root",
        type=pathlib.Path,
        default=DRIVERS_DIR,
        help="Root directory containing driver Makefiles (default: %(default)s)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show files that would change without modifying them.",
    )
    args = parser.parse_args()

    disabled_configs = collect_disabled_configs(args.defconfig)
    if not disabled_configs:
        print(f"No disabled configs found in {args.defconfig}")
        return

    changed_files = []
    for makefile in iter_makefiles(args.drivers_root):
        if args.dry_run:
            if would_change(makefile, disabled_configs):
                changed_files.append(makefile)
        else:
            if process_makefile(makefile, disabled_configs):
                changed_files.append(makefile)

    if args.dry_run:
        if changed_files:
            _print_paths("Files that would change:", changed_files)
        else:
            print("No changes would be made.")
    else:
        if changed_files:
            _print_paths("Updated files:", changed_files)
        else:
            print("No Makefiles required changes.")


if __name__ == "__main__":
    main()
