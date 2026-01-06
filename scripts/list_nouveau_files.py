#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import csv
import pathlib
import sys


def main():
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    nouveau_root = repo_root / "drivers" / "gpu" / "drm" / "nouveau"
    skip_names = {"Kconfig", "Kbuild", "Makefile"}

    if not nouveau_root.is_dir():
        print(f"nouveau directory not found at {nouveau_root}", file=sys.stderr)
        return 1

    files = [
        path.relative_to(repo_root)
        for path in nouveau_root.rglob("*")
        if path.is_file() and path.name not in skip_names
    ]

    output_path = repo_root / "nouveau_files.csv"
    with output_path.open("w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        for path in sorted(files):
            writer.writerow([path.as_posix()])

    try:
        for path in sorted(files):
            print(path.as_posix())
    except BrokenPipeError:
        sys.stdout.close()
        sys.stderr.close()
        return 141

    return 0


if __name__ == "__main__":
    sys.exit(main())
