#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import argparse
import re
import sys
from pathlib import Path
from typing import Set

# Matches declarations like "config FOO" or "menuconfig FOO" inside Kconfig files.
CONFIG_DECL_RE = re.compile(r"^\s*(?:menu)?config\s+([A-Z0-9_]+)\b", re.IGNORECASE)
# Matches enabled defconfig entries such as "CONFIG_FOO=y" or "CONFIG_FOO=m".
DEFCONFIG_ENTRY_RE = re.compile(
    r"^CONFIG_([A-Z0-9_]+)=(y|m)\s*(?:#.*)?$", re.IGNORECASE
)


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def collect_symbols(module_dir: Path) -> Set[str]:
    """Return CONFIG symbols declared in Kconfig files under module_dir."""
    symbols: Set[str] = set()
    for kconfig in module_dir.rglob("Kconfig*"):
        if not kconfig.is_file():
            continue
        try:
            content = kconfig.read_text(encoding="utf-8")
        except UnicodeDecodeError as exc:
            sys.exit(
                f"Error collecting CONFIG symbols: failed to decode {kconfig} as UTF-8: {exc}"
            )
        except OSError as exc:
            sys.exit(f"Error collecting CONFIG symbols: failed to read {kconfig}: {exc}")
        for line in content.splitlines():
            match = CONFIG_DECL_RE.match(line)
            if match:
                symbols.add(match.group(1))
    return symbols


def disable_symbols(defconfig: Path, symbols: Set[str]) -> int:
    """Disable enabled CONFIG entries in defconfig for the provided symbols."""
    try:
        lines = defconfig.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        sys.exit(f"Failed to read defconfig {defconfig} while disabling symbols: {exc}")

    disabled = 0
    new_lines = []
    for line in lines:
        match = DEFCONFIG_ENTRY_RE.match(line)
        if match and match.group(1) in symbols:
            new_lines.append(f"# CONFIG_{match.group(1)} is not set")
            disabled += 1
            continue
        new_lines.append(line)

    if disabled:
        try:
            defconfig.write_text("\n".join(new_lines) + "\n", encoding="utf-8")
        except OSError as exc:
            sys.exit(f"Failed to write updated defconfig {defconfig}: {exc}")
    return disabled


def validate_module_dir(path: Path, root: Path) -> Path:
    """Ensure the module path resolves inside drivers/ or fs/ within the repository."""
    path = (root / path).resolve()
    if not path.is_dir():
        sys.exit(f"{path} is not a valid directory")
    try:
        relative = path.relative_to(root)
    except ValueError:
        sys.exit(f"{path} is outside of the repository")
    if relative.parts[0] not in {"drivers", "fs"}:
        sys.exit("Module directory must be inside drivers/ or fs/")
    return path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Disable CONFIG entries defined under a drivers/ or fs/ subdirectory "
            "in arch/x86/configs/openeuler_defconfig."
        )
    )
    parser.add_argument(
        "--src",
        dest="module",
        action="append",
        help="Module directory under drivers/ or fs/ to disable; can be provided multiple times.",
    )
    parser.add_argument(
        "module_positional",
        nargs="*",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--defconfig",
        default="arch/x86/configs/openeuler_defconfig",
        help="Path to the defconfig to update (default: arch/x86/configs/openeuler_defconfig).",
    )
    return parser.parse_args()


def gather_module_dirs(args: argparse.Namespace) -> list[str]:
    module_dirs: list[str] = []
    if args.module is not None:
        module_dirs.extend(args.module)
    if args.module_positional:
        module_dirs.extend(args.module_positional)
        print(
            "Warning: positional module arguments are deprecated; please use --src",
            file=sys.stderr,
        )
    if not module_dirs:
        sys.exit(
            "At least one module directory must be provided using --src "
            "(e.g. --src drivers/accel --src fs/ext4)"
        )
    return module_dirs


def main() -> None:
    args = parse_args()
    root = repo_root()
    defconfig = (root / args.defconfig).resolve()

    if not defconfig.is_file():
        sys.exit(f"Defconfig file {defconfig} does not exist")

    module_dirs = gather_module_dirs(args)

    symbols: Set[str] = set()
    for module_path in module_dirs:
        module_dir = validate_module_dir(Path(module_path), root)
        symbols.update(collect_symbols(module_dir))

    if not symbols:
        sys.exit(
            "No CONFIG entries found in Kconfig files under the provided module directories."
        )

    disabled = disable_symbols(defconfig, symbols)
    if not disabled:
        print("No matching CONFIG entries enabled in defconfig; no changes made.")
    else:
        print(f"Disabled {disabled} CONFIG entries in {defconfig}")


if __name__ == "__main__":
    main()
