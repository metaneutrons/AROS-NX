#!/usr/bin/env python3
"""Check libinit's literal compile recipes through GenMF and GNU make.

This is a source-level command-line fixture.  It dry-runs real kickentry and
libentry targets; it does not invoke a compiler or qualify a native SDK.
"""

from collections import Counter
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "compiler/libinit/mmakefile.src"
ISA_FLAGS = (
    "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc",
    "-mabi=ilp32f",
    "-mcmodel=medany",
)
TARGET_FLAGS = ("-O2", "-fno-common", "-DLIBINIT_USER_FLAG=7")
EXPECTED_FLAGS = ISA_FLAGS + TARGET_FLAGS
TARGETS = ("libinit-kickentry", "libinit-libentry")


def _gnu_make() -> str:
    for name in ("gmake", "make"):
        executable = shutil.which(name)
        if executable is None:
            continue
        version = subprocess.run([executable, "--version"], capture_output=True,
                                 text=True, timeout=10, check=False)
        match = re.search(r"(?m)^GNU Make ([0-9]+)(?:\.|\s)", version.stdout)
        if version.returncode == 0 and match and int(match.group(1)) >= 4:
            return executable
    raise AssertionError("GNU Make 4 or newer is required (install gmake on macOS)")


class LibinitCFlagsTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-libinit-cflags-")
        self.addCleanup(temporary.cleanup)
        self.fixture = Path(temporary.name)
        (self.fixture / "config").mkdir()
        (self.fixture / "config/make.cfg").touch()
        self.source_root = self.fixture / "source"
        (self.source_root / "config").mkdir(parents=True)
        (self.source_root / "config/aros.cfg").symlink_to(ROOT / "config/aros.cfg")
        (self.source_root / "config/make.tail").symlink_to(ROOT / "config/make.tail")
        (self.source_root / "compiler").mkdir()
        (self.source_root / "compiler/libinit").symlink_to(ROOT / "compiler/libinit")
        (self.source_root / "tools/genmodule").mkdir(parents=True)
        (self.source_root / "tools/genmodule/Makefile.deps").touch()
        self.make = _gnu_make()
        # This path intentionally does not exist.  GNU make -n must print it,
        # never try to execute a compiler.
        self.compiler = self.fixture / "compiler-must-not-run"

    def generate(self, source: Path, label: str) -> Path:
        output = self.fixture / f"{label}.mk"
        subprocess.run(
            [sys.executable, str(ROOT / "tools/genmf/genmf.py"),
             str(ROOT / "config/make.tmpl"), str(source), str(output)],
            check=True, capture_output=True, text=True, timeout=30,
        )
        return output

    def compile_command(self, source: Path, target: str, label: str,
                        isa_flags: tuple[str, ...] = ISA_FLAGS,
                        cflags_override: str | None = None) -> list[str]:
        generated = self.generate(source, label)
        cflags = ("$(strip $(TARGET_ISA_CFLAGS) $(TARGET_CFLAGS) $(LLVM_NOWARN))"
                  if cflags_override is None else cflags_override)
        result = subprocess.run(
            [self.make, "--no-print-directory", "-n", "-f", str(generated), target,
             f"TOP={self.fixture}", f"SRCDIR={self.source_root}",
             "CURDIR=compiler/libinit", f"GENDIR={self.fixture / 'generated'}",
             f"TARGET_CC={self.compiler}",
             f"TARGET_SYSROOT=--sysroot={self.fixture / 'sysroot'}",
             f"TARGET_ISA_CFLAGS={' '.join(isa_flags)}",
             f"TARGET_CFLAGS={' '.join(TARGET_FLAGS)}",
             "LLVM_NOWARN=",
             # The default mirrors config/make.cfg.in:288.  Passing an explicit
             # value also exercises callers that replace the aggregate.
             f"CFLAGS={cflags}",
             "ECHO=echo", "Q="],
            check=False, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.compiler.exists(), "dry-run unexpectedly executed a compiler")
        command_lines = [line for line in result.stdout.splitlines()
                         if line.startswith(f"{self.compiler} ") and " -c " in line]
        self.assertEqual(len(command_lines), 1, result.stdout)
        return shlex.split(command_lines[0])

    def assert_flags_once(self, command: list[str], expected: tuple[str, ...]) -> None:
        counts = Counter(command)
        for flag in expected:
            self.assertEqual(counts[flag], 1,
                             f"{flag} appears {counts[flag]} times; expected exactly once")

    def test_make_cfg_aggregates_isa_and_target_cflags(self) -> None:
        make_cfg = (ROOT / "config/make.cfg.in").read_text()
        self.assertRegex(
            make_cfg,
            r"(?m)^CFLAGS\s*=\s*\$\(strip \$\(TARGET_ISA_CFLAGS\) "
            r"\$\(TARGET_CFLAGS\) \$\(LLVM_NOWARN\)\)$",
        )

    def test_real_libinit_targets_pass_each_selected_flag_once(self) -> None:
        for target in TARGETS:
            with self.subTest(target=target):
                command = self.compile_command(SOURCE, target, target)
                self.assert_flags_once(command, EXPECTED_FLAGS)

    def test_direct_cflags_override_keeps_required_and_override_flags(self) -> None:
        override_flags = ("-fwrapv", "-DLIBINIT_DIRECT_CFLAGS=1")
        for target in TARGETS:
            with self.subTest(target=target):
                command = self.compile_command(
                    SOURCE, target, f"{target}-direct-cflags",
                    cflags_override=" ".join(override_flags),
                )
                self.assert_flags_once(command, EXPECTED_FLAGS + override_flags)

    def test_empty_cflags_still_keeps_required_flags(self) -> None:
        for target in TARGETS:
            with self.subTest(target=target):
                command = self.compile_command(
                    SOURCE, target, f"{target}-empty-cflags", cflags_override="",
                )
                self.assert_flags_once(command, EXPECTED_FLAGS)

    def test_old_literal_prefix_counterprobe_rejects_duplicate_flags(self) -> None:
        source_text = SOURCE.read_text()
        current = ("@$(TARGET_CC) $(TARGET_SYSROOT) -c $(TARGET_ISA_CFLAGS) "
                   "$(TARGET_CFLAGS) $(filter-out $(TARGET_ISA_CFLAGS) "
                   "$(TARGET_CFLAGS),$(CFLAGS)) $< -o $@")
        old = ("@$(TARGET_CC) $(TARGET_SYSROOT) -c $(TARGET_ISA_CFLAGS) "
               "$(TARGET_CFLAGS) $(CFLAGS) $< -o $@")
        self.assertEqual(source_text.count(current), 2)
        mutated = self.fixture / "mmakefile-old-prefix.mmakesrc"
        mutated.write_text(source_text.replace(current, old))
        command = self.compile_command(mutated, "libinit-kickentry", "old-prefix")
        with self.assertRaisesRegex(AssertionError, r"-march=rv32.*appears 2 times"):
            self.assert_flags_once(command, EXPECTED_FLAGS)

    def test_missing_abi_counterprobe_is_rejected(self) -> None:
        command = self.compile_command(
            SOURCE, "libinit-kickentry", "missing-abi",
            tuple(flag for flag in ISA_FLAGS if not flag.startswith("-mabi=")),
        )
        with self.assertRaisesRegex(AssertionError, r"-mabi=ilp32f appears 0 times"):
            self.assert_flags_once(command, EXPECTED_FLAGS)


if __name__ == "__main__":
    unittest.main()
