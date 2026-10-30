#!/usr/bin/env python3
"""Behavioral regression tests for shared, concurrently requested fetches."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
FETCH_ONCE = ROOT / "tools/fetch-once.sh"
LLVM_MMAKE = ROOT / "tools/crosstools/llvm/mmakefile.src"


class FetchOnceTests(unittest.TestCase):
    def test_parallel_callers_publish_once(self) -> None:
        with tempfile.TemporaryDirectory(prefix="aros-fetch-once-") as temporary:
            work = Path(temporary)
            marker = work / "source.fetched"
            counter = work / "counter"
            command = work / "fetch.sh"
            command.write_text(
                "#!/bin/sh\n"
                "set -eu\n"
                "sleep 1\n"
                "printf 'fetch\\n' >> \"$1\"\n"
            )
            command.chmod(0o755)

            argv = [str(FETCH_ONCE), str(marker), "--", str(command), str(counter)]
            callers = [subprocess.Popen(argv) for _ in range(3)]
            self.assertEqual([caller.wait(timeout=10) for caller in callers], [0, 0, 0])
            self.assertTrue(marker.is_file())
            self.assertEqual(counter.read_text().splitlines(), ["fetch"])
            self.assertFalse(Path(f"{marker}.lock").exists())

            subprocess.run(argv, check=True, timeout=10)
            self.assertEqual(counter.read_text().splitlines(), ["fetch"])

    def test_failed_fetch_is_retryable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="aros-fetch-once-failure-") as temporary:
            marker = Path(temporary) / "source.fetched"
            failed = subprocess.run(
                [str(FETCH_ONCE), str(marker), "--", "sh", "-c", "exit 7"],
                check=False,
                timeout=10,
            )
            self.assertEqual(failed.returncode, 7)
            self.assertFalse(marker.exists())
            self.assertFalse(Path(f"{marker}.lock").exists())

            subprocess.run(
                [str(FETCH_ONCE), str(marker), "--", "sh", "-c", "exit 0"],
                check=True,
                timeout=10,
            )
            self.assertTrue(marker.is_file())

    def test_llvm_monorepo_enables_fetch_once(self) -> None:
        source = LLVM_MMAKE.read_text()
        declaration = source.split(
            "%fetch mmake=crosstools-llvm-project-fetch", maxsplit=1
        )[1].split("%create_patch", maxsplit=1)[0]
        self.assertIn("patches_specs=$(LLVM_PROJECT_PATCHES_SPECS)", declaration)
        self.assertIn("crosstools-llvm-project-fetch-once := yes", source)
        self.assertIn(
            "crosstools-llvm-project-fetch-once-marker := "
            "$(HOSTDIR)/Ports/host/llvm-project/.$(LLVM_PROJECT_ARCHBASE)-materialized",
            source,
        )

        template = (ROOT / "config/make.tmpl").read_text()
        self.assertIn("ifeq ($(%(mmake)-once),yes)", template)
        self.assertIn(
            "$(SRCDIR)/tools/fetch-once.sh $(%(mmake)-once-marker) --",
            template,
        )

        with tempfile.TemporaryDirectory(prefix="aros-fetch-once-genmf-") as temporary:
            generated = Path(temporary) / "mmakefile"
            subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/genmf/genmf.py"),
                    str(ROOT / "config/make.tmpl"),
                    str(LLVM_MMAKE),
                    str(generated),
                ],
                check=True,
                timeout=30,
            )
            output = generated.read_text()
            self.assertEqual(
                output.count("crosstools-llvm-project-fetch-once := yes"), 1
            )
            rule = output.split(
                "crosstools-llvm-project-fetch :", maxsplit=1
            )[1].split("\n\n", maxsplit=1)[0]
            self.assertIn(
                "ifeq ($(crosstools-llvm-project-fetch-once),yes)", rule
            )
            self.assertIn(
                "$(SRCDIR)/tools/fetch-once.sh "
                "$(crosstools-llvm-project-fetch-once-marker)",
                rule,
            )
            self.assertNotIn("compiler-rt-target-fetch-once := yes", output)
            self.assertNotIn("llvm-runtimes-target-fetch-once := yes", output)

    def test_only_llvm_consumers_seal_fetch_once_helper(self) -> None:
        for profile in ("arm-raspi", "pc-x86_64", "rpi-aarch64"):
            with self.subTest(profile=profile):
                contract = json.loads(
                    (ROOT / f"compiler/sdk/{profile}/consumer-v1.json").read_text()
                )
                paths = {entry["path"] for entry in contract["inputs"]}
                self.assertIn("tools/fetch-once.sh", paths)

        gnu_contract = json.loads(
            (ROOT / "arch/riscv-esp32p4/native-sdk/consumer-v2.json").read_text()
        )
        gnu_paths = {entry["path"] for entry in gnu_contract["inputs"]}
        self.assertNotIn("tools/fetch-once.sh", gnu_paths)


if __name__ == "__main__":
    unittest.main()
