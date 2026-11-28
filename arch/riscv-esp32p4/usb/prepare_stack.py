#!/usr/bin/env python3
"""Stage the pinned TinyUSB sources with the ESP32-P4 integration patches."""

from __future__ import annotations

import argparse
import hashlib
import os
import posixpath
import re
import shutil
import stat
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath
from typing import Iterable


TINYUSB_COMMIT = "4c195b7b94f813a37b77b184bcae7b2b6ef2835e"
TINYUSB_ROOT = f"tinyusb-{TINYUSB_COMMIT}"
TINYUSB_SHA256 = "f42c184ca988bb1f2e71a9f3f88c544e21752602331de981883d7bf9c0fc6063"


class PrepareError(RuntimeError):
    """An input archive or source tree does not match the staging contract."""


def _exists(path: Path) -> bool:
    try:
        path.lstat()
    except FileNotFoundError:
        return False
    return True


def _require_absent(path: Path) -> None:
    if _exists(path):
        raise PrepareError(f"destination already exists: {path}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise PrepareError(f"cannot read archive {path}: {exc}") from exc
    return digest.hexdigest()


def verify_archive(archive: Path) -> None:
    actual = _sha256(archive)
    if actual != TINYUSB_SHA256:
        raise PrepareError(
            f"TinyUSB archive SHA-256 mismatch: expected {TINYUSB_SHA256}, got {actual}"
        )


def _member_path(member: tarfile.TarInfo) -> tuple[str, ...]:
    name = member.name
    if not name or name.startswith("/") or "\\" in name or "\0" in name:
        raise PrepareError(f"unsafe archive path: {name!r}")
    if member.isdir() and name.endswith("/"):
        name = name[:-1]
    parts = tuple(name.split("/"))
    if any(part in ("", ".", "..") for part in parts):
        raise PrepareError(f"unsafe archive path: {member.name!r}")
    if parts[0] != TINYUSB_ROOT:
        raise PrepareError(f"unexpected top-level archive path: {member.name!r}")
    return parts


def _is_staged_path(parts: tuple[str, ...]) -> bool:
    return parts[1:] == ("LICENSE",) or parts[1:2] == ("src",)


def _validate_link_target(member: tarfile.TarInfo, parts: tuple[str, ...]) -> None:
    target = member.linkname
    if not target or target.startswith("/") or "\\" in target or "\0" in target:
        raise PrepareError(f"unsafe symlink target for {member.name!r}: {target!r}")
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname("/".join(parts)), target))
    if resolved != TINYUSB_ROOT and not resolved.startswith(TINYUSB_ROOT + "/"):
        raise PrepareError(f"symlink escapes archive root: {member.name!r} -> {target!r}")


def validate_members(members: Iterable[tarfile.TarInfo]) -> list[tarfile.TarInfo]:
    """Validate the complete archive inventory before writing any member."""
    validated: list[tuple[tarfile.TarInfo, tuple[str, ...]]] = []
    by_path: dict[tuple[str, ...], tarfile.TarInfo] = {}

    for member in members:
        parts = _member_path(member)
        if parts in by_path:
            raise PrepareError(f"duplicate archive path: {member.name!r}")
        by_path[parts] = member

        if member.isdir() or member.isreg():
            pass
        elif member.issym():
            _validate_link_target(member, parts)
            if _is_staged_path(parts):
                raise PrepareError(f"link in staged source tree: {member.name!r}")
        elif member.islnk():
            raise PrepareError(f"hard link in archive: {member.name!r}")
        else:
            raise PrepareError(f"unsupported archive entry type: {member.name!r}")

        if member.size < 0:
            raise PrepareError(f"negative archive member size: {member.name!r}")
        validated.append((member, parts))

    for parts, member in by_path.items():
        for end in range(1, len(parts)):
            ancestor = by_path.get(parts[:end])
            if ancestor is not None and not ancestor.isdir():
                raise PrepareError(f"non-directory archive parent: {ancestor.name!r}")

    root = by_path.get((TINYUSB_ROOT,))
    source = by_path.get((TINYUSB_ROOT, "src"))
    license_file = by_path.get((TINYUSB_ROOT, "LICENSE"))
    if root is None or not root.isdir():
        raise PrepareError(f"archive is missing its root directory {TINYUSB_ROOT!r}")
    if source is None or not source.isdir():
        raise PrepareError("archive is missing the TinyUSB src directory")
    if license_file is None or not license_file.isreg():
        raise PrepareError("archive is missing a regular LICENSE file")

    return [member for member, _parts in validated]


def _extract_selected(tar: tarfile.TarFile, members: list[tarfile.TarInfo], destination: Path) -> None:
    destination.mkdir(mode=0o755)
    directories: list[tuple[Path, tarfile.TarInfo]] = []

    for member in members:
        parts = _member_path(member)
        if not _is_staged_path(parts):
            continue
        relative = parts[1:]
        target = destination.joinpath(*relative)

        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            directories.append((target, member))
            continue

        if not member.isreg():
            raise PrepareError(f"non-regular staged file: {member.name!r}")
        target.parent.mkdir(parents=True, exist_ok=True)
        source = tar.extractfile(member)
        if source is None:
            raise PrepareError(f"cannot read archive member: {member.name!r}")
        with source, target.open("xb") as output:
            shutil.copyfileobj(source, output)
        os.chmod(target, stat.S_IMODE(member.mode) & 0o777)
        os.utime(target, (member.mtime, member.mtime), follow_symlinks=False)

    for directory, member in sorted(directories, key=lambda item: len(item[0].parts), reverse=True):
        os.chmod(directory, stat.S_IMODE(member.mode) & 0o777)
        os.utime(directory, (member.mtime, member.mtime), follow_symlinks=False)
    os.chmod(destination, 0o755)
    os.utime(destination, (0, 0), follow_symlinks=False)


def extract_archive(archive: Path, destination: Path) -> None:
    """Safely extract only LICENSE and src from the pinned archive."""
    archive = Path(archive)
    destination = Path(destination)
    _require_absent(destination)
    verify_archive(archive)
    destination.parent.mkdir(parents=True, exist_ok=True)

    try:
        with tarfile.open(archive, mode="r:gz") as tar:
            members = validate_members(tar.getmembers())
            with tempfile.TemporaryDirectory(
                prefix=f".{destination.name}.extract-", dir=destination.parent
            ) as temporary:
                staged = Path(temporary) / "tree"
                _extract_selected(tar, members, staged)
                _require_absent(destination)
                os.rename(staged, destination)
    except (OSError, tarfile.TarError) as exc:
        raise PrepareError(f"cannot extract TinyUSB archive: {exc}") from exc


def _strip_c_comments(source: str) -> str:
    """Remove comments while preserving string and character literals."""
    output: list[str] = []
    index = 0
    state = "code"
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "/":
                output.extend("  ")
                index += 2
                state = "line"
                continue
            if char == "/" and following == "*":
                output.extend("  ")
                index += 2
                state = "block"
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            output.append(char)
        elif state == "line":
            if char == "\n":
                output.append(char)
                state = "code"
            else:
                output.append(" ")
        elif state == "block":
            if char == "*" and following == "/":
                output.extend("  ")
                index += 2
                state = "code"
                continue
            output.append("\n" if char == "\n" else " ")
        else:
            output.append(char)
            if char == "\\" and following:
                output.append(following)
                index += 2
                continue
            if (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        index += 1
    if state == "block":
        raise PrepareError("unterminated block comment in DWC2 source")
    return "".join(output)


def _replace_exact(source: str, old: str, new: str, expected: int, label: str) -> str:
    found = source.count(old)
    if found != expected:
        raise PrepareError(f"unexpected TinyUSB source pattern in {label}: expected {expected}, found {found}")
    return source.replace(old, new)


def _patch_file(root: Path, relative: str, replacements: list[tuple[str, str, int]]) -> str:
    path = root / relative
    try:
        original_stat = path.stat()
        original = path.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise PrepareError(f"cannot read required TinyUSB source {relative}: {exc}") from exc
    if "p4_usb_guard" in original or "p4_usb_platform.h" in original:
        raise PrepareError(f"unexpected pre-existing ESP32-P4 patch marker in {relative}")

    source = original
    for old, new, expected in replacements:
        source = _replace_exact(source, old, new, expected, relative)
    try:
        path.write_bytes(source.encode("utf-8"))
        os.utime(path, (original_stat.st_atime, original_stat.st_mtime), follow_symlinks=False)
    except OSError as exc:
        raise PrepareError(f"cannot write staged TinyUSB source {relative}: {exc}") from exc
    return source


def _check_loop_inventory(relative: str, source: str, whiles: int, dos: int) -> None:
    code = _strip_c_comments(source)
    while_count = len(re.findall(r"\bwhile\s*\(", code))
    do_count = len(re.findall(r"\bdo\s*\{", code))
    if (while_count, do_count) != (whiles, dos):
        raise PrepareError(
            f"unexpected DWC2 loop inventory in {relative}: "
            f"expected {whiles} while/{dos} do loops, found {while_count} while/{do_count} do loops"
        )


def _apply_p4_patches(root: Path) -> None:
    common_c = "portable/synopsys/dwc2/dwc2_common.c"
    common_h = "portable/synopsys/dwc2/dwc2_common.h"
    dcd_c = "portable/synopsys/dwc2/dcd_dwc2.c"

    # Count every code loop, not only the edited text, so source drift fails closed.
    # The RXFLVL do-while contributes a fifth `while (...)` token in dcd_dwc2.c.
    for relative, whiles, dos in ((common_c, 4, 0), (common_h, 2, 0), (dcd_c, 5, 1)):
        try:
            source = (root / relative).read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError) as exc:
            raise PrepareError(f"cannot read required TinyUSB source {relative}: {exc}") from exc
        _check_loop_inventory(relative, source, whiles, dos)
        if "p4_usb_guard" in source or "p4_usb_platform.h" in source:
            raise PrepareError(f"unexpected pre-existing ESP32-P4 patch marker in {relative}")

    _patch_file(
        root,
        common_c,
        [
            ('#include "tusb_option.h"\n', '#include "tusb_option.h"\n#include "p4_usb_platform.h"\n', 1),
            (
                "while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) {\n  }",
                "while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) {\n    p4_usb_guard();\n  }",
                1,
            ),
            (
                "while (dwc2->grstctl & GRSTCTL_CSRST) {}",
                "while (dwc2->grstctl & GRSTCTL_CSRST) { p4_usb_guard(); }",
                1,
            ),
            (
                "while (!(dwc2->grstctl & GRSTCTL_CSRST_DONE)) {}",
                "while (!(dwc2->grstctl & GRSTCTL_CSRST_DONE)) { p4_usb_guard(); }",
                1,
            ),
            (
                "while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) {} // wait for AHB master IDLE",
                "while (!(dwc2->grstctl & GRSTCTL_AHBIDL)) { p4_usb_guard(); } // wait for AHB master IDLE",
                1,
            ),
        ],
    )

    _patch_file(
        root,
        common_h,
        [
            ('#include "dwc2_type.h"\n', '#include "dwc2_type.h"\n#include "p4_usb_platform.h"\n', 1),
            (
                '#elif defined(TUP_USBIP_DWC2_ESP32)\n  #include "dwc2_esp32.h"',
                '#elif defined(TUP_USBIP_DWC2_ESP32)\n  #include "dwc2_p4_aros.h"',
                1,
            ),
            (
                "while (0 != (dwc2->grstctl & GRSTCTL_TXFFLSH_Msk)) {}",
                "while (0 != (dwc2->grstctl & GRSTCTL_TXFFLSH_Msk)) { p4_usb_guard(); }",
                1,
            ),
            (
                "while (0 != (dwc2->grstctl & GRSTCTL_RXFFLSH_Msk)) {}",
                "while (0 != (dwc2->grstctl & GRSTCTL_RXFFLSH_Msk)) { p4_usb_guard(); }",
                1,
            ),
        ],
    )

    _patch_file(
        root,
        dcd_c,
        [
            ('#include "dwc2_common.h"\n', '#include "dwc2_common.h"\n#include "p4_usb_platform.h"\n', 1),
            (
                "#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31)",
                "#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31) && !defined(P4_USB_POLLED_HS_ONLY)",
                2,
            ),
            (
                "while ((dep->diepint & DIEPINT_INEPNE) == 0) {}",
                "while ((dep->diepint & DIEPINT_INEPNE) == 0) { p4_usb_guard(); }",
                1,
            ),
            (
                "while ((dep->diepint & DIEPINT_EPDISD_Msk) == 0) {}",
                "while ((dep->diepint & DIEPINT_EPDISD_Msk) == 0) { p4_usb_guard(); }",
                1,
            ),
            (
                "while ((dwc2->gintsts & GINTSTS_BOUTNAKEFF_Msk) == 0) {}",
                "while ((dwc2->gintsts & GINTSTS_BOUTNAKEFF_Msk) == 0) { p4_usb_guard(); }",
                1,
            ),
            (
                "while ((dep->doepint & DOEPINT_EPDISD_Msk) == 0) {}",
                "while ((dep->doepint & DOEPINT_EPDISD_Msk) == 0) { p4_usb_guard(); }",
                1,
            ),
            (
                "    do {\n      handle_rxflvl_irq(rhport); // read all packets\n    } while(dwc2->gintsts & GINTSTS_RXFLVL);",
                "    do {\n      p4_usb_guard();\n      handle_rxflvl_irq(rhport); // read all packets\n    } while(dwc2->gintsts & GINTSTS_RXFLVL);",
                1,
            ),
        ],
    )

    expected_calls = {common_c: 4, common_h: 2, dcd_c: 5}
    for relative, expected in expected_calls.items():
        source = (root / relative).read_bytes().decode("utf-8")
        code = _strip_c_comments(source)
        calls = len(re.findall(r"\bp4_usb_guard\s*\(\s*\)\s*;", code))
        if calls != expected:
            raise PrepareError(
                f"unexpected p4_usb_guard call count in {relative}: expected {expected}, found {calls}"
            )

    dcd = (root / dcd_c).read_bytes().decode("utf-8")
    usb_wrap_guard = (
        "#if defined(TUP_USBIP_DWC2_ESP32) && !TU_CHECK_MCU(OPT_MCU_ESP32S31) "
        "&& !defined(P4_USB_POLLED_HS_ONLY)"
    )
    if dcd.count(usb_wrap_guard) != 2:
        raise PrepareError("unexpected ESP32 USB_WRAP guard count in dcd_dwc2.c")


def _copy_regular_tree(source: Path, destination: Path) -> None:
    source_stat = source.lstat()
    if not stat.S_ISDIR(source_stat.st_mode):
        raise PrepareError(f"source root is not a directory: {source}")
    destination.mkdir()
    directories: list[tuple[Path, Path]] = [(source, destination)]

    for current, dirnames, filenames in os.walk(source, topdown=True, followlinks=False):
        current_path = Path(current)
        output_dir = destination / current_path.relative_to(source)
        dirnames.sort()
        filenames.sort()

        for name in list(dirnames):
            input_path = current_path / name
            metadata = input_path.lstat()
            if not stat.S_ISDIR(metadata.st_mode):
                raise PrepareError(f"symlink or non-directory in staged source: {input_path}")
            output_path = output_dir / name
            output_path.mkdir()
            directories.append((input_path, output_path))

        for name in filenames:
            input_path = current_path / name
            metadata = input_path.lstat()
            if not stat.S_ISREG(metadata.st_mode):
                raise PrepareError(f"symlink or special file in staged source: {input_path}")
            if metadata.st_nlink != 1:
                raise PrepareError(f"hard-linked file in staged source: {input_path}")
            output_path = output_dir / name
            shutil.copyfile(input_path, output_path)
            shutil.copystat(input_path, output_path, follow_symlinks=False)

    for input_dir, output_dir in sorted(
        directories, key=lambda pair: len(pair[0].parts), reverse=True
    ):
        shutil.copystat(input_dir, output_dir, follow_symlinks=False)


def stage_source_tree(source_root: Path, destination: Path) -> None:
    """Copy LICENSE + src, then apply the checked ESP32-P4 source edits."""
    source_root = Path(source_root)
    destination = Path(destination)
    _require_absent(destination)

    license_path = source_root / "LICENSE"
    source_path = source_root / "src"
    try:
        license_stat = license_path.lstat()
        if not stat.S_ISREG(license_stat.st_mode) or license_stat.st_nlink != 1:
            raise PrepareError("TinyUSB LICENSE must be a regular, unlinked file")
        if not stat.S_ISDIR(source_path.lstat().st_mode):
            raise PrepareError("TinyUSB src must be a real directory")
    except OSError as exc:
        raise PrepareError(f"incomplete TinyUSB source tree: {exc}") from exc

    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(
            prefix=f".{destination.name}.stage-", dir=destination.parent
        ) as temporary:
            staged = Path(temporary) / "tree"
            staged.mkdir()
            license_out = staged / "LICENSE"
            shutil.copyfile(license_path, license_out)
            shutil.copystat(license_path, license_out, follow_symlinks=False)
            _copy_regular_tree(source_path, staged / "src")
            _apply_p4_patches(staged / "src")
            os.chmod(staged, 0o755)
            os.utime(staged, (0, 0), follow_symlinks=False)
            _require_absent(destination)
            os.rename(staged, destination)
    except (OSError, shutil.Error) as exc:
        raise PrepareError(f"cannot stage TinyUSB source tree: {exc}") from exc


def prepare_stack(archive: Path, output_dir: Path) -> None:
    """Verify, extract and stage the pinned TinyUSB source into a fresh path."""
    archive = Path(archive)
    output_dir = Path(output_dir)
    _require_absent(output_dir)
    verify_archive(archive)
    output_dir.parent.mkdir(parents=True, exist_ok=True)

    try:
        with tempfile.TemporaryDirectory(
            prefix=f".{output_dir.name}.prepare-", dir=output_dir.parent
        ) as temporary:
            extracted = Path(temporary) / "upstream"
            staged = Path(temporary) / "staged"
            extract_archive(archive, extracted)
            stage_source_tree(extracted, staged)
            _require_absent(output_dir)
            os.rename(staged, output_dir)
    except (OSError, shutil.Error) as exc:
        raise PrepareError(f"cannot prepare TinyUSB stack: {exc}") from exc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path, help="local pinned TinyUSB .tar.gz archive")
    parser.add_argument("output_dir", type=Path, help="new output directory for LICENSE and src")
    args = parser.parse_args(argv)
    try:
        prepare_stack(args.archive, args.output_dir)
    except PrepareError as exc:
        parser.exit(1, f"prepare_stack.py: error: {exc}\n")
    print(f"Staged TinyUSB {TINYUSB_COMMIT} at {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
