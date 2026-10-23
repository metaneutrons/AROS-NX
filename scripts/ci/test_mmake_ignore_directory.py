#!/usr/bin/env python3
"""Focused checks for configure's exact MetaMake ignore-directory selector.

The behavioral checks execute only the generated selector shell fragment with
a small error stub. They do not run the full configure dependency probes.
"""

from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
OPTION_MARKER = "# Check whether --with-mmake-ignore-directory was given."
NEXT_SECTION_MARKER = "# Parse the target field into something useful."
EXPECTED_ERRORS = (
    "MetaMake ignore directory must be a normalized relative path",
    "MetaMake ignore directory must not contain dot components",
    "MetaMake exact ignore directory requires a relative path with a slash",
    "MetaMake ignore directory exceeds the configuration line bound",
)
APPEND_ASSIGNMENT = (
    'mmake_ignore_dirs="${mmake_ignore_dirs}${export_newline}ignoredir $withval"'
)


def _extract_source_selector(source: str) -> str:
    start = source.find('mmake_ignore_dirs=""')
    if start < 0:
        raise AssertionError("configure.in is missing the mmake_ignore_dirs reset")
    end = source.find(NEXT_SECTION_MARKER, start)
    if end < 0:
        raise AssertionError("configure.in is missing the selector end marker")
    fragment = source[start:end]
    if "AC_ARG_WITH([mmake-ignore-directory]," not in fragment:
        raise AssertionError("configure.in selector is missing its AC_ARG_WITH registration")
    return fragment


def _extract_generated_selector(configure: str) -> str:
    start = configure.find('mmake_ignore_dirs=""')
    if start < 0:
        raise AssertionError("generated configure is missing the mmake_ignore_dirs reset")
    option = configure.find(OPTION_MARKER, start)
    end = configure.find(NEXT_SECTION_MARKER, option)
    if option < 0 or end < 0:
        raise AssertionError("generated configure is missing the ignore-directory selector block")
    fragment = configure[start:end]
    if APPEND_ASSIGNMENT not in fragment:
        raise AssertionError("generated selector is missing the exact ignoredir append")
    return fragment


def _case_contracts(
    fragment: str, *, normalize_m4_quotes: bool
) -> list[tuple[str, tuple[str, ...]]]:
    cases = re.finditer(
        r"(?ms)^[ \t]*case[ \t]+(.+?)[ \t]+in[ \t]*\n(.*?)^[ \t]*esac[ \t]*$",
        fragment,
    )
    contracts = []
    for case in cases:
        subject = case.group(1).strip()
        arms = []
        for line in case.group(2).splitlines():
            # Case patterns are the only lines that start with a whitespace-
            # free shell glob followed by ')'. Action lines such as
            # AC_MSG_ERROR([...]) also contain ')' and must not become arms.
            match = re.match(r"^([^()\s]+(?:\|[^()\s]+)*)\)(?:[ \t]|$)", line.strip())
            if match:
                arms.append(match.group(1).strip())
        if normalize_m4_quotes:
            subject = subject.replace("[[", "[").replace("]]", "]")
            arms = [arm.replace("[[", "[").replace("]]", "]") for arm in arms]
        contracts.append((subject, tuple(arms)))
    return contracts


def _error_messages(fragment: str, *, generated: bool) -> list[str]:
    if generated:
        return re.findall(r'as_fn_error \$\? "([^"]+)"', fragment)
    return re.findall(r"AC_MSG_ERROR\(\[([^\]]+)\]\)", fragment)


def _run_generated_selector(
    selector: str,
    *,
    source_directory: str,
    build_directory: str,
    option: str | None = None,
    inherited_mmake_ignore_dirs: str = "stale ignoredir state",
) -> subprocess.CompletedProcess[str]:
    prelude = """\
as_fn_error() {
    printf '%s\\n' "$2" >&2
    exit 81
}
"""
    script = prelude + selector + '\nprintf \'%s\' "$mmake_ignore_dirs"\n'
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "AROS_BUILDDIR": build_directory,
        "SRCDIR": source_directory,
        "export_newline": "\n",
        "mmake_ignore_dirs": inherited_mmake_ignore_dirs,
    }
    if option is not None:
        environment["with_mmake_ignore_directory"] = option

    with tempfile.TemporaryDirectory(prefix="aros-mmake-ignore-") as working_directory:
        return subprocess.run(
            ["/bin/sh", "-c", script],
            cwd=working_directory,
            env=environment,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )


class MmakeIgnoreDirectoryConfigureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = (ROOT / "configure.in").read_text(encoding="utf-8")
        cls.configure = (ROOT / "configure").read_text(encoding="utf-8")
        cls.source_selector = _extract_source_selector(cls.source)
        cls.generated_selector = _extract_generated_selector(cls.configure)

    def test_source_and_generated_selector_registration_agree(self) -> None:
        self.assertIn(
            "[AS_HELP_STRING([--with-mmake-ignore-directory=RELATIVE/PATH],",
            self.source_selector,
        )
        self.assertIn("if test ${with_mmake_ignore_directory+y}", self.generated_selector)
        self.assertEqual(
            _case_contracts(self.source_selector, normalize_m4_quotes=True),
            _case_contracts(self.generated_selector, normalize_m4_quotes=False),
            "generated configure validation patterns must match configure.in",
        )
        self.assertEqual(
            _error_messages(self.source_selector, generated=False),
            _error_messages(self.generated_selector, generated=True),
            "generated configure must preserve each source validation reason",
        )
        self.assertEqual(
            re.findall(r'test "\$\{#withval\}" -gt (\d+)', self.source_selector),
            re.findall(r'test "\$\{#withval\}" -gt (\d+)', self.generated_selector),
        )
        self.assertIn(APPEND_ASSIGNMENT, self.source_selector)
        self.assertEqual(self.source_selector.count(APPEND_ASSIGNMENT), 1)
        self.assertEqual(self.generated_selector.count(APPEND_ASSIGNMENT), 1)
        self.assertIn('mmake_ignore_dirs=""', self.source_selector)
        self.assertIn('mmake_ignore_dirs=""', self.generated_selector)

    def test_default_clears_prior_state_and_in_source_ignore_is_preserved(self) -> None:
        default = _run_generated_selector(
            self.generated_selector,
            source_directory="/project/source",
            build_directory="/project/build",
        )
        self.assertEqual(default.returncode, 0, default.stderr)
        self.assertEqual(default.stdout, "")

        in_source = _run_generated_selector(
            self.generated_selector,
            source_directory="/project/source",
            build_directory="/project/source/build/generated",
            option="vendor/cache/metadata",
        )
        self.assertEqual(in_source.returncode, 0, in_source.stderr)
        self.assertEqual(
            in_source.stdout,
            "\nignoredir build/generated\nignoredir vendor/cache/metadata",
        )

    def test_out_of_source_option_appends_one_exact_ignoredir_entry(self) -> None:
        result = _run_generated_selector(
            self.generated_selector,
            source_directory="/project/source",
            build_directory="/project/build",
            option="workbench/tools/generated",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "\nignoredir workbench/tools/generated")

    def test_invalid_values_fail_with_their_generated_reason(self) -> None:
        invalid_values = (
            ("", EXPECTED_ERRORS[0]),
            ("/absolute/path", EXPECTED_ERRORS[0]),
            ("generated//cache", EXPECTED_ERRORS[0]),
            ("generated/cache/", EXPECTED_ERRORS[0]),
            ("generated/bad name", EXPECTED_ERRORS[0]),
            ("generated/./cache", EXPECTED_ERRORS[1]),
            ("generated/../cache", EXPECTED_ERRORS[1]),
            ("generated", EXPECTED_ERRORS[2]),
            ("a" * 243 + "/b", EXPECTED_ERRORS[3]),
        )
        for value, expected_error in invalid_values:
            with self.subTest(value=value):
                result = _run_generated_selector(
                    self.generated_selector,
                    source_directory="/project/source",
                    build_directory="/project/build",
                    option=value,
                )
                self.assertEqual(result.returncode, 81)
                self.assertEqual(result.stderr.strip(), expected_error)

    def test_configure_help_lists_the_exact_directory_option(self) -> None:
        result = subprocess.run(
            ["/bin/sh", str(ROOT / "configure"), "--help"],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        help_text = " ".join(result.stdout.split())
        self.assertIn("--with-mmake-ignore-directory=RELATIVE/PATH", help_text)
        self.assertIn(
            "exclude one exact project-relative generated subtree from MetaMake",
            help_text,
        )


if __name__ == "__main__":
    unittest.main()
