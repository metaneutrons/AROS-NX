"""Check actual MetaMake branch selection; optional released-transpiler probe.

No compiler, board, serial port or network access. Set AROS_TRANSPILER_TEST to
an explicitly selected executable to also test the closed product capability.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parents[1]
MMAKE = SOURCE / "wpa_supplicant/mmakefile.src"
MAKE = shutil.which("gmake") or shutil.which("make")
TRANSPILER = os.environ.get("AROS_TRANSPILER_TEST")
PROFILES = (("x86_64", "pc", "i386", ""),
            ("arm", "raspi", "", "hard"),
            ("aarch64", "raspi", "", ""))


def selected_flags(source, arch, diagnostic):
    # Keep the actual GNU make conditionals/assignments. Replace only the two
    # GenMF invocations with a recipe printing their expanded CFLAGS argument.
    prefix = source.split("# The install stamp gates", 1)[0]
    prefix = re.sub(r"^include .*\n", "", prefix, flags=re.M)
    invocation = re.compile(r"^%build_with_configure .*?(?<!\\)\n", re.M | re.S)

    def recipe(match):
        flags = re.findall(r'extracflags="([^"\n]+)"', match.group())
        if len(flags) != 1:
            raise AssertionError("one explicit CFLAGS expression required")
        # Expand through GNU make, without interpolating source into shell text.
        # Like every makefile test, this still requires trusted Make inputs.
        return "print-flags:\n\t$(info " + flags[0] + ")\n\t@:\n"

    fixture, count = invocation.subn(recipe, prefix)
    if count != 2:
        raise AssertionError("expected separate P4 and audited product invocations")
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "Makefile"
        path.write_text(fixture)
        env = {k: v for k, v in os.environ.items()
               if k not in ("MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS", "MAKEOVERRIDES")}
        return subprocess.run(
            [MAKE, "--no-print-directory", "-f", str(path), "print-flags",
             "AROS_TARGET_ARCH=" + arch, "P4_WIFI_ASSOC_DIAGNOSTICS=" + diagnostic,
             "OPTIMIZATION_CFLAGS=-O2"], env=env, check=True,
            capture_output=True, text=True, timeout=10).stdout.strip().split()


@unittest.skipUnless(MAKE, "GNU make unavailable")
class BuildFlags(unittest.TestCase):
    def test_default_and_diagnostic_switch_by_target(self):
        source = MMAKE.read_text()
        for arch in ("pc", "raspi", "opensbi", "esp32p4", "esp32p4-other"):
            for diagnostic in ("", "0", "1", "2"):
                expected = ["-O2", "-MD"]
                if arch == "esp32p4" and diagnostic == "1":
                    expected.append("-DWM_LIFECYCLE_DIAGNOSTICS=1")
                with self.subTest(arch=arch, diagnostic=diagnostic):
                    self.assertEqual(selected_flags(source, arch, diagnostic), expected)

    def test_counterprobe_cannot_silently_drop_p4_diagnostics(self):
        original = MMAKE.read_text()
        mutated = original.replace(" -MD $(WM_DIAGNOSTIC_CFLAGS)", " -MD")
        self.assertNotEqual(original, mutated)
        self.assertNotEqual(selected_flags(mutated, "esp32p4", "1"),
                            selected_flags(original, "esp32p4", "1"))


@unittest.skipUnless(TRANSPILER, "set AROS_TRANSPILER_TEST for release-contract probes")
class ReleasedCapability(unittest.TestCase):
    def probe(self, root, profile):
        cpu, platform, cpu32, float_abi = profile
        output = root / "targets.cmake"
        if output.exists():
            output.unlink()
        result = subprocess.run(
            [TRANSPILER, "--source-dir", str(root), "--output", str(output),
             "--cpu", cpu, "--platform", platform, "--toolchain", "llvm",
             "--cpu32", cpu32, "--use-mmu", "1", "--float-abi", float_abi],
            capture_output=True, text=True, timeout=60)
        return result, output

    def test_product_profiles_and_rejected_old_unconditional_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            destination = root / "workbench/network/WirelessManager"
            shutil.copytree(SOURCE, destination)
            fixture = destination / "wpa_supplicant/mmakefile.src"
            for profile in PROFILES:
                result, output = self.probe(root, profile)
                with self.subTest(profile=profile):
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertTrue(output.is_file())
                    self.assertIn("workbench-network-wirelessmanager", output.read_text())
            # Deliberately restore the incompatible CFLAGS in the product arm.
            source = fixture.read_text()
            source = source.replace('extracflags="$(OPTIMIZATION_CFLAGS) -MD"',
                                    'extracflags="$(OPTIMIZATION_CFLAGS) -MD $(WM_DIAGNOSTIC_CFLAGS)"')
            fixture.write_text(source)
            for profile in PROFILES:
                result, output = self.probe(root, profile)
                with self.subTest(counterprobe=profile):
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("AT0004", result.stderr)
                    self.assertIn("extracflags", result.stderr)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
