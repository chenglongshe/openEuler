#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import argparse
import csv
import pathlib
import sys

DEFAULT_OUTPUT = "nouveau_files.csv"


def parse_args():
    parser = argparse.ArgumentParser(
        description="List nouveau file paths and save them to a CSV file."
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
    nouveau_root = repo_root / "drivers" / "gpu" / "drm" / "nouveau"
    skip_names = {"Kconfig", "Kbuild", "Makefile"}

    if not nouveau_root.is_dir():
        print(f"nouveau directory not found at {nouveau_root}", file=sys.stderr)
        return 1

    file_paths = sorted(
        path.relative_to(repo_root)
        for path in nouveau_root.rglob("*")
        if path.is_file() and path.name not in skip_names
    )
    output_path = args.output.resolve(strict=False) if args.output else (repo_root / DEFAULT_OUTPUT)
    try:
        with output_path.open("w", newline="") as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(["file_path"])
            for path in file_paths:
                writer.writerow([path.as_posix()])
    except OSError as exc:
        print(f"Failed to write CSV to {output_path}: {exc}", file=sys.stderr)
        return 1

    try:
        for path in file_paths:
            print(path.as_posix())
    except BrokenPipeError:
        sys.stdout.close()
        sys.stderr.close()
        return 141

    return 0


if __name__ == "__main__":
    sys.exit(main())
