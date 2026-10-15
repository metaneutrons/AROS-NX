#!/usr/bin/env python3
"""Decode an already captured 4KiB HS diagnostic sector; never opens hardware."""
import argparse
from pathlib import Path
import struct
import zlib


def decode(data):
    if len(data) == 4096 and data[:4] == b"HST2":
        return decode_trace(data)
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


def decode_trace(data):
    """Accept only a contiguous, independently CRC-checked record prefix.

    A torn later write is an explicit error, retaining the valid prefix in
    the diagnostic rather than silently presenting a complete trace.
    """
    if len(data) != 4096:
        raise ValueError("truncated staged trace")
    records = []
    for sequence in range(16):
        record = data[sequence * 256:(sequence + 1) * 256]
        if record == b"\xff" * 256:
            if data[sequence * 256:] != b"\xff" * (4096 - sequence * 256):
                raise ValueError("non-contiguous staged trace")
            break
        seq, length, reserved = struct.unpack_from("<III", record, 4)
        crc, = struct.unpack_from("<I", record, 252)
        if (record[:4] != b"HST2" or seq != sequence or reserved != 0
                or not 1 <= length <= 236 or zlib.crc32(record[:252]) != crc
                or record[16 + length:252] != b"\xff" * (236 - length)):
            raise ValueError(f"torn/corrupt trace at slot {sequence}; valid prefix only:\n"
                             + "".join(records))
        payload = record[16:16 + length]
        if any(c != 10 and not 32 <= c < 127 for c in payload):
            raise ValueError("unexpected control/non-ASCII trace payload")
        records.append(f"record={sequence} " + payload.decode("ascii") + "\n")
    if not records:
        raise ValueError("empty staged trace")
    sample_complete = any(" stage=sample6_complete " in item for item in records)
    status = "sample6_complete" if sample_complete else "valid_prefix_only"
    return f"trace_status={status} records={len(records)} (not a USB/boot pass)\n" + "".join(records)


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
    def record(seq, text):
        value = b"HST2" + struct.pack("<III", seq, len(text), 0) + text
        value += b"\xff" * (252 - len(value))
        return value + struct.pack("<I", zlib.crc32(value))
    staged = record(0, b"stage=app_main_enter\n") + record(1, b"stage=usb_install_enter\n")
    staged += b"\xff" * (4096 - len(staged))
    assert "record=1 stage=usb_install_enter" in decode(staged)
    assert "trace_status=valid_prefix_only" in decode(staged)
    completed = staged[:512] + record(2, b"stage=sample6_complete ok=1\n") + staged[768:]
    assert "trace_status=sample6_complete" in decode(completed)
    for bad in (staged[:-1], staged[:256] + b"X" + staged[257:],
                staged[:256] + b"\xff" * 256 + staged[256:512] + staged[768:],
                record(1, b"wrong sequence\n") + staged[256:]):
        try:
            decode(bad)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid staged trace accepted")
    print("PASS staged decoder: contiguous trace and four corrupt/gap/sequence cases")


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
