#!/usr/bin/env python3
"""Tests for prepare_stack.py.

Run the archive integration cases with:
  TINYUSB_ARCHIVE=/path/to/tinyusb.tar.gz python3 test_prepare_stack.py
"""

from __future__ import annotations

import os
import shutil
import tarfile
import tempfile
import unittest
from pathlib import Path

import prepare_stack as stack


def _base_members() -> list[tarfile.TarInfo]:
    root = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/")
    root.type = tarfile.DIRTYPE
    source = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/src/")
    source.type = tarfile.DIRTYPE
    license_file = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/LICENSE")
    license_file.type = tarfile.REGTYPE
    license_file.size = 0
    return [root, source, license_file]


class ArchiveInventoryTests(unittest.TestCase):
    def test_rejects_path_traversal(self) -> None:
        members = _base_members()
        members.append(tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/src/../../outside"))
        with self.assertRaises(stack.PrepareError):
            stack.validate_members(members)

    def test_rejects_links_in_staged_source(self) -> None:
        members = _base_members()
        link = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/src/link")
        link.type = tarfile.SYMTYPE
        link.linkname = "../LICENSE"
        members.append(link)
        with self.assertRaisesRegex(stack.PrepareError, "link in staged"):
            stack.validate_members(members)

    def test_rejects_hardlinks(self) -> None:
        members = _base_members()
        link = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/docs/link")
        link.type = tarfile.LNKTYPE
        link.linkname = f"{stack.TINYUSB_ROOT}/LICENSE"
        members.append(link)
        with self.assertRaisesRegex(stack.PrepareError, "hard link"):
            stack.validate_members(members)

    def test_excluded_root_symlink_must_stay_inside_archive_root(self) -> None:
        safe_members = _base_members()
        safe_link = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/.agents")
        safe_link.type = tarfile.SYMTYPE
        safe_link.linkname = ".claude"
        safe_members.append(safe_link)
        self.assertEqual(len(stack.validate_members(safe_members)), len(safe_members))

        escaping_members = _base_members()
        escaping_link = tarfile.TarInfo(f"{stack.TINYUSB_ROOT}/.agents")
        escaping_link.type = tarfile.SYMTYPE
        escaping_link.linkname = "../../outside"
        escaping_members.append(escaping_link)
        with self.assertRaisesRegex(stack.PrepareError, "escapes archive root"):
            stack.validate_members(escaping_members)


class PinnedArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        archive = os.environ.get("TINYUSB_ARCHIVE")
        if not archive:
            raise unittest.SkipTest("set TINYUSB_ARCHIVE to run pinned archive integration tests")
        cls.archive = Path(archive)
        if not cls.archive.is_file():
            raise unittest.SkipTest(f"TinyUSB archive does not exist: {cls.archive}")

    def test_original_archive_stages_only_license_and_src(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            upstream = root / "upstream"
            output = root / "stage"
            stack.extract_archive(self.archive, upstream)
            stack.prepare_stack(self.archive, output)

            self.assertEqual({path.name for path in output.iterdir()}, {"LICENSE", "src"})
            self.assertGreater((output / "src").joinpath("tusb.c").stat().st_size, 0)
            self.assertEqual((output / "LICENSE").read_bytes(), (upstream / "LICENSE").read_bytes())

            upstream_entries = {
                path.relative_to(upstream).as_posix() for path in upstream.rglob("*")
            }
            staged_entries = {
                path.relative_to(output).as_posix() for path in output.rglob("*")
            }
            self.assertEqual(staged_entries, upstream_entries)
            patched = {
                "src/portable/synopsys/dwc2/dwc2_common.c",
                "src/portable/synopsys/dwc2/dwc2_common.h",
                "src/portable/synopsys/dwc2/dcd_dwc2.c",
            }
            for relative in sorted(upstream_entries):
                source = upstream / relative
                staged = output / relative
                if source.is_file():
                    if relative in patched:
                        self.assertNotEqual(staged.read_bytes(), source.read_bytes())
                        self.assertEqual(staged.stat().st_mtime_ns, source.stat().st_mtime_ns)
                    else:
                        self.assertEqual(staged.read_bytes(), source.read_bytes(), relative)

            common_c = (output / "src/portable/synopsys/dwc2/dwc2_common.c").read_text()
            common_h = (output / "src/portable/synopsys/dwc2/dwc2_common.h").read_text()
            dcd_c = (output / "src/portable/synopsys/dwc2/dcd_dwc2.c").read_text()
            self.assertIn('#include "p4_usb_platform.h"', common_c)
            self.assertIn('#include "p4_usb_platform.h"', common_h)
            self.assertIn('#include "p4_usb_platform.h"', dcd_c)
            self.assertIn('#include "dwc2_p4_aros.h"', common_h)
            self.assertNotIn('#include "dwc2_esp32.h"', common_h)
            self.assertEqual(common_c.count("p4_usb_guard();"), 4)
            self.assertEqual(common_h.count("p4_usb_guard();"), 2)
            self.assertEqual(dcd_c.count("p4_usb_guard();"), 5)
            usb_wrap_guard = (
                "#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31) "
                "&& !defined(P4_USB_POLLED_HS_ONLY)"
            )
            unguarded_usb_wrap = (
                "#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31)"
            )
            dcd_lines = dcd_c.splitlines()
            self.assertEqual(sum(line == usb_wrap_guard for line in dcd_lines), 2)
            self.assertFalse(any(line == unguarded_usb_wrap for line in dcd_lines))

    def test_corrupt_archive_is_rejected_without_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            corrupt = root / "corrupt.tar.gz"
            content = bytearray(self.archive.read_bytes())
            content[len(content) // 2] ^= 0x01
            corrupt.write_bytes(content)
            output = root / "must-not-exist"

            with self.assertRaisesRegex(stack.PrepareError, "SHA-256 mismatch"):
                stack.prepare_stack(corrupt, output)
            self.assertFalse(output.exists())

    def test_existing_output_is_rejected_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "existing"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_text("keep")

            with self.assertRaisesRegex(stack.PrepareError, "already exists"):
                stack.prepare_stack(self.archive, output)
            self.assertEqual(sentinel.read_text(), "keep")

    def test_changed_patch_pattern_fails_on_isolated_extracted_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            extracted = root / "extracted"
            stack.extract_archive(self.archive, extracted)
            isolated = root / "isolated"
            shutil.copytree(extracted, isolated)

            common_c = isolated / "src/portable/synopsys/dwc2/dwc2_common.c"
            original = common_c.read_bytes()
            expected = b"while (dwc2->grstctl & GRSTCTL_CSRST) {}"
            replacement = b"while (dwc2->grstctl & GRSTCTL_CSRST) { }"
            self.assertEqual(original.count(expected), 1)
            common_c.write_bytes(original.replace(expected, replacement, 1))

            output = root / "must-not-exist"
            with self.assertRaisesRegex(stack.PrepareError, "unexpected TinyUSB source pattern"):
                stack.stage_source_tree(isolated, output)
            self.assertFalse(output.exists())

    def test_changed_usb_wrap_guard_fails_on_isolated_extracted_copy(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            extracted = root / "extracted"
            stack.extract_archive(self.archive, extracted)
            isolated = root / "isolated"
            shutil.copytree(extracted, isolated)

            dcd_c = isolated / "src/portable/synopsys/dwc2/dcd_dwc2.c"
            original = dcd_c.read_bytes()
            expected = b"#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31)"
            replacement = b"#if defined(TUP_USBIP_DWC2_ESP32)  && !TU_CHECK_MCU(OPT_MCU_ESP32S31)"
            self.assertEqual(original.count(expected), 2)
            dcd_c.write_bytes(original.replace(expected, replacement, 1))

            output = root / "must-not-exist"
            with self.assertRaisesRegex(stack.PrepareError, "unexpected TinyUSB source pattern"):
                stack.stage_source_tree(isolated, output)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
