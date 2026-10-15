#!/usr/bin/env python3
"""Decode an already captured 4KiB HS diagnostic sector; never opens hardware."""
import argparse
from pathlib import Path
import struct
import zlib


def decode(data):
    if len(data) != 4096 or data[:4] != b"HSJ1":
        raise ValueError("missing/truncated journal (not a captured HS result)")
    version, length, crc = struct.unpack_from("<III", data, 4)
    if version != 1 or not 1 <= length <= 768:
        raise ValueError("unsupported journal version/length")
    payload = data[16:16 + length]
    if zlib.crc32(payload) != crc or data[16 + length:] != b"\xff" * (4096 - 16 - length):
        raise ValueError("corrupt/partial journal; no result")
    if any(c != 10 and not 32 <= c < 127 for c in payload):
        raise ValueError("unexpected control/non-ASCII bytes in register dump")
    text = payload.decode("ascii")
    if not text.startswith("[HSReference] ") or not text.endswith("\n"):
        raise ValueError("not a complete reference register record")
    return text


def self_test():
    text = b"[HSReference] sample=6 mounted=0 mode=DMA\n"
    data = b"HSJ1" + struct.pack("<III", 1, len(text), zlib.crc32(text)) + text
    data += b"\xff" * (4096 - len(data))
    assert decode(data) == text.decode("ascii")
    for bad in (data[:-1], b"\xff" * 4096, data[:16] + b"X" + data[17:],
                data[:-1] + b"\x00", data[:4] + struct.pack("<I", 2) + data[8:]):
        try:
            decode(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid journal accepted")
    print("PASS journal decoder: valid record and five corrupt/incompatible cases")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sector", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.sector:
        with args.sector.open("rb") as stream:
            data = stream.read(4097)
        try:
            print(decode(data), end="")
        except ValueError as error:
            parser.exit(2, str(error) + "\n")
    else:
        parser.error("supply a saved sector or --self-test")


if __name__ == "__main__":
    main()
