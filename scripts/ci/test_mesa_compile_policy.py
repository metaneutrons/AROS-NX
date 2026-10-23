"""Check Mesa's source-owned compile policy through GenMF and GNU Make.

These authoring tests do not compile Mesa or qualify either upstream version.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WRAPPERS = ("", "libcompiler", "libglapi", "libmesa", "libmesautil")
LEGACY_POLICY = """\
-include $(GENDIR)/workbench/libs/mesa/mesa3dgl/mesa3dgllibrary.entrypoints
ifneq ($(mesa3dgl_ENTRYPOINTS),)
MESA_COMPILEOPTIONS += $(CFLAGS_FUNCTION_SECTIONS)
endif
"""
READBACK = """\
.PHONY: inspect-mesa-options
inspect-mesa-options:
\t@printf 'C=<%s>\\nCXX=<%s>\\n' '$(USER_CFLAGS)' '$(USER_CXXFLAGS)'
"""


class MesaCompilePolicyTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-mesa-compile-policy-")
        self.addCleanup(temporary.cleanup)
        self.fixture = Path(temporary.name)
        (self.fixture / "config").mkdir()
        (self.fixture / "config/make.cfg").write_text(
            f"GENMODULE := {self.fixture}/unused-genmodule\nECHO := echo\n",
            encoding="utf-8",
        )
        self.make = shutil.which("gmake") or shutil.which("make")
        self.assertIsNotNone(self.make, "GNU Make is required")
        version = self.run_command([self.make, "--version"])
        self.assertTrue(version.startswith("GNU Make "), version)
        self.legacy = (self.fixture / "gen/workbench/libs/mesa/mesa3dgl/"
                       "mesa3dgllibrary.entrypoints")
        self.legacy.parent.mkdir(parents=True)

    def run_command(self, arguments: list[str]) -> str:
        result = subprocess.run(arguments, cwd=self.fixture, capture_output=True,
                                text=True, timeout=30, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def generate(self, wrapper: str, counter: bool = False) -> Path:
        relative = Path("workbench/libs/mesa") / wrapper
        # A lexical '../' include still requires its preceding directory to exist.
        (self.fixture / "gen" / relative).mkdir(parents=True, exist_ok=True)
        output = self.fixture / ("wrapper-" + (wrapper or "parent") + ".mk")
        self.run_command([
            sys.executable, str(ROOT / "tools/genmf/genmf.py"),
            str(ROOT / "config/make.tmpl"), str(ROOT / relative / "mmakefile.src"),
            str(output),
        ])
        with output.open("a", encoding="iso-8859-15") as generated:
            generated.write("\n" + (LEGACY_POLICY if counter else "") + READBACK)
        return output

    def options(self, wrapper: str, version: str, counter: bool = False) -> str:
        generated = self.generate(wrapper, counter)
        return self.run_command([
            self.make, "-rR", "-f", str(generated), "inspect-mesa-options",
            f"TOP={self.fixture}", f"SRCDIR={ROOT}",
            f"CURDIR={Path('workbench/libs/mesa') / wrapper}",
            f"GENDIR={self.fixture}/gen", f"PORTSDIR={self.fixture}/ports",
            "TARGET=inspect-mesa-options", "CFLAGS_GNU11=-std=gnu11",
            "CXXFLAGS_CXX17=-std=c++17", "CFLAGS_FUNCTION_SECTIONS=-ffunction-sections",
            f"OPT_MESAGL={version}", "AROS_TARGET_CPU=arm", "AROS_TARGET_ARCH=raspi",
        ])

    def test_all_wrappers_ignore_stale_linker_anchor_files(self) -> None:
        for version in ("20.0.8", "26.0.0"):
            for wrapper in WRAPPERS:
                with self.subTest(version=version, wrapper=wrapper):
                    self.legacy.unlink(missing_ok=True)
                    clean = self.options(wrapper, version)
                    self.assertIn("-std=gnu11", clean)
                    self.assertIn("-std=c++17", clean)
                    self.assertNotIn("-ffunction-sections", clean)
                    self.legacy.write_text(
                        "mesa3dgl_ENTRYPOINTS += -Wl,--undefined=stale_entrypoint\n",
                        encoding="utf-8",
                    )
                    self.assertEqual(self.options(wrapper, version), clean)

    def test_legacy_dependency_counter_changes_actual_make_readback(self) -> None:
        self.legacy.write_text(
            "mesa3dgl_ENTRYPOINTS += -Wl,--undefined=stale_entrypoint\n",
            encoding="utf-8",
        )
        for wrapper in WRAPPERS:
            with self.subTest(wrapper=wrapper):
                current = self.options(wrapper, "26.0.0")
                regressed = self.options(wrapper, "26.0.0", counter=True)
                self.assertNotEqual(regressed, current)
                self.assertIn("-ffunction-sections", regressed)


if __name__ == "__main__":
    unittest.main()
