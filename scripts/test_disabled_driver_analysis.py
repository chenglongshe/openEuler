from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().with_name("disabled_driver_analysis.py")
MODULE_SPEC = importlib.util.spec_from_file_location("disabled_driver_analysis", MODULE_PATH)
analysis = importlib.util.module_from_spec(MODULE_SPEC)
assert MODULE_SPEC and MODULE_SPEC.loader
sys.modules["disabled_driver_analysis"] = analysis
MODULE_SPEC.loader.exec_module(analysis)  # type: ignore[arg-type]


def write_files(root: Path, files: dict[str, str]) -> None:
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)


class DisabledDriverAnalysisTests(unittest.TestCase):
    def test_parent_config_disables_nested_objects(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            defconfig = root / "defconfig"
            defconfig.write_text(
                "\n".join(
                    [
                        "# CONFIG_FOO is not set",
                        "CONFIG_BAR=y",
                        "CONFIG_DUAL=m",
                    ]
                )
                + "\n"
            )

            write_files(
                root,
                {
                    "drivers/Makefile": "obj-$(CONFIG_FOO) += foo/\nobj-$(CONFIG_DUAL) += shared.o\n",
                    "drivers/foo/Makefile": "obj-$(CONFIG_BAR) += inner.o\ninner-objs := leaf.o\n",
                    "drivers/foo/inner.c": "",
                    "drivers/foo/leaf.c": "",
                    "drivers/shared.c": "",
                },
            )

            statuses = analysis.parse_defconfig(defconfig)
            disabled = analysis.find_disabled_sources(root, root / "drivers", statuses)

            self.assertIn("drivers/foo/inner.c", disabled)
            self.assertIn("drivers/foo/leaf.c", disabled)
            self.assertNotIn("drivers/shared.c", disabled)

    def test_optional_component_guarded_by_disabled_config(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            defconfig = root / "defconfig"
            defconfig.write_text(
                "\n".join(
                    [
                        "CONFIG_FOO=y",
                        "CONFIG_BAR=y",
                        "# CONFIG_EXTRA is not set",
                    ]
                )
                + "\n"
            )

            write_files(
                root,
                {
                    "drivers/Makefile": "obj-$(CONFIG_FOO) += foo/\n",
                    "drivers/foo/Makefile": "obj-$(CONFIG_BAR) += inner.o\ninner-$(CONFIG_EXTRA) += optional.o\n",
                    "drivers/foo/inner.c": "",
                    "drivers/foo/optional.c": "",
                },
            )

            statuses = analysis.parse_defconfig(defconfig)
            disabled = analysis.find_disabled_sources(root, root / "drivers", statuses)

            self.assertIn("drivers/foo/optional.c", disabled)
            self.assertNotIn("drivers/foo/inner.c", disabled)

    def test_multiple_configs_keep_driver_enabled(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            defconfig = root / "defconfig"
            defconfig.write_text(
                "\n".join(
                    [
                        "# CONFIG_ONE is not set",
                        "CONFIG_OTHER=m",
                    ]
                )
                + "\n"
            )

            write_files(
                root,
                {
                    "drivers/Makefile": "obj-$(CONFIG_ONE) += dual.o\nobj-$(CONFIG_OTHER) += dual.o\n",
                    "drivers/dual.c": "",
                },
            )

            statuses = analysis.parse_defconfig(defconfig)
            disabled = analysis.find_disabled_sources(root, root / "drivers", statuses)

            self.assertNotIn("drivers/dual.c", disabled)


if __name__ == "__main__":
    unittest.main()
