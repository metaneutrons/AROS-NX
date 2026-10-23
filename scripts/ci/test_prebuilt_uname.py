#!/usr/bin/env python3
"""Keep generated uname wrappers outside an installed compiler input."""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def selection(text):
    found = re.findall(
        r'(?m)^aros_uname_dir=\$\{aros_tools_dir\}\n'
        r'if test "x\$crosstools" = "xyes"; then\n'
        r'    aros_uname_dir=\$\{AROS_CROSSTOOLSDIR\}\nfi$', text)
    if len(found) != 1:
        raise AssertionError("expected one mode-dependent uname directory selection")
    return found[0]


def snapshot(directory):
    return {str(path.relative_to(directory)): (
        path.stat().st_ino, path.stat().st_mode, path.stat().st_size,
        path.stat().st_mtime_ns, path.stat().st_ctime_ns,
        path.read_bytes() if path.is_file() else None)
        for path in [directory, *sorted(directory.rglob("*"))]}


class PrebuiltUnameTests(unittest.TestCase):
    def test_source_and_generated_registration_agree(self):
        for name in ("configure.in", "configure"):
            text = (ROOT / name).read_text()
            selection(text)
            self.assertIn('${aros_uname_dir}/${aros_target_cpu}-aros-uname:scripts/uname.in', text)
            self.assertIn('chmod a+x ${aros_uname_dir}/${aros_target_cpu}-aros-uname', text)
            self.assertIn('aros_uname_dir=${aros_uname_dir}', text)
            self.assertNotIn('${AROS_CROSSTOOLSDIR}/${aros_target_cpu}-aros-uname', text)
        make = (ROOT / "config/make.cfg.in").read_text()
        self.assertRegex(make, r'PATH\s*:= \$\(AROS_BUILD_TOOLDIR\):\$\(AROS_BUILD_CROSSTOOLSDIR\):')

    def exercise(self, selector, mode):
        with tempfile.TemporaryDirectory(prefix="aros-uname-prefix-") as temporary:
            root = Path(temporary)
            compiler, tools = root / "compiler", root / "build-tools"
            compiler.mkdir()
            tools.mkdir()
            installed = compiler / "aarch64-aros-uname"
            installed.write_text("original compiler input\n")
            installed.chmod(0o755)
            (tools / "genmf.py").write_text("fixture\n")
            before = snapshot(compiler)
            # Execute the actual selector, output destination and chmod recipe.
            script = selector + '\n' + (
                'printf "generated wrapper\\n" > "${aros_uname_dir}/${aros_target_cpu}-aros-uname"\n'
                'chmod a+x "${aros_uname_dir}/${aros_target_cpu}-aros-uname"\n'
                'chmod a+x "${aros_tools_dir}/genmf.py"\n')
            subprocess.run(["/bin/sh", "-eu", "-c", script], check=True, timeout=10,
                           env={"PATH": "/usr/bin:/bin", "crosstools": mode,
                                "aros_tools_dir": str(tools),
                                "AROS_CROSSTOOLSDIR": str(compiler),
                                "aros_target_cpu": "aarch64"})
            if mode == "no":
                self.assertEqual(before, snapshot(compiler), "installed prefix changed")
                generated = tools / "aarch64-aros-uname"
            else:
                self.assertNotEqual(before, snapshot(compiler))
                generated = installed
                self.assertFalse((tools / generated.name).exists())
            self.assertEqual(generated.read_text(), "generated wrapper\n")
            self.assertTrue(os.access(generated, os.X_OK))

    def test_prebuilt_prefix_is_unchanged_in_both_configure_forms(self):
        for name in ("configure.in", "configure"):
            with self.subTest(name=name):
                self.exercise(selection((ROOT / name).read_text()), "no")

    def test_compiler_build_still_installs_packaged_uname(self):
        for name in ("configure.in", "configure"):
            with self.subTest(name=name):
                self.exercise(selection((ROOT / name).read_text()), "yes")

    def test_old_unconditional_destination_counterprobe_is_detected(self):
        with self.assertRaisesRegex(AssertionError, "installed prefix changed"):
            self.exercise('aros_uname_dir=${AROS_CROSSTOOLSDIR}', "no")


if __name__ == "__main__":
    unittest.main()
