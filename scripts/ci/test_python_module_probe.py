#!/usr/bin/env python3
"""Execute the source-owned module macro with a controlled Python launcher."""

from contextlib import contextmanager
from pathlib import Path
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MACRO = ROOT / "scripts/autoconf/m4/python.m4"


class PythonModuleProbeTests(unittest.TestCase):
    @contextmanager
    def fixture(self, *, missing=False, failing=False, cross=False):
        with tempfile.TemporaryDirectory(prefix="aros-python-probe-") as temporary:
            root = Path(temporary)
            modules = root / "modules"
            package = modules / "owned_probe_package"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text("")
            if not missing:
                (package / "child.py").write_text("print('module-import-only-log')\n")
            launcher = root / "selected python's launcher"
            launcher.write_text("#!/bin/sh\n" + (
                "exit 23\n" if failing else
                f"exec {shlex.quote(str(Path(sys.executable).resolve()))} -S -B \"$@\"\n"))
            launcher.chmod(0o755)
            (root / "configure.ac").write_text(
                "AC_INIT([owned-python-probe], [1])\n"
                f"m4_include([{MACRO}])\n" +
                ("cross_compiling=yes\n" if cross else "") +
                "PC_PYTHON_CHECK_MODULE([owned_probe_package.child],\n"
                "  [AC_MSG_NOTICE([owned module present])],\n"
                "  [AC_MSG_FAILURE([owned module missing])])\nAC_OUTPUT\n")
            environment = {"PATH": os.environ["PATH"], "LC_ALL": "C"}
            subprocess.run([shutil.which("autoconf"), "-o", "configure", "configure.ac"], cwd=root,
                           env=environment, check=True, capture_output=True,
                           text=True, timeout=30)
            environment.update(PYTHON=str(launcher), PYTHONPATH=str(modules))
            yield root, environment

    def configure(self, root, environment):
        return subprocess.run(["/bin/sh", "./configure"], cwd=root,
                              env=environment, capture_output=True,
                              text=True, timeout=15)

    def test_script_interpreter_with_spaces_runs_and_logs_import_output(self):
        with self.fixture() as (root, environment):
            result = self.configure(root, environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("owned module present", result.stdout)
            self.assertNotIn("module-import-only-log", result.stdout)
            self.assertIn("module-import-only-log", (root / "config.log").read_text())

    def test_missing_module_is_not_accepted(self):
        with self.fixture(missing=True) as (root, environment):
            result = self.configure(root, environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("owned module missing", result.stderr)
            self.assertNotIn("owned module present", result.stdout)

    def test_failing_selected_interpreter_does_not_fall_back(self):
        with self.fixture(failing=True) as (root, environment):
            result = self.configure(root, environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("owned module missing", result.stderr)
            self.assertIn("$? = 23", (root / "config.log").read_text())

    def test_build_side_python_still_runs_for_a_cross_target(self):
        with self.fixture(cross=True) as (root, environment):
            result = self.configure(root, environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("owned module present", result.stdout)

    def test_old_shebang_invocation_refuses_the_spaced_interpreter_path(self):
        with self.fixture() as (root, environment):
            self.assertEqual(self.configure(root, environment).returncode, 0)
            probe = root / "legacy-probe"
            probe.write_text(f"#!{environment['PYTHON']}\nimport owned_probe_package.child\n")
            probe.chmod(0o755)
            result = subprocess.run(["/bin/sh", "-c", "./legacy-probe"], cwd=root,
                                    env=environment, capture_output=True,
                                    text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("module-import-only-log", result.stdout)

    @unittest.skipUnless(sys.platform == "darwin", "Darwin nested-script interpreter counter")
    def test_darwin_nested_script_counter_has_no_space_in_interpreter_path(self):
        with self.fixture() as (root, environment):
            launcher = root / "selected-python"
            self.assertNotIn(" ", str(launcher))
            launcher.write_bytes(Path(environment["PYTHON"]).read_bytes())
            launcher.chmod(0o755)
            environment["PYTHON"] = str(launcher)
            self.assertEqual(self.configure(root, environment).returncode, 0)
            probe = root / "legacy-probe"
            probe.write_text(f"#!{launcher}\nimport owned_probe_package.child\n")
            probe.chmod(0o755)
            result = subprocess.run(["/bin/sh", "-c", "./legacy-probe"], cwd=root,
                                    env=environment, capture_output=True,
                                    text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("module-import-only-log", result.stdout)

    def test_generated_source_checks_use_the_same_explicit_interpreter(self):
        text = (ROOT / "configure").read_text()
        modules = re.findall(r'\("\$PYTHON" -c \'import sys\ntry:\n    import ([a-z]+)\n', text)
        self.assertEqual(modules, ["mako", "yaml", "crcmod"])
        self.assertNotIn("ac_fn_py_try_run", text)


if __name__ == "__main__":
    unittest.main()
