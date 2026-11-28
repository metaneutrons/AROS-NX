#!/usr/bin/env python3
"""Convert a user-supplied GSLX670_FW C table to Silead firmware records.

This tool intentionally contains no controller firmware.  The input remains
the user's responsibility because the redistribution status of Silead RAM
firmware is not clear enough for AROS to carry the D1001 vendor table.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import tempfile
from pathlib import Path


TABLE_START = re.compile(
    r"static\s+const\s+struct\s+fw_data\s+GSLX670_FW\s*\[\s*\]\s*=\s*\{"
)
RECORD = re.compile(
    r"\{\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*,\s*"
    r"(0[xX][0-9a-fA-F]+|[0-9]+)\s*\}"
)
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def parse_table(source: str) -> list[tuple[int, int]]:
    match = TABLE_START.search(source)
    if not match:
        raise ValueError("GSLX670_FW table start not found")

    end = source.find("};", match.end())
    if end < 0:
        raise ValueError("GSLX670_FW table end not found")

    body = COMMENT.sub("", source[match.end() : end])
    records = [(int(a, 0), int(v, 0)) for a, v in RECORD.findall(body)]
    residue = RECORD.sub("", body)
    residue = re.sub(r"[\s,]", "", residue)
    if residue:
        raise ValueError(f"unparsed content in firmware table: {residue[:40]!r}")
    if not records:
        raise ValueError("GSLX670_FW table is empty")

    pages = 0
    for index, (offset, value) in enumerate(records):
        if not 0 <= offset <= 0xFF:
            raise ValueError(f"record {index}: offset 0x{offset:x} exceeds one byte")
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"record {index}: value 0x{value:x} exceeds 32 bits")
        if offset == 0xF0:
            pages += 1
        elif offset > 0x7C or offset % 4:
            raise ValueError(
                f"record {index}: data offset 0x{offset:02x} is not 0x00..0x7c/4"
            )
    if pages == 0:
        raise ValueError("firmware table contains no 0xf0 page-selection record")

    return records


def encode_binary(records: list[tuple[int, int]]) -> bytes:
    # This is the format consumed by Linux's independent Silead driver:
    # little-endian u32 offset followed by little-endian u32 value.
    return b"".join(struct.pack("<II", offset, value) for offset, value in records)


def encode_header(records: list[tuple[int, int]], digest: str) -> bytes:
    lines = [
        "/* Generated from user-supplied firmware; do not commit this file. */",
        f"#define P4_GSL_FW_RECORD_COUNT {len(records)}U",
        f'#define P4_GSL_FW_SHA256 "{digest}"',
        "static const struct P4GSLFirmwareRecord p4_gsl_firmware[] = {",
    ]
    lines.extend(
        f"    {{ 0x{offset:02x}U, 0x{value:08x}UL }},"
        for offset, value in records
    )
    lines.extend(("};", ""))
    return "\n".join(lines).encode("ascii")


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="vendor header containing GSLX670_FW")
    parser.add_argument("output", type=Path, help="generated .fw binary")
    parser.add_argument(
        "--c-header",
        type=Path,
        help="also write a private generated C include for a diagnostic build",
    )
    args = parser.parse_args()

    records = parse_table(args.input.read_text(encoding="utf-8"))
    binary = encode_binary(records)
    digest = hashlib.sha256(binary).hexdigest()
    atomic_write(args.output, binary)
    if args.c_header:
        atomic_write(args.c_header, encode_header(records, digest))

    print(f"records: {len(records)}")
    print(f"bytes: {len(binary)}")
    print(f"sha256: {digest}")
    print(f"binary: {args.output}")
    if args.c_header:
        print(f"private C header: {args.c_header}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
