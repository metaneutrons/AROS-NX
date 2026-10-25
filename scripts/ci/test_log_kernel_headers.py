"""Compile the log kernels in a translation unit owning its own source ID."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MATH = ROOT / "compiler/crt/stdc/math"


class LogKernelHeaderTests(unittest.TestCase):
    def compile_header(self, header, function, *, replacement=None, lint=False):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            content = (MATH / header).read_text()
            if replacement is not None:
                content = replacement(content)
            (temporary / header).write_text(content)
            source = temporary / "consumer.c"
            source.write_text(
                'static char rcsid[] = "including translation unit";\n'
                f'#include "{header}"\n'
                'int main(void) {\n'
                f'  return rcsid[0] != \'i\' || {function}(0.0) != 0.0\n'
                f'    || !({function}(0.25) > 0.0);\n'
                '}\n'
            )
            executable = temporary / "consumer"
            command = shlex.split(os.environ.get("CC", "cc"))
            command += ["-std=c99", "-Wall", "-Wextra", "-Werror"]
            if lint:
                command += ["-Dlint"]
            result = subprocess.run(
                command + [str(source), "-o", str(executable)],
                capture_output=True, text=True, timeout=30,
            )
            if result.returncode == 0:
                executed = subprocess.run([str(executable)], timeout=10)
                self.assertEqual(executed.returncode, 0)
            return result

    def test_headers_coexist_with_callers_source_identifier(self):
        for header, function in (("k_log.h", "k_log1p"), ("k_logf.h", "k_log1pf")):
            for lint in (False, True):
                with self.subTest(header=header, lint=lint):
                    result = self.compile_header(header, function, lint=lint)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_original_malformed_and_syntax_only_repairs_are_rejected(self):
        for header, function in (("k_log.h", "k_log1p"), ("k_logf.h", "k_log1pf")):
            for malformed in (True, False):
                with self.subTest(header=header, malformed=malformed):
                    def restore_identifier(content):
                        lines = content.splitlines(keepends=True)
                        matches = [index for index, line in enumerate(lines)
                                   if line.startswith("/* $FreeBSD: src/lib/msun/src/k_log")]
                        self.assertEqual(len(matches), 1)
                        index = matches[0]
                        identifier = lines[index].strip()[3:-3]
                        suffix = ");" if malformed else ";"
                        lines[index] = f'static char rcsid[] = "{identifier}"{suffix}\n'
                        return "".join(lines)

                    result = self.compile_header(header, function, replacement=restore_identifier)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("rcsid", result.stderr)
                    self.assertIn("redefinition", result.stderr.lower())


if __name__ == "__main__":
    unittest.main()
