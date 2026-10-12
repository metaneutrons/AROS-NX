#!/usr/bin/env python3
"""Exercise SOURCE_DATE_EPOCH through the real host-built genmodule."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENMODULE_DIR = ROOT / "tools/genmodule"
BASE_EPOCH = "946684800"  # 2000-01-01 00:00:00 UTC


class GenmoduleEpochTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise RuntimeError("host cc is required to build genmodule")

        dependencies = (GENMODULE_DIR / "Makefile.deps").read_text(encoding="utf-8")
        try:
            source_block = dependencies.split("GENMODULE_SRCS :=", 1)[1]
            source_block = source_block.split("GENMODULE_INCS :=", 1)[0]
        except IndexError as error:
            raise RuntimeError("Makefile.deps must declare GENMODULE_SRCS and GENMODULE_INCS") from error
        sources = [item for item in source_block.replace("\\", " ").split()
                   if item.endswith(".c")]
        if not sources:
            raise RuntimeError("Makefile.deps contains no genmodule C sources")

        temporary = tempfile.TemporaryDirectory(prefix="aros-genmodule-epoch-build-")
        cls.addClassCleanup(temporary.cleanup)
        cls.build_dir = Path(temporary.name)
        cls.genmodule = cls.build_dir / "genmodule"
        command = [compiler, f"-I{GENMODULE_DIR}"]
        command.extend(str(GENMODULE_DIR / source) for source in sources)
        command.extend(["-o", str(cls.genmodule)])
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(f"could not build genmodule with host cc:\n{result.stderr}")

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-genmodule-epoch-test-")
        self.addCleanup(temporary.cleanup)
        self.fixture = Path(temporary.name)

    def write_config(self, name: str = "epochtest", explicit_date: str | None = None) -> Path:
        lines = ["##begin config", "basename EpochTest", "version 1.0"]
        if explicit_date is not None:
            lines.append(f"date {explicit_date}")
        lines.extend(["##end config", ""])
        config = self.fixture / f"{name}.conf"
        config.write_text("\n".join(lines), encoding="utf-8")
        return config

    def output_dir(self, name: str) -> Path:
        output = self.fixture / name
        output.mkdir()
        for directory in ("proto", "clib", "defines", "inline"):
            (output / directory).mkdir()
        return output

    def invoke(self, config: Path, output: Path, command: str,
               epoch: str | None = None, timezone: str | None = None) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment.pop("SOURCE_DATE_EPOCH", None)
        if epoch is not None:
            environment["SOURCE_DATE_EPOCH"] = epoch
        if timezone is not None:
            environment["TZ"] = timezone
        return subprocess.run(
            [str(self.genmodule), "-c", str(config), "-d", str(output),
             command, "epochtest", "library"],
            cwd=output,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
        )

    def generate_all(self, config: Path, output: Path, epoch: str | None = None,
                     timezone: str | None = None) -> dict[str, bytes]:
        for command in ("writeincludes", "writelibdefs"):
            result = self.invoke(config, output, command, epoch, timezone)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "")
        return {
            path.relative_to(output).as_posix(): path.read_bytes()
            for path in sorted(output.rglob("*")) if path.is_file()
        }

    def assert_generated_inventory(self, generated: dict[str, bytes]) -> None:
        self.assertEqual(set(generated), {
            "clib/epochtest_protos.h",
            "defines/epochtest.h",
            "defines/epochtest_LVO.h",
            "epochtest_libdefs.h",
            "inline/epochtest.h",
            "proto/epochtest.h",
        })

    def module_date(self, libdefs: bytes) -> bytes:
        date = re.search(rb'#define MOD_DATE_STRING\s+"([^"]+)"', libdefs)
        self.assertIsNotNone(date)
        return date.group(1)

    def test_explicit_epoch_makes_headers_and_libdefs_identical_across_timezones(self) -> None:
        config = self.write_config()
        timezones = ("UTC", "US/Pacific", "Pacific/Kiritimati", "America/Los_Angeles")
        outputs = []
        for index, timezone in enumerate(timezones):
            with self.subTest(timezone=timezone):
                output = self.output_dir(f"timezone-{index}")
                generated = self.generate_all(config, output, BASE_EPOCH, timezone)
                self.assert_generated_inventory(generated)
                outputs.append(generated)

        for filename, contents in outputs[0].items():
            if filename.endswith(".h") and filename != "epochtest_libdefs.h":
                self.assertIn(b"Copyright (C) 1995-2000", contents, filename)
        self.assertEqual(self.module_date(outputs[0]["epochtest_libdefs.h"]), b"1.1.2000")
        for generated in outputs[1:]:
            self.assertEqual(generated, outputs[0])

    def test_changed_epoch_changes_banner_year_and_default_module_date(self) -> None:
        config = self.write_config()
        epoch_2000 = self.generate_all(config, self.output_dir("year-2000"), BASE_EPOCH, "UTC")
        epoch_2001 = self.generate_all(config, self.output_dir("year-2001"), "1000000000", "UTC")

        self.assertIn(b"Copyright (C) 1995-2000", epoch_2000["proto/epochtest.h"])
        self.assertIn(b"Copyright (C) 1995-2001", epoch_2001["proto/epochtest.h"])
        self.assertEqual(self.module_date(epoch_2000["epochtest_libdefs.h"]), b"1.1.2000")
        self.assertEqual(self.module_date(epoch_2001["epochtest_libdefs.h"]), b"9.9.2001")
        self.assertNotEqual(epoch_2000["proto/epochtest.h"], epoch_2001["proto/epochtest.h"])
        self.assertNotEqual(epoch_2000["epochtest_libdefs.h"], epoch_2001["epochtest_libdefs.h"])

    def test_zero_epoch_is_supported(self) -> None:
        generated = self.generate_all(
            self.write_config(), self.output_dir("zero"), "0", "UTC"
        )
        self.assertIn(b"Copyright (C) 1995-1970", generated["proto/epochtest.h"])
        self.assertEqual(self.module_date(generated["epochtest_libdefs.h"]), b"1.1.1970")

    def test_leading_zeroes_preserve_the_selected_epoch(self) -> None:
        config = self.write_config()
        ordinary = self.generate_all(config, self.output_dir("ordinary"), BASE_EPOCH, "UTC")
        padded = self.generate_all(config, self.output_dir("padded"), "000" + BASE_EPOCH, "UTC")
        self.assertEqual(padded, ordinary)

    def test_four_digit_calendar_boundary_is_supported_on_this_host(self) -> None:
        generated = self.generate_all(
            self.write_config(), self.output_dir("calendar-boundary"),
            "253402300799", "Pacific/Kiritimati",
        )
        self.assertIn(b"Copyright (C) 1995-9999", generated["proto/epochtest.h"])
        self.assertEqual(self.module_date(generated["epochtest_libdefs.h"]), b"31.12.9999")

    def test_explicit_config_date_is_preserved(self) -> None:
        generated = self.generate_all(
            self.write_config(explicit_date="5.6.1998"),
            self.output_dir("explicit-date"), BASE_EPOCH, "UTC",
        )
        self.assertIn(b"Copyright (C) 1995-2000", generated["proto/epochtest.h"])
        self.assertEqual(self.module_date(generated["epochtest_libdefs.h"]), b"5.6.1998")

    def test_unset_epoch_retains_host_utc_banner_and_local_default_date(self) -> None:
        config = self.write_config()
        before = time.time()
        generated = self.generate_all(config, self.output_dir("unset"))
        after = time.time()

        banner = re.search(rb"Copyright \(C\) 1995-(\d+),", generated["proto/epochtest.h"])
        self.assertIsNotNone(banner)
        self.assertIn(int(banner.group(1)), {time.gmtime(before).tm_year, time.gmtime(after).tm_year})

        date = self.module_date(generated["epochtest_libdefs.h"])
        possible_local_dates = {
            f"{local.tm_mday}.{local.tm_mon}.{local.tm_year}".encode("ascii")
            for local in (time.localtime(before), time.localtime(after))
        }
        self.assertIn(date, possible_local_dates)

    def test_invalid_epochs_fail_before_generating_files_even_with_explicit_date(self) -> None:
        config = self.write_config(explicit_date="5.6.1998")
        invalid_epochs = (
            "",
            "+946684800",
            "-1",
            " 946684800",
            "946684800 ",
            "946684800garbage",
            "18446744073709551616",  # uintmax_t overflow
            "253402300800",  # first second beyond the four-digit UTC year range
        )
        for index, epoch in enumerate(invalid_epochs):
            with self.subTest(epoch=repr(epoch)):
                for command in ("writeincludes", "writelibdefs"):
                    output = self.output_dir(f"invalid-{index}-{command}")
                    result = self.invoke(config, output, command, epoch)
                    self.assertEqual(result.returncode, 20, result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual(
                        result.stderr,
                        "genmodule: invalid or unrepresentable SOURCE_DATE_EPOCH\n",
                    )
                    self.assertEqual(
                        [path for path in output.rglob("*") if path.is_file()], [],
                        "invalid epoch must not leave partial generated files",
                    )


if __name__ == "__main__":
    unittest.main()
