#!/usr/bin/env python3
"""Actual layout parser, isolated structural positive and negative fixtures."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "workbench/tools/ESP32Board"


def record(label, offset, size, kind=1, subtype=0):
    return struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label.encode(), 0)


def table(*records):
    return b"".join(records) + b"\xff" * 32


def main():
    good = table(record("nvs", 0x9000, 0x6000), record("app", 0x10000, 0x100000, 0, 0x10))
    md5 = b"\xeb\xeb" + b"\xff" * 14 + b"\xaa" * 16
    cases = [
        ("good", good, 0x400000, 1, 2),
        ("adapt-capacity", good, 0x1000000, 1, 2),
        ("unsorted", table(record("app", 0x10000, 0x100000, 0), record("nvs", 0x9000, 0x6000)), 0x400000, 1, 2),
        ("overlap", table(record("one", 0x9000, 0x6000), record("two", 0xA000, 0x1000)), 0x400000, 0, 0),
        ("outside", good, 0x100000, 0, 0),
        ("wrap", table(record("wrap", 0xFFFFF000, 0x2000)), 0x1000000, 0, 0),
        ("capacity-not-power2", good, 0x300000, 0, 0),
        ("capacity-too-big", good, 0x10000000, 0, 0),
        ("duplicate-label", table(record("nvs", 0x9000, 0x1000), record("nvs", 0xA000, 0x1000)), 0x400000, 0, 0),
        ("unaligned-data", table(record("nvs", 0x9001, 0x1000)), 0x400000, 0, 0),
        ("unaligned-app", table(record("app", 0x9000, 0x1000, 0)), 0x400000, 0, 0),
        ("zero-size", table(record("nvs", 0x9000, 0)), 0x400000, 0, 0),
        ("valid-data-size-unaligned", table(record("data", 0x9000, 0x1001)), 0x400000, 1, 1),
        ("invalid-app-size-unaligned", table(record("app", 0x10000, 0x1001, 0)), 0x400000, 0, 0),
        ("empty-label", table(record("", 0x9000, 0x1000)), 0x400000, 0, 0),
        ("control-label", table(record("bad\n", 0x9000, 0x1000)), 0x400000, 0, 0),
        ("unterminated", good[:-32], 0x400000, 0, 0),
        ("truncated", good[:-1], 0x400000, 0, 0),
        ("dirty-tail", good + bytes(32), 0x400000, 0, 0),
        ("md5-marker", good[:-32] + md5 + b"\xff" * 32, 0x400000, 1, 2),
        ("bad-md5-marker", good[:-32] + b"\xeb\xeb" + bytes(30), 0x400000, 0, 0),
        ("data-after-md5", good[:-32] + md5 + record("extra", 0x200000, 0x1000), 0x400000, 0, 0),
        ("empty-table", b"\xff" * 64, 0x400000, 0, 0),
        ("too-many", table(*(record(f"p{i}", i * 4096, 4096) for i in range(33))), 0x400000, 0, 0),
        ("too-big-table", good + b"\xff" * 4096, 0x400000, 0, 0),
    ]
    candidate = Path("/Volumes/Dev/Build/c6-legacy-log-build.8qQvpV/build/partition_table/partition-table.bin")
    if candidate.exists():
        data = candidate.read_bytes()
        count = sum(data[i:i+2] == b"\xaa\x50" for i in range(0, len(data), 32))
        cases.append(("actual-candidate-table-not-installed", data, 0x400000, 1, count))
    with tempfile.TemporaryDirectory(prefix="board-layout-") as temporary:
        temporary = Path(temporary)
        for opt in ("0", "2"):
            executable = temporary / ("test-O" + opt)
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-O" + opt, "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I" + str(APP), str(APP / "layout.c"),
                            str(Path(__file__).with_suffix(".c")), "-o", str(executable)], check=True)
            for name, data, capacity, expected, count in cases:
                fixture = temporary / name
                fixture.write_bytes(data)
                subprocess.run([str(executable), str(fixture), str(capacity), str(expected), str(count)], check=True)
            print(f"O{opt}: {len(cases)} layout positive and counter-probes passed")


if __name__ == "__main__":
    main()
