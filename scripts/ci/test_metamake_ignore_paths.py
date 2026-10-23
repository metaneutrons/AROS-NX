#!/usr/bin/env python3
"""Compile MetaMake in a private fixture and test ignoredir path behavior."""

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
METAMAKE = ROOT / "tools/MetaMake"
SOURCES = (
    "mmake.c",
    "mem.c",
    "list.c",
    "var.c",
    "dirnode.c",
    "dep.c",
    "project.c",
    "cache.c",
    "io.c",
)
FEATURES = (
    "HAVE_DIRENT_H",
    "HAVE_GETCWD",
    "HAVE_INTTYPES_H",
    "HAVE_NETINET_IN_H",
    "HAVE_STDARG_H",
    "HAVE_STDINT_H",
    "HAVE_STDIO_H",
    "HAVE_STDLIB_H",
    "HAVE_STRERROR",
    "HAVE_STRING_H",
    "HAVE_SYS_STAT_H",
    "HAVE_SYS_TYPES_H",
    "HAVE_UNISTD_H",
    "HAVE_VPRINTF",
    "PROTOTYPES",
    "STDC_HEADERS",
)


def _fixture_config_header() -> str:
    header = (METAMAKE / "config.h.in").read_text(encoding="utf-8")
    for feature in FEATURES:
        header, count = re.subn(
            rf"(?m)^#undef {feature}$", f"#define {feature} 1", header
        )
        if count != 1:
            raise AssertionError(f"config.h.in must define one placeholder for {feature}")
    header, count = re.subn(
        r"(?m)^#undef PACKAGE_VERSION$",
        '#define PACKAGE_VERSION "ignore-path-test"',
        header,
    )
    if count != 1:
        raise AssertionError("config.h.in must define one PACKAGE_VERSION placeholder")
    return header


def _directory_mtimes(root: Path) -> dict[Path, int]:
    result = {Path("."): root.stat().st_mtime_ns}
    for path in root.rglob("*"):
        if path.is_dir() and not path.is_symlink():
            result[path.relative_to(root)] = path.stat().st_mtime_ns
    return result


class MetaMakeIgnorePathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise AssertionError("a host C compiler named cc is required")

        temporary = tempfile.TemporaryDirectory(prefix="aros-metamake-ignore-compile-")
        cls.compile_temp = temporary
        cls.compile_root = Path(temporary.name)
        include_dir = cls.compile_root / "include"
        include_dir.mkdir()
        (include_dir / "config.h").write_text(
            _fixture_config_header(), encoding="utf-8"
        )
        cls.executable = cls.compile_root / "mmake"
        command = [
            compiler,
            "-I",
            str(include_dir),
            "-I",
            str(METAMAKE),
            *(str(METAMAKE / source) for source in SOURCES),
            "-o",
            str(cls.executable),
        ]
        result = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.compile_temp.cleanup()

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-metamake-ignore-case-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "selected-source-top"
        self.build = self.root / "separate-build-top"
        self.source.mkdir()
        self.build.mkdir()
        self.config = self.root / "mmake.config"

    @staticmethod
    def _makefile(directory: Path, target: str) -> None:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "mmakefile").write_text(
            f"#MM- {target}:\n", encoding="ascii"
        )

    def _write_config(self, ignoredirs: tuple[str, ...]) -> None:
        lines = [
            "maketool true",
            "defaultmakefilename mmakefile",
            "defaulttarget root-target",
            *(f"ignoredir {path}" for path in ignoredirs),
        ]
        self.config.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _run(self, target: str, *, debug: bool = False) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["MMAKE_CONFIG"] = str(self.config)
        environment["HOME"] = str(self.root)
        environment.pop("_MMAKE_TARGET", None)
        command = [
            str(self.executable),
            f"--srcdir={self.source}",
            f"--builddir={self.build}",
        ]
        if debug:
            command.append("--debug")
        command.append(f"default.{target}")
        return subprocess.run(
            command,
            cwd=self.build,
            env=environment,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def _assert_target(self, target: str, present: bool) -> subprocess.CompletedProcess[str]:
        result = self._run(target)
        expected = 0 if present else 20
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def test_exact_paths_cache_changes_custom_top_and_legacy_names(self) -> None:
        self._makefile(self.source / "build/arm-raspi", "exact-target")
        self._makefile(self.source / "build/arm-raspi-extra", "sibling-target")
        self._makefile(
            self.source / "thirdparty/build/arm-raspi", "nested-basename-target"
        )
        self._makefile(self.source / "vendor/CVS", "legacy-cvs-target")
        mtimes = _directory_mtimes(self.source)

        # First populate the cache while the exact path is still included.
        self._write_config(("CVS",))
        self._assert_target("exact-target", True)
        self._assert_target("legacy-cvs-target", False)

        # The config is outside the selected source top, so these changes do
        # not alter any scanned directory timestamp. The cache manifest must
        # nevertheless discard the old directory tree.
        self._write_config(("build/arm-raspi", "CVS"))
        self._assert_target("exact-target", False)
        self._assert_target("sibling-target", True)
        self._assert_target("nested-basename-target", True)
        self._assert_target("legacy-cvs-target", False)
        self.assertEqual(_directory_mtimes(self.source), mtimes)

        self._write_config(("CVS",))
        self._assert_target("exact-target", True)
        self._assert_target("sibling-target", True)
        self._assert_target("nested-basename-target", True)
        self._assert_target("legacy-cvs-target", False)
        self.assertEqual(_directory_mtimes(self.source), mtimes)

    @unittest.skipUnless(os.name == "posix", "the alias probe requires POSIX symlinks")
    def test_path_ignore_is_lexical_and_does_not_follow_symlink_aliases(self) -> None:
        target = self.source / "physical/target"
        self._makefile(target, "symlink-target")
        os.symlink(target, self.source / "alias")
        self._write_config(("physical/target",))

        result = self._run("symlink-target", debug=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[MMAKE] scandirnode(): scanning alias", result.stdout)
        self.assertNotIn(
            "[MMAKE] scandirnode(): scanning physical/target", result.stdout
        )

    def test_ignored_directories_precede_makefile_name_matching(self) -> None:
        self._makefile(self.source, "root-target")
        self._makefile(
            self.source / "build/mmakefile", "ignored-mmakefile-directory-target"
        )
        self._makefile(
            self.source / "build/mmakefile.src", "ignored-src-directory-target"
        )
        self._makefile(self.source / "ordinary", "regular-file-counter-target")
        self._write_config(
            (
                "build/mmakefile",
                "build/mmakefile.src",
                "ordinary/mmakefile",
            )
        )

        self._assert_target("root-target", True)
        self._assert_target("regular-file-counter-target", True)
        self._assert_target("ignored-mmakefile-directory-target", False)
        self._assert_target("ignored-src-directory-target", False)

    def test_invalid_slash_paths_are_rejected_during_config_load(self) -> None:
        for invalid in (
            "",
            "/absolute/path",
            "./relative/path",
            "relative/./path",
            "relative/../path",
            "relative//path",
            "relative/path/",
            "C:/absolute/path",
            r"relative\path/child",
        ):
            with self.subTest(ignoredir=invalid):
                self._write_config((invalid,))
                result = self._run("unneeded-target")
                self.assertEqual(result.returncode, 10, result.stdout + result.stderr)
                self.assertIn("Invalid ignoredir path", result.stderr)
                self.assertNotIn("Scanning dirs", result.stdout)


if __name__ == "__main__":
    unittest.main()
