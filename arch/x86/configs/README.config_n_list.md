This directory contains a pre-computed list of `CONFIG` options that are explicitly disabled (`=n`) in `openeuler_defconfig`.

The file `openeuler_defconfig_config_n_list.txt` was generated with:

```bash
grep '^# CONFIG' arch/x86/configs/openeuler_defconfig | sed 's/^# //; s/ is not set$/=n/'
```

Regenerate the list with the same command if `openeuler_defconfig` changes.

`openeuler_defconfig_config_n_file_map.txt` lists, for each disabled `CONFIG`, the source files in this tree that reference it. It was generated with:

```bash
python - <<'PY'
import os, re
from pathlib import Path
root = Path.cwd()  # run from repo root
defconfig = root / 'arch/x86/configs/openeuler_defconfig'
disabled = set()
for line in defconfig.read_text().splitlines():
    if line.startswith('# CONFIG') and 'is not set' in line:
        disabled.add(line.split()[1])
regex = re.compile(r'\bCONFIG_[A-Za-z0-9_]+\b')
mapping = {k: [] for k in disabled}
skip_dirs = {'.git', '.github', 'out', 'build', 'tmp'}
def skip_dir(name: str) -> bool:
    return name in skip_dirs or name.startswith('.venv')
out_file = root / 'arch/x86/configs/openeuler_defconfig_config_n_file_map.txt'
for dirpath, dirnames, filenames in os.walk(root, topdown=True):
    if skip_dir(Path(dirpath).name):
        dirnames.clear()
        continue
    dirnames[:] = [d for d in dirnames if not skip_dir(d)]
    for fname in filenames:
        path = Path(dirpath) / fname
        try:
            if path.is_symlink():
                continue
            text = path.read_text(errors='replace')
        except (UnicodeDecodeError, OSError, PermissionError):
            continue
        common = set(regex.findall(text)) & disabled
        if not common:
            continue
        rel = path.relative_to(root)
        for conf in common:
            mapping[conf].append(str(rel))
with open(out_file, 'w') as out:
    for conf in sorted(disabled):
        files = mapping[conf]
        out.write(f"{conf}:\n")
        if files:
            for p in sorted(set(files)):
                out.write(f"  - {p}\n")
        else:
            out.write("  - <no references found>\n")
PY
```
