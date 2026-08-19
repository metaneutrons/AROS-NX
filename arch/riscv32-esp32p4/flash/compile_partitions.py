#!/usr/bin/env python3
"""
    ESP-IDF Compatible Partition Table Binary Generator.
    Converts partitions.csv to partitions.bin with MD5 checksum.
"""

import sys
import os
import struct
import hashlib

PARTITION_MAGIC = 0x50AA
MD5_MAGIC = 0xEBEB

TYPE_MAP = {
    "app": 0x00,
    "data": 0x01
}

SUBTYPE_MAP = {
    "factory": 0x00,
    "ota_0": 0x10,
    "ota_1": 0x11,
    "ota_2": 0x12,
    "ota": 0x00,
    "phy": 0x01,
    "nvs": 0x02,
    "nvs_keys": 0x04,
    "fat": 0x81,
    "spiffs": 0x82
}

def parse_int(s):
    s = s.strip()
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    elif s.endswith("k") or s.endswith("K"):
        return int(s[:-1]) * 1024
    elif s.endswith("m") or s.endswith("M"):
        return int(s[:-1]) * 1024 * 1024
    return int(s)

def compile_partitions(csv_path, bin_path):
    table = bytearray()
    
    with open(csv_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            
            parts = [p.strip() for p in line.split(",")]
            if len(parts) < 5:
                continue
            
            name = parts[0][:16].encode("utf-8")
            name_padded = name + b"\x00" * (16 - len(name))
            
            ptype_str = parts[1]
            psubtype_str = parts[2]
            
            ptype = TYPE_MAP.get(ptype_str, 0x01)
            psubtype = SUBTYPE_MAP.get(psubtype_str, 0x00)
            
            offset = parse_int(parts[3])
            size = parse_int(parts[4])
            flags = 0
            if len(parts) >= 6 and parts[5]:
                if "encrypted" in parts[5]:
                    flags |= 1

            # Format: <2B magic><1B type><1B subtype><4B offset><4B size><16B name><4B flags> = 32 bytes
            entry = struct.pack("<HBBII16sI", PARTITION_MAGIC, ptype, psubtype, offset, size, name_padded, flags)
            table.extend(entry)

    # Add MD5 checksum entry (0xEBEB)
    md5 = hashlib.md5(table).digest()
    md5_entry = struct.pack("<HH28s", MD5_MAGIC, 0x0000, md5 + b"\xFF" * 12)
    table.extend(md5_entry)

    # Pad table to 0xC00 (3KB standard table partition size)
    if len(table) < 0xC00:
        table.extend(b"\xFF" * (0xC00 - len(table)))

    with open(bin_path, "wb") as f:
        f.write(table)
    
    print(f"Generated partition table binary: {bin_path} ({len(table)} bytes)")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: compile_partitions.py <partitions.csv> <partitions.bin>")
        sys.exit(1)
    compile_partitions(sys.argv[1], sys.argv[2])
