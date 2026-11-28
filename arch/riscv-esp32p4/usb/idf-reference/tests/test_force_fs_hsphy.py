"""Configure-only checks for the isolated TinyUSB DWC2 source override."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REFERENCE = Path(__file__).resolve().parents[1]
MODULE = REFERENCE / "force_fs_hsphy.cmake"
DEFAULT_SOURCE = (
    REFERENCE
    / "managed_components/espressif__tinyusb"
    / "src/portable/synopsys/dwc2/dcd_dwc2.c"
)
EXPECTED_SHA256 = "b02378ee5b90f4eb00fee6289ab7531432e7e41b91e618c0319890b03b9fb25f"
ORIGINAL_STATEMENT = "dcfg |= DCFG_DSPD_HS << DCFG_DSPD_Pos;"
REPLACEMENT_STATEMENT = "dcfg |= DCFG_DSPD_FS_HSPHY << DCFG_DSPD_Pos;"


def configured_source() -> Path | None:
    supplied = os.environ.get("HS_REFERENCE_DCD_DWC2_SOURCE")
    source = Path(supplied).expanduser() if supplied else DEFAULT_SOURCE
    if source.is_file():
        return source.resolve()
    return None


def cmake_quote(value: str | Path) -> str:
    """Quote a local path for the generated temporary CMakeLists.txt."""
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


class ForceFsHsphyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = configured_source()
        if cls.source is None:
            raise unittest.SkipTest(
                "pinned TinyUSB dcd_dwc2.c is not present; set "
                "HS_REFERENCE_DCD_DWC2_SOURCE to its path"
            )
        digest = hashlib.sha256(cls.source.read_bytes()).hexdigest()
        if digest != EXPECTED_SHA256:
            raise AssertionError(
                f"supplied dcd_dwc2.c SHA-256 is {digest}, expected {EXPECTED_SHA256}"
            )

    def configure(
        self,
        source: Path,
        *,
        source_entries: tuple[str, ...] | None = None,
        output: str | Path | None = None,
        relative_target_source: bool = False,
        output_parent_symlink_target: Path | None = None,
        output_hardlink: bool = False,
        mode: str = "",
        bvalid: bool = False,
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        temporary = tempfile.TemporaryDirectory(prefix="force-fs-hsphy-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        project = root / "project"
        build = root / "build"
        project.mkdir()
        if output_hardlink:
            (build / "hs-fs-comparison").mkdir(parents=True)
            os.link(source, build / "hs-fs-comparison/dcd_dwc2.c")
        if output_parent_symlink_target is not None:
            build.mkdir()
            (build / "hs-fs-comparison").symlink_to(
                output_parent_symlink_target, target_is_directory=True
            )
        (project / "other.c").write_text("int other(void) { return 0; }\n")

        if source_entries is not None:
            entries = source_entries
        elif relative_target_source:
            (project / "dcd_dwc2.c").symlink_to(source)
            entries = ("dcd_dwc2.c",)
        else:
            entries = (str(source),)
        sources = "\n".join(f"    {cmake_quote(entry)}" for entry in entries)
        output_path = output or (build / "hs-fs-comparison/dcd_dwc2.c")
        project_file = f"""\
cmake_minimum_required(VERSION 3.22)
project(force_fs_hsphy_test C)
add_library(espressif__tinyusb STATIC
{sources}
)
target_include_directories(espressif__tinyusb PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}/existing-include")
include({cmake_quote(MODULE)})
set(HS_REFERENCE_BVALID {"ON" if bvalid else "OFF"})
hs_reference_force_fs_hsphy(
    espressif__tinyusb
    {cmake_quote(source)}
    {cmake_quote(output_path)}
    {mode}
)

get_target_property(actual_sources espressif__tinyusb SOURCES)
get_target_property(actual_includes espressif__tinyusb INCLUDE_DIRECTORIES)
file(REAL_PATH {cmake_quote(source)} expected_source)
file(REAL_PATH {cmake_quote(source.parent)} expected_include)
file(REAL_PATH "${{CMAKE_BINARY_DIR}}/hs-fs-comparison/dcd_dwc2.c" expected_output)
set(found_original FALSE)
set(found_output FALSE)
foreach(item IN LISTS actual_sources)
    if(item MATCHES "\\\\$<")
        continue()
    endif()
    file(REAL_PATH "${{item}}" item_real BASE_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}")
    if(item_real STREQUAL expected_source)
        set(found_original TRUE)
    endif()
    if(item_real STREQUAL expected_output)
        set(found_output TRUE)
    endif()
endforeach()
if(found_original)
    message(FATAL_ERROR "original dependency source remains in target SOURCES")
endif()
if(NOT found_output)
    message(FATAL_ERROR "generated source is missing from target SOURCES")
endif()
list(FIND actual_includes "${{expected_include}}" source_include_index)
if(source_include_index EQUAL -1)
    message(FATAL_ERROR "original source parent is missing from PRIVATE includes")
endif()
list(FIND actual_includes "${{CMAKE_CURRENT_SOURCE_DIR}}/existing-include" existing_include_index)
if(existing_include_index EQUAL -1)
    message(FATAL_ERROR "pre-existing include directory was lost")
endif()
"""
        (project / "CMakeLists.txt").write_text(project_file)
        result = subprocess.run(
            ["cmake", "-S", str(project), "-B", str(build)],
            check=False,
            capture_output=True,
            text=True,
        )
        return result, build

    def test_rx_trace_only_adds_three_observer_calls(self) -> None:
        original = self.source.read_text()
        result, build = self.configure(self.source, mode="RX_TRACE")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        generated = (build / "hs-fs-comparison/dcd_dwc2.c").read_text()
        self.assertEqual(generated.count("hs_rx_record("), 3)
        self.assertEqual(generated.count('include "rx_trace.h"'), 1)
        restored = generated.removeprefix('#include "rx_trace.h"\n')
        for call in (
            "hs_rx_record(&hs_rx_state, rhport, HS_RX_IRQ, gintsts);",
            "hs_rx_record(&hs_rx_state, rhport, HS_RX_POP, grxstsp.value);",
            "hs_rx_record(&hs_rx_state, rhport, HS_RX_EP_SETUP, epnum);",
        ):
            self.assertEqual(restored.count(call), 1)
            restored = restored.replace("\n  " + call, "")
        self.assertEqual(restored, original)
        self.assertEqual(self.source.read_text(), original)

    def test_rejects_unknown_diagnostic_mode(self) -> None:
        result, _ = self.configure(self.source, mode="UNKNOWN")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unknown DCD diagnostic mode", result.stdout + result.stderr)

    def test_bvalid_changes_only_session_initialization(self) -> None:
        off, off_build = self.configure(self.source, mode="RX_TRACE")
        on, on_build = self.configure(self.source, mode="RX_TRACE", bvalid=True)
        self.assertEqual(off.returncode, 0, off.stdout + off.stderr)
        self.assertEqual(on.returncode, 0, on.stdout + on.stderr)
        old = "dwc2->gotgctl &= ~(GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL | GOTGCTL_VBVALOVAL);"
        new = "dwc2->gotgctl = (dwc2->gotgctl & ~GOTGCTL_AVALOEN) | GOTGCTL_BVALOEN | GOTGCTL_BVALOVAL;"
        off_text = (off_build / "hs-fs-comparison/dcd_dwc2.c").read_text()
        on_text = (on_build / "hs-fs-comparison/dcd_dwc2.c").read_text()
        self.assertEqual(off_text.count(old), 1)
        self.assertEqual(on_text.count(new), 1)
        self.assertEqual(on_text, off_text.replace(old, new))

    def test_replaces_one_statement_in_an_out_of_tree_copy(self) -> None:
        original_bytes = self.source.read_bytes()
        original_digest = hashlib.sha256(original_bytes).hexdigest()

        result, build = self.configure(self.source)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        generated = build / "hs-fs-comparison/dcd_dwc2.c"
        self.assertTrue(generated.is_file())
        generated_bytes = generated.read_bytes()
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), original_digest)

        original_text = original_bytes.decode()
        generated_text = generated_bytes.decode()
        self.assertEqual(original_text.count(ORIGINAL_STATEMENT), 1)
        self.assertEqual(
            generated_text,
            original_text.replace(ORIGINAL_STATEMENT, REPLACEMENT_STATEMENT, 1),
        )
        self.assertEqual(generated_text.count(REPLACEMENT_STATEMENT), 1)
        original_lines = original_text.splitlines()
        generated_lines = generated_text.splitlines()
        self.assertEqual(len(original_lines), len(generated_lines))
        self.assertEqual(
            [index for index, pair in enumerate(zip(original_lines, generated_lines)) if pair[0] != pair[1]],
            [next(i for i, line in enumerate(original_lines) if ORIGINAL_STATEMENT in line)],
        )

    def test_normalizes_a_relative_source_in_target_sources(self) -> None:
        result, _ = self.configure(self.source, relative_target_source=True)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_a_mutated_dependency_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="force-fs-hsphy-mutated-") as temporary:
            mutated = Path(temporary) / "dcd_dwc2.c"
            shutil.copyfile(self.source, mutated)
            with mutated.open("ab") as changed_source:
                changed_source.write(b"\n/* test-only mutation */\n")

            result, _ = self.configure(mutated)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SHA-256 mismatch", result.stdout + result.stderr)

    def test_rejects_when_the_source_is_not_a_target_member(self) -> None:
        result, _ = self.configure(
            self.source,
            source_entries=("${CMAKE_CURRENT_SOURCE_DIR}/other.c",),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 0", result.stdout + result.stderr)

    def test_rejects_duplicate_target_source_members(self) -> None:
        result, _ = self.configure(self.source, source_entries=(str(self.source), str(self.source)))

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("found 2", result.stdout + result.stderr)

    def test_replaces_output_hardlink_without_changing_source(self) -> None:
        original = self.source.read_bytes()
        with tempfile.TemporaryDirectory(prefix="force-fs-hardlink-") as temporary:
            source = Path(temporary) / "dcd_dwc2.c"
            shutil.copyfile(self.source, source)
            result, build = self.configure(source, output_hardlink=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(source.read_bytes(), original)
            self.assertFalse(os.path.samefile(source, build / "hs-fs-comparison/dcd_dwc2.c"))

    def test_rejects_output_parent_symlink_escape(self) -> None:
        with tempfile.TemporaryDirectory(prefix="force-fs-hsphy-output-") as temporary:
            outside = Path(temporary) / "outside-build"
            outside.mkdir()
            original_digest = hashlib.sha256(self.source.read_bytes()).hexdigest()

            result, _ = self.configure(
                self.source, output_parent_symlink_target=outside
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("symbolic-link", result.stdout + result.stderr)
            self.assertFalse((outside / "dcd_dwc2.c").exists())
            self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), original_digest)

    def test_rejects_relative_output_path(self) -> None:
        result, _ = self.configure(self.source, output="relative-output.c")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output must be an absolute path", result.stdout + result.stderr)

    def test_rejects_output_in_dependency_directory(self) -> None:
        output = self.source.parent / "must-not-be-created.c"

        result, _ = self.configure(self.source, output=output)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
