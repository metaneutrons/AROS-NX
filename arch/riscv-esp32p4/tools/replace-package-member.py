#!/usr/bin/env python3
"""Replace one PKG v1 member, preserving every other record byte-for-byte.

Accepts an exact package or a padded flash readback. Never writes the input.
"""
import argparse
import hashlib
import pathlib
import struct


def replace(source, member, replacement):
    if source[:4] != b"PKG\x01":
        raise ValueError("not a PKG v1 package")
    size = struct.unpack_from(">I", source, 4)[0]
    if not 8 <= size <= min(len(source), 0x3e0000):
        raise ValueError("invalid package size")
    offset, matches, records = 8, 0, []
    while offset < size:
        start = offset
        plen = struct.unpack_from(">I", source, offset)[0]
        offset += 4
        if offset + plen + 5 > size:
            raise ValueError("invalid name length")
        name = source[offset:offset + plen].rstrip(b"\0").decode("utf-8")
        offset += plen + 1
        dlen = struct.unpack_from(">I", source, offset)[0]
        length_offset = offset
        offset += 4
        end = offset + dlen
        if end > size:
            raise ValueError("invalid data length")
        if name == member:
            matches += 1
            records.append(source[start:length_offset]
                           + struct.pack(">I", len(replacement)) + replacement)
        else:
            records.append(source[start:end])
        offset = end
    if offset != size or matches != 1:
        raise ValueError("expected exactly one matching member")
    payload = b"".join(records)
    result = b"PKG\x01" + struct.pack(">I", 8 + len(payload)) + payload
    if len(result) > 0x3e0000:
        raise ValueError("replacement would overlap the flash volume")
    return result, len(records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("member")
    parser.add_argument("replacement", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    result, count = replace(args.source.read_bytes(), args.member,
                            args.replacement.read_bytes())
    # Exclusive creation prevents accidentally overwriting a backup.
    with args.output.open("xb") as output:
        output.write(result)
    print(f"{count} members; replaced only {args.member}; {len(result)} bytes; "
          f"SHA256 {hashlib.sha256(result).hexdigest()}")
