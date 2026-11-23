#!/usr/bin/env python3
"""Regression checks for Darwin's selected host make and archive recipe."""

from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def _darwin_host_case(configure_text: str) -> str:
    """Return the Darwin arm of configure's host-specific case statement."""
    default = configure_text.find('aros_host_make="make"')
    if default < 0:
        raise AssertionError('configure is missing the default aros_host_make="make"')
    host = configure_text[default:]
    case = re.search(
        r'(?ms)^case "\$host_os" in\s*$.*?^(    darwin\*\)\s*\n.*?)(?=^    [^\s].*\)\s*$)',
        host,
    )
    if case is None:
        raise AssertionError("configure is missing its Darwin host case")
    return case.group(1)


def _assert_configure_selects_gmake(configure_text: str) -> str:
    branch = _darwin_host_case(configure_text)
    assignments = re.findall(r'(?m)^\s*aros_host_make="([^"]+)"\s*$', branch)
    if assignments != ["gmake"]:
        raise AssertionError('Darwin host case must override aros_host_make with "gmake" and keep it selected')
    return branch


def _mklib_recipe() -> str:
    """Extract only the named make.tmpl recipe; this is not a GenMF parser."""
    lines = (ROOT / "config/make.tmpl").read_text().splitlines()
    start = next((index for index, line in enumerate(lines)
                  if line.startswith("%define mklib_q ")), None)
    if start is None:
        raise AssertionError("config/make.tmpl is missing the mklib_q definition")
    end = next((index for index in range(start + 1, len(lines))
                if lines[index] == "%end"), None)
    if end is None:
        raise AssertionError("config/make.tmpl mklib_q definition has no %end")
    recipe = "\n".join(lines[start + 1:end])
    if "$(file >%(to).ar.rsp,%(from))" not in recipe:
        raise AssertionError("mklib_q must retain its archive response-file recipe line")
    return recipe


def _gnu_make(minimum_major: int = 4) -> str | None:
    """Prefer gmake; allow GNU make as the behavioral-test name on Linux."""
    for name in ("gmake", "make"):
        executable = shutil.which(name)
        if executable is None:
            continue
        result = subprocess.run([executable, "--version"], capture_output=True,
                                text=True, timeout=10, check=False)
        match = re.search(r"(?m)^GNU Make ([0-9]+)(?:\.|\s)", result.stdout)
        if result.returncode == 0 and match and int(match.group(1)) >= minimum_major:
            return executable
    return None


def _archive_pair_supports_response_files(ar: str, ranlib: str) -> bool:
    """Prove the archive pair accepts the response syntax used by mklib_q."""
    try:
        with tempfile.TemporaryDirectory(prefix="aros-archive-rsp-") as temporary:
            fixture = Path(temporary)
            members = [fixture / "first.o", fixture / "second.o"]
            response = fixture / "members.rsp"
            archive = fixture / "libprobe.a"
            for member in members:
                member.write_bytes(b"archive response-file capability probe\n")
            response.write_text("".join(f"{member}\n" for member in members))
            create = subprocess.run(
                [ar, "cr", str(archive), f"@{response}"],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            if create.returncode != 0:
                return False
            index = subprocess.run(
                [ranlib, str(archive)],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            if index.returncode != 0:
                return False
            listing = subprocess.run(
                [ar, "t", str(archive)],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            return (listing.returncode == 0
                    and listing.stdout.splitlines() == [member.name for member in members])
    except (OSError, subprocess.TimeoutExpired):
        return False


def _homebrew_llvm_archive_tools() -> tuple[str, str] | None:
    """Locate keg-only Homebrew LLVM tools that are intentionally absent from PATH."""
    if platform.system() != "Darwin":
        return None
    brew = shutil.which("brew")
    if brew is None:
        return None
    try:
        result = subprocess.run(
            [brew, "--prefix", "llvm"],
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    bindir = Path(result.stdout.strip()) / "bin"
    ar = bindir / "llvm-ar"
    ranlib = bindir / "llvm-ranlib"
    if ar.is_file() and ranlib.is_file():
        return str(ar), str(ranlib)
    return None


def _system_gnu_archive_tools() -> tuple[str, str] | None:
    """Prefer the system GNU pair over unrelated toolchains earlier in PATH."""
    if platform.system() == "Darwin":
        return None
    ar = Path("/usr/bin/ar")
    ranlib = Path("/usr/bin/ranlib")
    if not ar.is_file() or not ranlib.is_file():
        return None
    try:
        ar_version = subprocess.run(
            [str(ar), "--version"], capture_output=True, text=True,
            timeout=10, check=False,
        )
        ranlib_version = subprocess.run(
            [str(ranlib), "--version"], capture_output=True, text=True,
            timeout=10, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if (ar_version.returncode == 0 and ranlib_version.returncode == 0
            and "GNU ar" in ar_version.stdout
            and "GNU ranlib" in ranlib_version.stdout):
        return str(ar), str(ranlib)
    return None


class DarwinHostMakeContractTests(unittest.TestCase):
    def test_source_and_generated_configure_select_gmake_for_darwin(self) -> None:
        source = (ROOT / "configure.in").read_text()
        generated = (ROOT / "configure").read_text()
        source_branch = _assert_configure_selects_gmake(source)
        generated_branch = _assert_configure_selects_gmake(generated)
        self.assertEqual(
            re.findall(r'(?m)^\s*aros_host_make="[^"]+"\s*$', source_branch),
            re.findall(r'(?m)^\s*aros_host_make="[^"]+"\s*$', generated_branch),
            "generated configure must retain the source Darwin host-make selection",
        )

        host_cfg = (ROOT / "config/host.cfg.in").read_text()
        mmake_config = (ROOT / "mmake.config.in").read_text()
        self.assertRegex(host_cfg, r"(?m)^HOST_MAKE\s*:=\s*@aros_host_make@\s*$")
        self.assertRegex(mmake_config, r"(?m)^maketool \$\(HOST_MAKE\)(?:\s|$)")

    def test_reversion_and_later_override_counterprobes_fail(self) -> None:
        for path in (ROOT / "configure.in", ROOT / "configure"):
            original = path.read_text()
            branch = _darwin_host_case(original)
            before_end, terminator, after_end = branch.rpartition("\n        ;;")
            self.assertTrue(terminator, "Darwin arm must have its closing case terminator")
            for changed_branch in (
                branch.replace('aros_host_make="gmake"', 'aros_host_make="make"', 1),
                before_end + '\n        aros_host_make="make"' + terminator + after_end,
            ):
                with self.subTest(configure=path.name, branch=changed_branch != branch):
                    changed = original.replace(branch, changed_branch, 1)
                    self.assertNotEqual(original, changed)
                    with self.assertRaisesRegex(AssertionError, "Darwin host case"):
                        _assert_configure_selects_gmake(changed)


class DarwinHostMakeRecipeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.make = _gnu_make()
        if cls.make is None:
            raise AssertionError("GNU Make 4 or newer is required (install gmake on macOS)")
        cc = shutil.which("cc")
        if cc is None:
            raise AssertionError("a native C compiler is required for the one-object fixture")
        cls.ar, cls.ranlib = cls._archive_tools()
        cls.recipe = _mklib_recipe()
        cls.temporary = tempfile.TemporaryDirectory(prefix="aros-darwin-make-")
        cls.fixture = Path(cls.temporary.name)
        cls.object_file = cls.fixture / "trivial.o"
        subprocess.run(
            [cc, "-c", "-x", "c", "-o", str(cls.object_file), "-"],
            input="int aros_make_fixture(void) { return 42; }\n",
            text=True, capture_output=True, timeout=30, check=True,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_nested_gmake_builds_archive_from_mklib_recipe(self) -> None:
        archive = self.fixture / "libfixture.a"
        result = self._run_nested_recipe(self.make, archive)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        response = Path(f"{archive}.ar.rsp")
        self.assertTrue(response.is_file(), "GNU make did not create the .ar.rsp file")
        self.assertEqual(response.read_text().strip(), str(self.object_file))
        self.assertTrue(archive.is_file(), "nested GNU make did not create the archive")
        members = subprocess.run([self.ar, "t", str(archive)], capture_output=True,
                                 text=True, timeout=10, check=False)
        self.assertEqual(members.returncode, 0, members.stderr)
        self.assertIn("trivial.o", members.stdout.splitlines())

    def test_selected_archive_tools_accept_response_files(self) -> None:
        self.assertTrue(_archive_pair_supports_response_files(self.ar, self.ranlib))

    def test_response_file_probe_rejects_non_archive_tools(self) -> None:
        self.assertFalse(_archive_pair_supports_response_files(sys.executable, sys.executable))

    def test_apple_make_381_fails_before_creating_response_file(self) -> None:
        apple_make = Path("/usr/bin/make")
        if platform.system() != "Darwin" or not apple_make.is_file():
            self.skipTest("Apple make 3.81 is available only on macOS")
        version = subprocess.run([str(apple_make), "--version"], capture_output=True,
                                 text=True, timeout=10, check=False)
        if "GNU Make 3.81" not in version.stdout:
            self.skipTest("/usr/bin/make is not Apple's GNU Make 3.81")
        archive = self.fixture / "libapple-make.a"
        result = self._run_nested_recipe(str(apple_make), archive)
        self.assertNotEqual(result.returncode, 0, "Apple make 3.81 unexpectedly accepted $(file ...)")
        self.assertFalse(Path(f"{archive}.ar.rsp").exists(),
                         "Apple make 3.81 unexpectedly generated the archive response file")

    @staticmethod
    def _archive_tools() -> tuple[str, str]:
        candidates = []
        system_gnu = _system_gnu_archive_tools()
        if system_gnu is not None:
            candidates.append(system_gnu)
        llvm_ar = shutil.which("llvm-ar")
        llvm_ranlib = shutil.which("llvm-ranlib")
        if (llvm_ar and llvm_ranlib
                and Path(llvm_ar).resolve().parent == Path(llvm_ranlib).resolve().parent):
            candidates.append((llvm_ar, llvm_ranlib))
        homebrew = _homebrew_llvm_archive_tools()
        if homebrew is not None and homebrew not in candidates:
            candidates.append(homebrew)
        ar, ranlib = shutil.which("ar"), shutil.which("ranlib")
        if ar and ranlib:
            candidates.append((ar, ranlib))
        for candidate in candidates:
            if _archive_pair_supports_response_files(*candidate):
                return candidate
        raise AssertionError("response-file-capable llvm-ar/llvm-ranlib or GNU ar/ranlib is required")

    def _run_nested_recipe(self, host_make: str, archive: Path) -> subprocess.CompletedProcess[str]:
        substitutions = {
            "ar": "$(AR)",
            "ranlib": "$(RANLIB)",
            "to": "$(LIBRARY)",
            "from": "$(OBJECT)",
        }
        body = re.sub(
            r"%\((ar|ranlib|to|from)\)",
            lambda match: substitutions[match.group(1)],
            self.recipe,
        )
        inner = self.fixture / f"{archive.stem}.inner.mk"
        inner.write_text(
            # configure.in passes its selected ar executable with the native
            # archive-creation flags in the AR variable.
            f"AR := {self.ar} cr\n"
            f"RANLIB := {self.ranlib}\n"
            f"LIBRARY := {archive}\n"
            f"OBJECT := {self.object_file}\n"
            f"TARGETDIR := {self.fixture}\n"
            "Q := @\n"
            "ECHO := echo\n"
            # Keep the response file so the test can observe the $(file ...) output.
            "RM := :\n"
            "$(LIBRARY): $(OBJECT)\n"
            f"{body}\n"
        )
        outer = self.fixture / f"{archive.stem}.outer.mk"
        outer.write_text(
            f"HOST_MAKE := {host_make}\n"
            f"INNER_MAKEFILE := {inner}\n"
            f"LIBRARY := {archive}\n"
            ".PHONY: all\n"
            "all:\n"
            "\t$(HOST_MAKE) --no-print-directory -f $(INNER_MAKEFILE) $(LIBRARY)\n"
        )
        return subprocess.run([self.make, "--no-print-directory", "-f", str(outer), "all"],
                              capture_output=True, text=True, timeout=30, check=False)


if __name__ == "__main__":
    unittest.main()
