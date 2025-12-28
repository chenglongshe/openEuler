#!/usr/bin/env python3

import argparse
import csv
import re
import posixpath
from pathlib import Path
from typing import List, Optional, Set, Tuple

# Common patterns found in Kbuild .cmd files
RE_SOURCE = re.compile(r'^\s*source_[^:]+:=\s*(\S+\.c)\s*$', re.M)
RE_CMD_CC = re.compile(r'^\s*cmd_[^:]+:=.*?\s-c\s.*?(\S+\.c)(?:\s|$)', re.M)

# Parse #include "xxx.c" inside C files
RE_INCLUDE_C = re.compile(r'^\s*#\s*include\s*"([^"]+\.c)"\s*$', re.M)

MAX_CMD_SEARCH_DEPTH = 4

def normpath_rel(s: str) -> str:
    """Normalize to POSIX style and collapse ../ ./ to avoid path mismatches"""
    s = s.strip().strip('"').strip("'").replace("\\", "/")
    s = posixpath.normpath(s)
    if s.startswith("./"):
        s = s[2:]
    return s


def extract_c_from_cmd_text(cmd_text: str) -> Set[str]:
    """Extract possible .c paths from .cmd text"""
    return set(RE_SOURCE.findall(cmd_text)) | set(RE_CMD_CC.findall(cmd_text))


def find_kernel_top(
    start: Path,
    dirs: Tuple[str, ...] = ("drivers", "fs", "net"),
    max_depth_cmd_check: int = MAX_CMD_SEARCH_DEPTH,
) -> Path:
    """
    Auto-detect kernel build root (searching upward from start):
      - at least one directory in dirs exists
      - and a *.cmd file can be found under those directories (prefer build output tree)
    Return start if nothing qualifies.
    """
    start = start.resolve()
    cur = start
    while True:
        # Condition 1: at least one target directory exists
        exists_any = any((cur / d).is_dir() for d in dirs)
        if exists_any:
            # Condition 2: shallow search for *.cmd under dirs (avoid full rglob cost)
            found_cmd = False
            for d in dirs:
                base = cur / d
                if not base.is_dir():
                    continue
                # Layered search: dive at most max_depth_cmd_check levels
                # Use combined glob patterns to reduce cost
                patterns = ["*.cmd"]
                for depth in range(1, max_depth_cmd_check + 1):
                    patterns.append(str(Path(*(["*"] * depth)) / "*.cmd"))
                for pat in patterns:
                    if any(base.glob(pat)):
                        found_cmd = True
                        break
                if found_cmd:
                    break

            if found_cmd:
                return cur

        # Stop at filesystem root
        if cur.parent == cur:
            break
        cur = cur.parent

    return start


def cmd_to_base_relpath(kernel_top: Path, base_name: str, raw_c_path: str) -> Optional[str]:
    """
    Map a .cmd C path to one relative to base_name/ (return like gpu/drm/...)
    Supports paths such as drivers/gpu/drm/amd/amdgpu/../acp/acp_hw.c via normpath
    """
    s = normpath_rel(raw_c_path)

    # 1) Trim to after /base_name/ (works for absolute or relative)
    marker = f"/{base_name}/"
    if marker in f"/{s}":
        s2 = f"/{s}"
        rel = s2.split(marker, 1)[1]
        return normpath_rel(rel)

    # 2) Starts with base_name/
    if s.startswith(f"{base_name}/"):
        return normpath_rel(s[len(base_name) + 1 :])

    # 3) Relative path: resolve against kernel_top and see if it falls under base_name/
    p = Path(s)
    if not p.is_absolute():
        cand = (kernel_top / p).resolve()
        try:
            rel_to_top = cand.relative_to(kernel_top.resolve()).as_posix()
        except ValueError:
            rel_to_top = None
        if rel_to_top and (rel_to_top == base_name or rel_to_top.startswith(base_name + "/")):
            return normpath_rel(rel_to_top[len(base_name) + 1 :])

    return None


def list_all_c(base_dir: Path) -> Set[str]:
    """All *.c files under base_dir (relative to base_dir)"""
    res = set()
    for p in base_dir.rglob("*.c"):
        if any(part.startswith(".") for part in p.parts):
            continue
        res.add(p.relative_to(base_dir).as_posix())
    return res


def built_c_from_o(base_dir: Path) -> Set[str]:
    """Fallback: if xxx.o exists beside xxx.c, treat xxx.c as built (relative to base_dir)"""
    built = set()
    for o in base_dir.rglob("*.o"):
        if any(part.startswith(".") for part in o.parts):
            continue
        c = o.with_suffix(".c")
        if c.exists():
            built.add(c.relative_to(base_dir).as_posix())
    return built


def parse_built_c_from_cmd(kernel_top: Path, base_dir: Path, base_name: str) -> Set[str]:
    """Scan *.cmd under base_dir and parse built .c files (relative to base_dir)"""
    built = set()
    for cmd in base_dir.rglob("*.cmd"):
        try:
            text = cmd.read_text(errors="ignore")
        except (OSError, UnicodeDecodeError):
            continue
        for raw in extract_c_from_cmd_text(text):
            rel = cmd_to_base_relpath(kernel_top, base_name, raw)
            if rel:
                built.add(rel)
    return built


def resolve_included_c(include_str: str, including_rel: str) -> str:
    """Resolve #include \"xxx.c\" into a path relative to base_dir"""
    inc = normpath_rel(include_str)
    base_dir = posixpath.dirname(including_rel)
    combined = posixpath.normpath(posixpath.join(base_dir, inc))
    if combined.startswith("./"):
        combined = combined[2:]
    return combined


def expand_built_by_includes(base_dir: Path, built_set: Set[str], all_c: Set[str]) -> Set[str]:
    """
    Mark \"*.c\" included by already built files as used (transitive closure)
    """
    queue = list(built_set)
    seen = set(built_set)

    while queue:
        rel = queue.pop()
        if rel not in all_c:
            continue
        p = base_dir / rel
        try:
            text = p.read_text(errors="ignore")
        except (OSError, UnicodeDecodeError):
            continue

        for inc in RE_INCLUDE_C.findall(text):
            inc_rel = resolve_included_c(inc, rel)
            if inc_rel in all_c and inc_rel not in seen:
                seen.add(inc_rel)
                built_set.add(inc_rel)
                queue.append(inc_rel)

    return built_set


def compute_unbuilt_for_dir(kernel_top: Path, base_name: str) -> List[str]:
    """
    Calculate unbuilt .c files under a top-level directory (drivers/fs/net).
    Return sorted relative paths prefixed with the directory (e.g., drivers/xxx.c).
    """
    base_dir = kernel_top / base_name
    if not base_dir.is_dir():
        return []

    all_c = list_all_c(base_dir)

    # Built set = parsed from cmd + fallback from .o + expansion via include "*.c"
    built_c = parse_built_c_from_cmd(kernel_top, base_dir, base_name) | built_c_from_o(base_dir)
    built_c = expand_built_by_includes(base_dir, built_c, all_c)

    unbuilt = sorted(all_c - built_c)
    return [f"{base_name}/{p}" for p in unbuilt]


def write_one_csv(out_csv: Path, sections: List[Tuple[str, List[str]]], header: bool) -> None:
    """
    Output a CSV: each section starts with '#drivers' row followed by that section's paths
    """
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if header:
            w.writerow(["path"])
        for name, paths in sections:
            w.writerow([f"#{name}"])
            for p in paths:
                w.writerow([p])


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Generate a CSV of unbuilt .c files (drivers/fs/net) with #drivers/#fs/#net sections; includes #include \"*.c\" files."
    )
    ap.add_argument("--dirs", default="drivers,fs,net", help='Directories to process, comma-separated. Default "drivers,fs,net".')
    ap.add_argument("--out", default="unbuilt_all.csv", help="Output CSV file name (default unbuilt_all.csv).")
    ap.add_argument("--no-header", action="store_true", help="Skip header row (path header is written by default).")
    ap.add_argument(
        "--top",
        default=None,
        help="Optional: manually specify kernel build root. Usually unnecessary (auto-detected).",
    )
    args = ap.parse_args()

    dirs = [x.strip() for x in args.dirs.split(",") if x.strip()]
    start = Path(".").resolve()

    # Auto-detect kernel_top: prefer build root containing *.cmd files
    kernel_top = Path(args.top).resolve() if args.top else find_kernel_top(start, tuple(dirs))

    sections = []
    for d in dirs:
        paths = compute_unbuilt_for_dir(kernel_top, d)
        sections.append((d, paths))
        print(f"[OK] {d}: unbuilt={len(paths)}")

    write_one_csv(Path(args.out), sections, header=(not args.no_header))
    print(f"[OK] kernel_top = {kernel_top}")
    print(f"[OK] output     = {Path(args.out).resolve()}")


if __name__ == "__main__":
    main()
