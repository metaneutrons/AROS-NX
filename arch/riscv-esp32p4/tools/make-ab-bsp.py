#!/usr/bin/env python3
"""Bind an unsigned ESP32-P4 core to its BSP; never write a device.

SHA-256 detects corruption/mismatching, not publisher authenticity. The
128-byte envelope is consumed by the A/B bootloader and kernel only.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import tempfile

HEADER_BYTES = 128
CORE_LIMIT = 512 * 1024
BSP_LIMIT = 6 * 1024 * 1024


def envelope(core, package):
    if not 24 <= len(core) <= CORE_LIMIT or core[0] != 0xE9:
        raise ValueError("invalid or oversized ESP image")
    # ESP32-P4 image header chip ID is 18 (little endian at byte 12).
    if struct.unpack_from("<H", core, 12)[0] != 18:
        raise ValueError("image is not ESP32-P4")
    if len(package) < 8 or package[:4] != b"PKG\x01":
        raise ValueError("not PKG v1")
    if struct.unpack_from(">I", package, 4)[0] != len(package):
        raise ValueError("PKG length differs from complete file")
    if len(package) > BSP_LIMIT - HEADER_BYTES:
        raise ValueError("package plus envelope exceeds BSP slot")
    header = (b"P4ABPKG\x01" + struct.pack("<II", len(core), len(package))
              + hashlib.sha256(core).digest() + hashlib.sha256(package).digest())
    return header + hashlib.sha256(header).digest() + bytes(16) + package


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("package", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--replace", action="store_true",
                        help="atomically replace the host build artifact, never a device")
    args = parser.parse_args()
    try:
        # Reuse the actual loader's package auditor before producing a pair.
        spec = importlib.util.spec_from_file_location(
            "p4_package_audit", Path(__file__).resolve().parent.parent / "boot/audit-package.py")
        audit = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(audit)
        package, entries, declared = audit.members(args.package)
        if not entries or declared != len(package):
            raise ValueError("empty package or trailing bytes")
        supported = audit._read_supported_relocations()
        names = set()
        for name, size, offset in entries:
            if not name or name in names:
                raise ValueError("empty or duplicate package member name")
            names.add(name)
            _, _, issues = audit.audit_elf(package[offset:offset + size], supported)
            if issues:
                raise ValueError(f"unloadable PKG member: {name}")
        core = args.core.read_bytes()
        result = envelope(core, package)
        if args.replace:
            if args.output.is_symlink() or (args.output.exists() and not args.output.is_file()):
                raise ValueError("replacement target is not a regular host file")
            descriptor, temporary = tempfile.mkstemp(prefix=args.output.name + ".", dir=args.output.parent)
            try:
                with os.fdopen(descriptor, "wb") as output:
                    output.write(result)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, args.output)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        else:
            with args.output.open("xb") as output:
                output.write(result)
        print(f"A/B BSP: {len(result)} bytes sha256={hashlib.sha256(result).hexdigest()}")
    except (OSError, ValueError) as exc:
        parser.exit(1, f"A/B BSP rejected: {exc}\n")


if __name__ == "__main__":
    main()
