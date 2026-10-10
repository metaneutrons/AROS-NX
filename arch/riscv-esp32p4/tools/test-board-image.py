#!/usr/bin/env python3
"""Compile actual image/SHA sources; good/corrupt copies never touch hardware."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "workbench/tools/ESP32Board"
WPA = ROOT / "workbench/network/WirelessManager/src"


def image(chip=13, app=True):
    header = bytearray(24)
    header[0:2] = b"\xe9\x01"
    struct.pack_into("<H", header, 12, chip)
    header[23] = 1
    data = bytearray(256)
    struct.pack_into("<I", data, 0, 0xABCD5432 if app else 0)
    blob = header + struct.pack("<II", 0x42000000, len(data)) + data
    checksum = 0xEF
    for value in data:
        checksum ^= value
    blob += bytes(15 - len(blob) % 16) + bytes([checksum])
    blob += hashlib.sha256(blob).digest()
    return bytes(blob)


def main():
    good = image()
    checksum_bad = bytearray(good)
    checksum_bad[-33] ^= 1
    simple_bad = bytearray(good)
    simple_bad[-1] ^= 1
    huge_segment = bytearray(good)
    struct.pack_into("<I", huge_segment, 28, 0xFFFFFFFF)
    zero_segments = bytearray(good)
    zero_segments[1] = 0
    padding_bad = bytearray(good)
    padding_bad[-34] = 1
    padding_bad[-32:] = hashlib.sha256(padding_bad[:-32]).digest()
    cases = [
        ("good", good, None, 0),
        ("upper-hash", good, hashlib.sha256(good).hexdigest().upper(), 0),
        ("wrong-hash", good, "0" * 64, 2),
        ("bad-hex", good, "g" * 64, 1),
        ("short-hash", good, "0" * 63, 1),
        ("wrong-chip", image(18), None, 4),
        ("bootloader", image(app=False), None, 5),
        ("truncated", good[:-1], None, 3),
        ("trailing", good + b"x", None, 3),
        ("checksum", bytes(checksum_bad), None, 6),
        ("simple-hash", bytes(simple_bad), None, 7),
        ("segment-overflow", bytes(huge_segment), None, 3),
        ("no-segments", bytes(zero_segments), None, 3),
        ("valid-nonzero-padding", bytes(padding_bad), None, 0),
        ("too-short", good[:16], None, 1),
        ("too-large", bytes(1536 * 1024 + 1), None, 1),
    ]
    candidate = Path("/Volumes/Dev/Build/c6-legacy-log-build.8qQvpV/build/network_adapter.bin")
    if candidate.exists():
        data = candidate.read_bytes()
        # Exact existing private candidate, not an arbitrary matching first file.
        assert len(data) == 1105312
        assert hashlib.sha256(data).hexdigest() == "149ec760ce40bbf159d1d780f001f44de19477b5076e40a91a15764410ce0b86"
        cases.append(("actual-c6-candidate", data, None, 0))
    with tempfile.TemporaryDirectory(prefix="board-image-") as temp:
        temp = Path(temp)
        for opt in ("0", "2"):
            executable = temp / ("test-O" + opt)
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O" + opt,
                            "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I" + str(APP), "-I" + str(WPA / "utils"),
                            "-I" + str(WPA / "crypto"), "-I" + str(WPA),
                            str(APP / "hash.c"), str(APP / "image.c"),
                            str(Path(__file__).with_suffix(".c")), "-o", str(executable)], check=True)
            for name, data, digest, expected in cases:
                fixture = temp / name
                fixture.write_bytes(data)
                digest = digest if digest is not None else hashlib.sha256(data).hexdigest()
                subprocess.run([str(executable), str(fixture), digest, str(expected)], check=True)
            print(f"O{opt}: {len(cases)} image/hash positive and counter-probes passed")


if __name__ == "__main__":
    main()
