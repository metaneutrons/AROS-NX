#!/usr/bin/env python3
"""
    Merge tool to generate a 32MB single flashable image for ESP32-P4 AROS.
    Layout:
      0x000000: bootloader.bin
      0x010000: partitions.bin
      0x011000: otadata.bin
      0x020000: aros_a.bin
"""

import sys
import os

TOTAL_SIZE = 32 * 1024 * 1024  # 32MB

def merge(output_path, bootloader_path, partitions_path, otadata_path, aros_path):
    print(f"Creating 32MB merged image: {output_path}")
    image = bytearray([0xFF] * TOTAL_SIZE)

    def write_at(offset, file_path):
        if file_path and os.path.exists(file_path):
            with open(file_path, "rb") as f:
                data = f.read()
                image[offset:offset+len(data)] = data
                print(f"  [+] Wrote {file_path} ({len(data)} bytes) at offset 0x{offset:06X}")
        else:
            print(f"  [-] Skipped {file_path} (not found)")

    # ESP32-P4 bootloader offset is standardly 0x002000 (with 0x0 fallback)
    write_at(0x002000, bootloader_path)
    write_at(0x010000, partitions_path)
    write_at(0x019000, otadata_path)
    write_at(0x020000, aros_path)

    with open(output_path, "wb") as out:
        out.write(image)
    print(f"Successfully generated {output_path} ({len(image)} bytes).")

if __name__ == "__main__":
    if len(sys.argv) < 6:
        print("Usage: merge_image.py <output.bin> <bootloader.bin> <partitions.bin> <otadata.bin> <aros_a.bin>")
        sys.exit(1)
    merge(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
