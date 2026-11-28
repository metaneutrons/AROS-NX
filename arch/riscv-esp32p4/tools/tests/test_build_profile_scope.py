"""Exercise the P4 parse-time profile guard through the real GNU Make include."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE_ROOT = Path(__file__).resolve().parents[4]
PORT_ROOT = Path(__file__).resolve().parents[2]
BOARD_MK = PORT_ROOT / "board/board.mk"
PROFILE_MARKER = Path("config/esp32p4-build-profile.json")


def find_make():
    """Use the configured make executable, or GNU Make from PATH."""
    configured = os.environ.get("MAKE")
    executable = shutil.which(configured) if configured else (
        shutil.which("gmake") or shutil.which("make")
    )
    if executable is None:
        raise AssertionError("GNU Make is required (set MAKE or install gmake)")
    version = subprocess.run(
        [executable, "--version"], capture_output=True, text=True, check=True, timeout=10
    )
    identity = re.match(r"GNU Make ([0-9]+)\.", version.stdout)
    if identity is None or int(identity.group(1)) < 4:
        raise AssertionError("the scope regression requires GNU Make 4 or newer")
    return Path(executable).resolve()


class BuildProfileScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.make = find_make()
        cls.python = Path(sys.executable).resolve()

    def run_make(self, top, arch, cpu, python, board="d1001", layout="legacy"):
        env = {
            "PATH": os.environ.get("PATH", os.defpath),
            "LC_ALL": "C",
            "PYTHONNOUSERSITE": "1",
            "PYTHONSAFEPATH": "1",
            "MAKEFLAGS": "",
            "MFLAGS": "",
            "GNUMAKEFLAGS": "",
        }

        return subprocess.run(
            [
                str(self.make),
                "-rR",
                "--no-print-directory",
                "-f",
                str(BOARD_MK),
                "--eval=probe:;@:",
                "probe",
                f"SRCDIR={SOURCE_ROOT}",
                f"TOP={top}",
                f"PYTHON={python}",
                f"AROS_TARGET_ARCH={arch}",
                f"AROS_TARGET_CPU={cpu}",
                f"P4_BOARD={board}",
                f"P4_FLASH_LAYOUT={layout}",
            ],
            cwd=SOURCE_ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )

    def assert_make_succeeded(self, result):
        self.assertEqual(
            0,
            result.returncode,
            msg=f"make failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )

    def test_foreign_targets_do_not_run_checker_or_write_marker(self):
        targets = (
            ("pc", "x86_64"),
            ("raspi", "arm"),
            ("raspi", "aarch64"),
            ("esp32p4", "arm"),
            ("esp32p4", "riscv64"),
            ("raspi", "riscv"),
        )
        with tempfile.TemporaryDirectory(prefix="p4-profile-scope-") as directory:
            fixture = Path(directory)
            missing_python = fixture / "python-must-not-run"
            for index, (arch, cpu) in enumerate(targets):
                with self.subTest(arch=arch, cpu=cpu):
                    top = fixture / f"foreign-{index}"
                    top.mkdir()
                    result = self.run_make(top, arch, cpu, missing_python)
                    self.assert_make_succeeded(result)
                    self.assertFalse((top / PROFILE_MARKER).exists())

    def test_empty_top_inspection_does_not_run_checker(self):
        with tempfile.TemporaryDirectory(prefix="p4-profile-empty-top-") as directory:
            missing_python = Path(directory) / "python-must-not-run"
            result = self.run_make("", "esp32p4", "riscv", missing_python)
            self.assert_make_succeeded(result)

    def test_p4_marker_is_stable_and_rejects_stale_board_or_layout(self):
        with tempfile.TemporaryDirectory(prefix="p4-profile-build-with-spaces-") as directory:
            top = Path(directory) / "private build tree"
            top.mkdir()
            marker = top / PROFILE_MARKER

            result = self.run_make(top, "esp32p4", "riscv", self.python)
            self.assert_make_succeeded(result)
            self.assertEqual(
                {"board": "d1001", "layout": "legacy"},
                json.loads(marker.read_text(encoding="utf-8")),
            )
            original = marker.read_bytes()

            result = self.run_make(top, "esp32p4", "riscv", self.python)
            self.assert_make_succeeded(result)
            self.assertEqual(original, marker.read_bytes())

            result = self.run_make(
                top, "esp32p4", "riscv", self.python, board="jc1060p470c-v2"
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("board/layout changed", result.stderr)
            self.assertEqual(original, marker.read_bytes())

            result = self.run_make(
                top, "esp32p4", "riscv", self.python, layout="ab"
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("board/layout changed", result.stderr)
            self.assertEqual(original, marker.read_bytes())

    def test_unknown_p4_board_still_fails_before_marker_creation(self):
        with tempfile.TemporaryDirectory(prefix="p4-profile-unknown-board-") as directory:
            top = Path(directory) / "build"
            top.mkdir()
            result = self.run_make(
                top, "esp32p4", "riscv", self.python, board="unknown-board"
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Unsupported ESP32-P4 board", result.stderr)
            self.assertFalse((top / PROFILE_MARKER).exists())


if __name__ == "__main__":
    unittest.main()
