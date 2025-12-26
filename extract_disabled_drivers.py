#!/usr/bin/env python3
import csv
import os
import re
import sys
from collections import defaultdict


REPO_ROOT = os.path.abspath(os.getcwd())
DEFCONFIG_PATH = os.path.join(REPO_ROOT, "arch", "x86", "configs", "openeuler_defconfig")
DRIVERS_ROOT = os.path.join(REPO_ROOT, "drivers")
OUTPUT_CSV = os.path.join(REPO_ROOT, "disabled_drivers_cfiles.csv")

COMPLEX_FUNCS = (
    "wildcard",
    "shell",
    "foreach",
    "if",
    "call",
    "filter",
    "patsubst",
    "subst",
    "addprefix",
    "addsuffix",
    "word",
    "words",
    "dir",
    "notdir",
    "basename",
    "realpath",
    "lastword",
    "sort",
    "unique",
    "abspath",
    "info",
    "error",
    "warning",
)


def read_defconfig_n(path):
    disabled = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if re.match(r"^CONFIG_[A-Za-z0-9_]+=n$", line):
                cfg = line.split("=", 1)[0]
                disabled.add(cfg)
    return disabled


def join_continuations(lines):
    merged = []
    buffer = ""
    for raw in lines:
        stripped = raw.rstrip("\n")
        if stripped.endswith("\\"):
            buffer += stripped[:-1] + " "
            continue
        buffer += stripped
        merged.append(buffer)
        buffer = ""
    if buffer:
        merged.append(buffer)
    return merged


def has_complex_func(line):
    for func in COMPLEX_FUNCS:
        if f"$({func}" in line:
            return True
    return False


def expand_tokens(expr, var_map, warnings):
    tokens = []
    for raw in expr.split():
        current = [raw]
        vars_found = re.findall(r"\$\(([A-Za-z0-9_]+)\)", raw)
        for var in vars_found:
            next_tokens = []
            replacement = var_map.get(var)
            if not replacement:
                warnings.append(f"Unresolved variable {var} in token '{raw}', skipped")
                current = []
                break
            for partial in current:
                for val in replacement:
                    next_tokens.append(partial.replace(f"$({var})", val))
            current = next_tokens
        tokens.extend(current)
    return tokens


def pathify_tokens(base_dir, tokens, warnings):
    paths = []
    for tok in tokens:
        if "$(" in tok:
            warnings.append(f"Unexpanded token '{tok}', skipped")
            continue
        if tok.endswith("/"):
            norm = os.path.normpath(os.path.join(base_dir, tok)) + "/"
            paths.append(norm)
        elif tok.endswith((".o", ".c")):
            norm = os.path.normpath(os.path.join(base_dir, tok))
            paths.append(norm)
        else:
            warnings.append(f"Unsupported token '{tok}', skipped")
    return paths


def parse_makefile(path):
    base_dir = os.path.dirname(path)
    var_map = defaultdict(list)
    config_items = defaultdict(list)
    default_items = []
    aggregates = defaultdict(list)
    warnings = []

    with open(path, encoding="utf-8") as f:
        lines = join_continuations(f.readlines())

    for raw in lines:
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if has_complex_func(line):
            warnings.append(f"Complex function in {path}: '{raw.strip()}', skipped")
            continue

        m = re.match(r"^(obj|subdir)-\$\((CONFIG_[A-Za-z0-9_]+)\)\s*\+?=\s*(.+)$", line)
        if m:
            tokens = expand_tokens(m.group(3), var_map, warnings)
            paths = pathify_tokens(base_dir, tokens, warnings)
            config_items[m.group(2)].extend(paths)
            continue

        m = re.match(r"^(obj|subdir)-(y|m)\s*\+?=\s*(.+)$", line)
        if m:
            tokens = expand_tokens(m.group(3), var_map, warnings)
            paths = pathify_tokens(base_dir, tokens, warnings)
            default_items.extend(paths)
            continue

        m = re.match(r"^([A-Za-z0-9_./-]+)-(y|objs)\s*(?::=|\+=|=)\s*(.+)$", line)
        if m and not m.group(1).startswith(("obj", "subdir")):
            tokens = expand_tokens(m.group(3), var_map, warnings)
            paths = pathify_tokens(base_dir, tokens, warnings)
            target_name = m.group(1)
            if not target_name.endswith(".o"):
                target_name = f"{target_name}.o"
            target = os.path.normpath(os.path.join(base_dir, target_name))
            aggregates[target].extend(paths)
            continue

        m = re.match(r"^([A-Za-z0-9_./-]+)\s*(:=|\+=|=)\s*(.*)$", line)
        if m:
            var, op, expr = m.group(1), m.group(2), m.group(3)
            tokens = expand_tokens(expr, var_map, warnings)
            if op == "+=":
                var_map[var].extend(tokens)
            else:
                var_map[var] = tokens

    return {
        "config_items": config_items,
        "default_items": default_items,
        "aggregates": aggregates,
        "warnings": warnings,
    }


def collect_parse_results():
    parsed = {}
    for root, _, files in os.walk(DRIVERS_ROOT):
        for fname in files:
            if fname not in ("Makefile", "Kbuild"):
                continue
            path = os.path.join(root, fname)
            data = parse_makefile(path)
            if root in parsed:
                existing = parsed[root]
                for cfg, items in data["config_items"].items():
                    existing["config_items"][cfg].extend(items)
                existing["default_items"].extend(data["default_items"])
                for tgt, comps in data["aggregates"].items():
                    existing["aggregates"][tgt].extend(comps)
                existing["warnings"].extend(data["warnings"])
            else:
                parsed[root] = data
    return parsed


def resolve_tokens(parsed, disabled_configs):
    aggregates = defaultdict(list)
    for data in parsed.values():
        for target, comps in data["aggregates"].items():
            aggregates[target].extend(comps)

    warnings = []
    final_c_files = set()
    visited_o = set()
    visited_dir = set()

    def add_c_path(obj_path):
        c_path = obj_path[:-2] + ".c"
        if os.path.isfile(c_path):
            final_c_files.add(os.path.relpath(c_path, REPO_ROOT))
        else:
            warnings.append(f"Missing .c for {obj_path}, skipped")

    def walk_token(tok):
        if tok.endswith("/"):
            dir_path = tok.rstrip("/")
            if dir_path in visited_dir:
                return
            visited_dir.add(dir_path)
            data = parsed.get(dir_path)
            if not data:
                warnings.append(f"No Kbuild/Makefile parsed for {dir_path}, skipped")
                return
            items = list(data["default_items"])
            for cfg in disabled_configs:
                items.extend(data["config_items"].get(cfg, []))
            for item in items:
                walk_token(item)
            return

        if tok.endswith(".c"):
            if os.path.isfile(tok):
                final_c_files.add(os.path.relpath(tok, REPO_ROOT))
            else:
                warnings.append(f".c file {tok} not found, skipped")
            return

        if tok.endswith(".o"):
            if tok in visited_o:
                return
            visited_o.add(tok)
            comps = aggregates.get(tok)
            if comps:
                for comp in comps:
                    walk_token(comp)
            else:
                add_c_path(tok)
            return

        warnings.append(f"Unhandled token {tok}, skipped")

    seeds = []
    for dir_path, data in parsed.items():
        for cfg in disabled_configs:
            seeds.extend(data["config_items"].get(cfg, []))

    for seed in seeds:
        walk_token(seed)

    return final_c_files, warnings


def main():
    disabled_configs = read_defconfig_n(DEFCONFIG_PATH)
    parsed = collect_parse_results()

    final_c_files, warn_from_resolve = resolve_tokens(parsed, disabled_configs)
    all_warnings = []
    for data in parsed.values():
        all_warnings.extend(data["warnings"])
    all_warnings.extend(warn_from_resolve)

    with open(OUTPUT_CSV, "w", newline="", encoding="utf-8") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(["c_file"])
        for path in sorted(final_c_files):
            writer.writerow([path])

    print(f"Disabled CONFIG count: {len(disabled_configs)}")
    print(f"Collected .c files: {len(final_c_files)}")
    print(f"Skipped entries: {len(all_warnings)}")
    for w in all_warnings:
        print(f"warning: {w}")


if __name__ == "__main__":
    sys.setrecursionlimit(5000)
    main()
