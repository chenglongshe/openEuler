#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
from typing import DefaultDict, Dict, Iterable, List, Sequence, Set, Tuple


@dataclass(frozen=True)
class Requirement:
    config: str
    relation: str  # set, unset, y, m, not-y, not-m

    def is_satisfied(self, statuses: Dict[str, str]) -> bool:
        value = statuses.get(self.config, "n")
        if self.relation == "set":
            return value in ("y", "m")
        if self.relation == "unset":
            return value not in ("y", "m")
        if self.relation == "y":
            return value == "y"
        if self.relation == "m":
            return value == "m"
        if self.relation == "not-y":
            return value != "y"
        if self.relation == "not-m":
            return value != "m"
        return False


@dataclass
class Condition:
    true_reqs: List[Requirement]
    false_reqs: List[Requirement]
    in_true: bool = True

    def current_requirements(self) -> List[Requirement]:
        return self.true_reqs if self.in_true else self.false_reqs


def parse_defconfig(defconfig_path: Path) -> Dict[str, str]:
    statuses: Dict[str, str] = {}
    for raw_line in defconfig_path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            if "is not set" in line:
                name = line.split()[1]
                statuses[name] = "n"
            continue
        if line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            statuses[name] = value.strip()
    return statuses


def strip_comment(line: str) -> str:
    parts = re.split(r"(?<!\\)#", line, 1)
    return parts[0].strip()


def preprocess_makefile(path: Path) -> List[str]:
    lines: List[str] = []
    buffer = ""
    for raw_line in path.read_text().splitlines():
        line = raw_line.rstrip()
        if line.endswith("\\"):
            buffer += line[:-1] + " "
            continue
        buffer += line
        lines.append(buffer)
        buffer = ""
    if buffer:
        lines.append(buffer)
    return lines


def configs_in_text(text: str) -> List[str]:
    return re.findall(r"CONFIG_[A-Za-z0-9_]+", text)


def parse_condition(line: str) -> Condition | None:
    m = re.match(r"^ifdef\s+(CONFIG_[A-Za-z0-9_]+)", line)
    if m:
        cfg = m.group(1)
        return Condition([Requirement(cfg, "set")], [Requirement(cfg, "unset")])
    m = re.match(r"^ifndef\s+(CONFIG_[A-Za-z0-9_]+)", line)
    if m:
        cfg = m.group(1)
        return Condition([Requirement(cfg, "unset")], [Requirement(cfg, "set")])
    m = re.match(r"^ifn?eq\s*\(\s*\$\((CONFIG_[^)]+)\)\s*,\s*([^)]+)\)", line)
    if m:
        cfg, value = m.group(1), m.group(2).strip()
        value = value.strip('"').strip("'")
        is_eq = line.startswith("ifeq")
        if value == "y":
            true_req, false_req = Requirement(cfg, "y"), Requirement(cfg, "not-y")
        elif value == "m":
            true_req, false_req = Requirement(cfg, "m"), Requirement(cfg, "not-m")
        elif value == "":
            true_req, false_req = Requirement(cfg, "unset"), Requirement(cfg, "set")
        else:
            return None
        return Condition([true_req], [false_req]) if is_eq else Condition([false_req], [true_req])
    return None


def dedup_gatings(gatings: Iterable[Sequence[Requirement]]) -> List[List[Requirement]]:
    unique = set()
    result: List[List[Requirement]] = []
    for reqs in gatings:
        key = tuple(sorted(reqs, key=lambda r: (r.config, r.relation)))
        if key in unique:
            continue
        unique.add(key)
        result.append(list(key))
    return result


def combine_gatings(
    inherited: Iterable[Sequence[Requirement]], extra: Sequence[Requirement]
) -> List[List[Requirement]]:
    combined: List[List[Requirement]] = []
    for base in inherited:
        merged = list(base) + list(extra)
        combined.append(merged)
    return dedup_gatings(combined)


def normalize_token(token: str) -> str:
    cleaned = token.strip()
    cleaned = cleaned.replace("$(obj)/", "").replace("$(src)/", "").replace("$(srctree)/", "")
    cleaned = cleaned.rstrip("/")
    cleaned = cleaned.lstrip("./")
    return cleaned.strip()


def parse_aggregator_var(varname: str) -> Tuple[str | None, str | None]:
    if varname.startswith("obj-"):
        return None, None
    base = varname.strip()
    kind = None
    for suffix in ("-objs", "-y", "-m"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            kind = suffix
            break
    if "-$(CONFIG" in base:
        base = base.split("-$(CONFIG", 1)[0]
    base = base.rstrip("-")
    if not base:
        return None, kind
    return base, kind


def add_gatings(mapping: DefaultDict[str, List[List[Requirement]]], key: str, gating_sets: List[List[Requirement]]) -> None:
    if not gating_sets:
        return
    existing = mapping.get(key, [])
    mapping[key] = dedup_gatings(existing + gating_sets)


def collect_mappings(
    root: Path, drivers_dir: Path, statuses: Dict[str, str]
) -> Tuple[DefaultDict[str, List[List[Requirement]]], DefaultDict[str, List[Tuple[str, List[Requirement]]]]]:
    root_objects: DefaultDict[str, List[List[Requirement]]] = defaultdict(list)
    aggregator_children: DefaultDict[str, List[Tuple[str, List[Requirement]]]] = defaultdict(list)

    dir_gatings: Dict[Path, List[List[Requirement]]] = {drivers_dir: [[]]}
    queue: deque[Path] = deque([drivers_dir])

    while queue:
        current_dir = queue.popleft()
        inherited = dir_gatings.get(current_dir, [[]])
        rel_prefix = current_dir.relative_to(root)
        makefile_path = current_dir / "Makefile"
        if not makefile_path.exists():
            alt_path = current_dir / "Kbuild"
            if not alt_path.exists():
                continue
            makefile_path = alt_path
        lines = preprocess_makefile(makefile_path)
        cond_stack: List[Condition] = []

        for raw_line in lines:
            line = strip_comment(raw_line)
            if not line:
                continue

            if line.startswith("endif"):
                if cond_stack:
                    cond_stack.pop()
                continue

            if line.startswith("else"):
                if cond_stack:
                    cond_stack[-1].in_true = not cond_stack[-1].in_true
                stripped = line[4:].strip()
                if stripped:
                    line = stripped
                else:
                    continue

            if line.startswith("if"):
                cond = parse_condition(line)
                if cond:
                    cond_stack.append(cond)
                    continue

            cond_reqs: List[Requirement] = []
            for cond in cond_stack:
                cond_reqs.extend(cond.current_requirements())

            assignment = re.match(r"^([^:=+]+?)\s*([-+:]?=)\s*(.+)$", line)
            if not assignment:
                continue
            varname = assignment.group(1).strip()
            rhs = assignment.group(3).strip()
            var_reqs = [Requirement(cfg, "set") for cfg in configs_in_text(varname)]
            base_reqs = cond_reqs + var_reqs

            tokens = [t for t in rhs.split() if t]
            agg_base, agg_kind = parse_aggregator_var(varname)

            if varname.startswith("obj-"):
                gatings_for_line = combine_gatings(inherited, base_reqs)
                for token in tokens:
                    if "$(" in token:
                        continue
                    is_dir = token.rstrip().endswith("/")
                    cleaned = normalize_token(token)
                    if not cleaned:
                        continue
                    rel_path = (rel_prefix / cleaned).as_posix()
                    if is_dir:
                        subdir_path = (current_dir / cleaned).resolve()
                        existing = dir_gatings.get(subdir_path, [])
                        combined = dedup_gatings(existing + gatings_for_line)
                        if combined != existing:
                            dir_gatings[subdir_path] = combined
                            queue.append(subdir_path)
                        continue
                    if not cleaned.endswith(".o"):
                        continue
                    add_gatings(root_objects, rel_path, gatings_for_line)
                continue

            if agg_base:
                base_object = (rel_prefix / agg_base)
                if not base_object.suffix:
                    base_object = base_object.with_suffix(".o")
                base_object_str = base_object.as_posix()
                for token in tokens:
                    if "$(" in token:
                        continue
                    cleaned = normalize_token(token)
                    if not cleaned.endswith(".o"):
                        continue
                    child = (rel_prefix / cleaned).as_posix()
                    aggregator_children[base_object_str].append((child, list(base_reqs)))
                continue

    return root_objects, aggregator_children


def propagate_objects(
    root_objects: DefaultDict[str, List[List[Requirement]]],
    aggregator_children: DefaultDict[str, List[Tuple[str, List[Requirement]]]],
) -> DefaultDict[str, List[List[Requirement]]]:
    final_gatings: DefaultDict[str, List[List[Requirement]]] = defaultdict(list)
    seen: DefaultDict[str, Set[Tuple[Requirement, ...]]] = defaultdict(set)
    queue: deque[Tuple[str, List[Requirement]]] = deque()
    for obj, gatings in root_objects.items():
        for gating in gatings:
            queue.append((obj, gating))

    while queue:
        obj, gating = queue.popleft()
        key = tuple(sorted(gating, key=lambda r: (r.config, r.relation)))
        if key in seen[obj]:
            continue
        seen[obj].add(key)
        final_gatings[obj].append(list(key))
        for child, extra in aggregator_children.get(obj, []):
            queue.append((child, list(gating) + list(extra)))

    return final_gatings


def evaluate_gatings(gatings: Sequence[Sequence[Requirement]], statuses: Dict[str, str]) -> bool:
    return any(all(req.is_satisfied(statuses) for req in gating) for gating in gatings)


def find_disabled_sources(root: Path, drivers_dir: Path, statuses: Dict[str, str]) -> List[str]:
    root_objects, aggregator_children = collect_mappings(root, drivers_dir, statuses)
    final_gatings = propagate_objects(root_objects, aggregator_children)
    disabled: List[str] = []
    for obj, gatings in final_gatings.items():
        if not gatings:
            continue
        if evaluate_gatings(gatings, statuses):
            continue
        c_path = Path(obj).with_suffix(".c")
        absolute = root / c_path
        if absolute.is_file():
            disabled.append(c_path.as_posix())
    disabled = sorted(set(disabled))
    return disabled


def main() -> None:
    script_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="List driver C sources disabled by CONFIG_=n selections.")
    parser.add_argument(
        "--defconfig",
        type=Path,
        default=script_root / "arch/x86/configs/openeuler_defconfig",
        help="Path to defconfig to inspect.",
    )
    parser.add_argument(
        "--drivers",
        type=Path,
        default=script_root / "drivers",
        help="Path to drivers directory.",
    )
    parser.add_argument(
        "--absolute",
        action="store_true",
        help="Output absolute paths instead of paths relative to the repository root.",
    )
    args = parser.parse_args()

    statuses = parse_defconfig(args.defconfig)
    disabled = find_disabled_sources(script_root, args.drivers, statuses)
    output_paths = disabled
    if args.absolute:
        output_paths = [(script_root / Path(item)).resolve().as_posix() for item in disabled]

    writer = csv.writer(sys.stdout)
    for entry in output_paths:
        writer.writerow([entry])


if __name__ == "__main__":
    main()
