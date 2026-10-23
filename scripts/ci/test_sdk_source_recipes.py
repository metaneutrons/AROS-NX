#!/usr/bin/env python3
"""Exercise source-owned SDK fetch declarations through GenMF and GNU make.

These are authoring regressions, not a native SDK or release qualification.
"""

from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
FETCH_TARGETS = {
    "arch/all-native/acpica/mmakefile.src": "acpica-fetch",
    "compiler/boost/mmakefile.src": "compiler-boost-fetch",
    "compiler/softfloat/mmakefile.src": "linklibs-softfloat-fetch",
    "external/bz2/mmakefile.src": "external-bzip2-fetch",
    "workbench/libs/codesets/mmakefile.src": "workbench-libs-codesets-fetch",
    "workbench/libs/expat/mmakefile.src": "workbench-libs-expat-fetch",
    "workbench/libs/freetype2/mmakefile.src": "freetype2-fetch",
    "workbench/libs/glu/mmakefile.src": "workbench-libs-glu-fetch",
    "workbench/libs/jpeg/mmakefile.src": "jpeg-fetch",
    "workbench/libs/lzma/mmakefile.src": "xz-fetch",
    "workbench/libs/mbedtls/mmakefile.src": "mbedtls-fetch",
    "workbench/libs/tiff/mmakefile.src": "libtiff-fetch",
    "workbench/libs/utf8proc/mmakefile.src": "workbench-libs-utf8proc-fetch",
    "workbench/libs/z/mmakefile.src": "zlib-fetch",
    "workbench/libs/zstd/mmakefile.src": "workbench-libs-zstd-fetch",
}


class SdkSourceRecipeTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-sdk-recipes-")
        self.addCleanup(temporary.cleanup)
        self.fixture = Path(temporary.name)
        (self.fixture / "config").mkdir()
        (self.fixture / "config/make.cfg").touch()
        self.make = shutil.which("gmake") or shutil.which("make")
        self.assertIsNotNone(self.make, "GNU make is required")
        version = subprocess.run([self.make, "--version"], check=True,
                                 capture_output=True, text=True, timeout=10)
        identity = re.match(r"GNU Make ([0-9]+)\.", version.stdout)
        self.assertIsNotNone(identity, "the recipe tests require GNU make, not BSD make")
        self.assertGreaterEqual(int(identity.group(1)), 4, "GNU make 4 or newer is required")

    def generate(self, relative: str) -> Path:
        output = self.fixture / (relative.replace("/", "_") + ".mk")
        subprocess.run(
            [sys.executable, str(ROOT / "tools/genmf/genmf.py"),
             str(ROOT / "config/make.tmpl"), str(ROOT / relative), str(output)],
            check=True, capture_output=True, text=True, timeout=30,
        )
        return output

    def fetch_arguments(self, relative: str, target: str) -> list[str]:
        generated = self.generate(relative)
        result = subprocess.run(
            [self.make, "-n", "-f", str(generated), target,
             f"TOP={self.fixture}", f"SRCDIR={ROOT}",
             f"CURDIR={Path(relative).parent}",
             f"GENDIR={self.fixture}/generated", f"PORTSDIR={self.fixture}/ports",
             f"AROS_LIBRARIES={self.fixture}/libraries",
             f"PORTSSOURCEDIR={self.fixture}/cache", "FETCH=sdk-fetch-probe",
             "ECHO=echo", "IF=if", "TEST=test", "TOUCH=touch", "Q="],
            check=False, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line for line in result.stdout.replace("\\\n", " ").splitlines()
                 if line.startswith("sdk-fetch-probe ")]
        self.assertEqual(len(lines), 1, result.stdout)
        return shlex.split(lines[0])[1:]

    def assert_fetch_pins(self, args: list[str]) -> None:
        archive = args[args.index("-a") + 1]
        suffixes = args[args.index("-s") + 1].split()
        checksums = args[args.index("-cs") + 1].split()
        self.assertTrue(archive)
        self.assertNotIn("$", archive)
        self.assertTrue(suffixes)
        expected_names = {f"{archive}.{suffix}" for suffix in suffixes}
        actual_names = set()
        for checksum in checksums:
            self.assertRegex(checksum, r"^[^=]+\.(?:zip|tar\.(?:gz|xz|bz2))=sha256:[0-9a-f]{64}$")
            actual_names.add(checksum.split("=", 1)[0])
        self.assertEqual(actual_names, expected_names, "every candidate must have an exact pin")
        self.assertEqual(len(checksums), len(actual_names), "duplicate checksum candidate")

    def test_every_sdk_archive_candidate_has_an_expanded_pin(self) -> None:
        for relative, target in FETCH_TARGETS.items():
            with self.subTest(recipe=relative):
                args = self.fetch_arguments(relative, target)
                self.assert_fetch_pins(args)

    def test_missing_candidate_pin_counterprobe(self) -> None:
        args = self.fetch_arguments("workbench/libs/jpeg/mmakefile.src", "jpeg-fetch")
        args[args.index("-cs") + 1] = ""
        with self.assertRaisesRegex(AssertionError, "every candidate must have an exact pin"):
            self.assert_fetch_pins(args)

    def test_unpinned_fallback_counterprobe(self) -> None:
        args = self.fetch_arguments("workbench/libs/jpeg/mmakefile.src", "jpeg-fetch")
        args[args.index("-s") + 1] += " tar.xz"
        with self.assertRaisesRegex(AssertionError, "every candidate must have an exact pin"):
            self.assert_fetch_pins(args)

    def test_zlib_retains_its_exact_canonical_representation(self) -> None:
        args = self.fetch_arguments("workbench/libs/z/mmakefile.src", "zlib-fetch")
        self.assertEqual(args[args.index("--normalization") + 1], "canonical-tar-gzip-v1")
        self.assertEqual(args[args.index("--normalized-size") + 1], "2337668")
        self.assertEqual(args[args.index("-cs") + 1],
                         "zlib.tar.gz=sha256:883d22e0b9aefc31a383c462adacdbd7941e6862559c5b27e59599db8114e783")

    def test_softfloat_zip_pin_survives_genmf_and_make(self) -> None:
        args = self.fetch_arguments("compiler/softfloat/mmakefile.src", "linklibs-softfloat-fetch")
        self.assert_fetch_pins(args)
        self.assertEqual(args[args.index("-cs") + 1],
                         "SoftFloat-3e.zip=sha256:21130ce885d35c1fe73fc1e1bf2244178167e05c6747cad5f450cc991714c746")

    def test_normalization_cache_and_counterprobes(self) -> None:
        subprocess.run(["bash", str(ROOT / "scripts/tests/test-fetch-normalization.sh")],
                       check=True, capture_output=True, text=True, timeout=60)

    def test_private_gsm_header_list_is_explicitly_empty(self) -> None:
        generated = self.generate("workbench/classes/datatypes/wav/gsm/mmakefile.src")
        source = (ROOT / "workbench/classes/datatypes/wav/gsm/mmakefile.src").read_text()
        self.assertIn('%copy_includes includes=""', source)
        self.assertNotIn("$(INCLUDE_FILES)", generated.read_text())

    def assert_codesets_edge(self, generated: str) -> None:
        self.assertRegex(generated,
                         r"(?m)^#MM linklibs-codesets : workbench-libs-codesets-includes$",
                         "Codesets linklib must explicitly depend on its header producer")
        self.assertRegex(generated, r"(?m)^workbench-libs-codesets-includes :")

    def test_codesets_header_prerequisite_survives_genmf(self) -> None:
        generated = self.generate("workbench/libs/codesets/mmakefile.src").read_text()
        self.assert_codesets_edge(generated)
        missing_edge = generated.replace(
            "#MM linklibs-codesets : workbench-libs-codesets-includes", "#MM linklibs-codesets :")
        with self.assertRaisesRegex(AssertionError, "explicitly depend on its header producer"):
            self.assert_codesets_edge(missing_edge)


if __name__ == "__main__":
    unittest.main()
