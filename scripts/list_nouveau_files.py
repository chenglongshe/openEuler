#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import argparse
import csv
import pathlib
import sys

DEFAULT_OUTPUT = "nouveau_files.csv"


def parse_args():
    parser = argparse.ArgumentParser(
        description="List file paths under a driver subtree and save them to a CSV file."
    )
    parser.add_argument(
        "path",
        nargs="?",
        default="drivers/gpu/drm/nouveau",
        help="Subtree to scan (e.g., drivers/gpu/drm/radeon or drivers/net/ethernet/yunsilicon)",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help=(
            f"Output CSV file (default: {DEFAULT_OUTPUT} in repo root; custom paths "
            "are resolved from the current directory)"
        ),
    )
    return parser.parse_args()


def main():
    args = parse_args()
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    target_root = pathlib.Path(args.path)
    if not target_root.is_absolute():
        target_root = repo_root / target_root
    target_root = target_root.resolve()
    skip_names = {"Kconfig", "Kbuild", "Makefile"}

    if not target_root.is_dir():
        print(f"Target directory not found at {target_root}", file=sys.stderr)
        return 1

    file_paths = sorted(
        path
        for path in target_root.rglob("*")
        if path.is_file() and path.name not in skip_names
    )
    output_path = args.output.resolve(strict=False) if args.output else (repo_root / DEFAULT_OUTPUT)
    try:
        with output_path.open("w", newline="") as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(["file_path"])
            for path in file_paths:
                rel = path
                try:
                    rel = path.relative_to(repo_root)
                except ValueError:
                    pass
                writer.writerow([rel.as_posix()])
    except OSError as exc:
        print(f"Failed to write CSV to {output_path}: {exc}", file=sys.stderr)
        return 1

    try:
        for path in file_paths:
            rel = path
            try:
                rel = path.relative_to(repo_root)
            except ValueError:
                pass
            print(rel.as_posix())
    except BrokenPipeError:
        sys.stdout.close()
        sys.stderr.close()
        return 141

    return 0


if __name__ == "__main__":
    sys.exit(main())
