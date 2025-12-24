This directory contains a pre-computed list of `CONFIG` options that are explicitly disabled (`=n`) in `openeuler_defconfig`.

The file `openeuler_defconfig_config_n_list.txt` was generated with:

```bash
grep '^# CONFIG' arch/x86/configs/openeuler_defconfig | sed 's/^# //; s/ is not set$/=n/'
```

Regenerate the list with the same command if `openeuler_defconfig` changes.
