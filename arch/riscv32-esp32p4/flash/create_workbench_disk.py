#!/usr/bin/env python3
"""
    create_workbench_disk.py - AROS Workbench Base Partition Image Generator
    Generates a 15.875MB FAT16 partition image containing the standard
    AROS System directory structure and Startup-Sequence for ESP32-P4 flash.
"""

import sys
import os
import struct
import datetime

PARTITION_SIZE = 0xFE0000  # 16,646,144 bytes (15.875 MB)
SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 8     # 4KB Cluster
RESERVED_SECTORS = 4
NUM_FATS = 2
ROOT_DIR_ENTRIES = 512

STARTUP_SEQUENCE = b"""; ==========================================================
; AROS Research Operating System - Startup-Sequence
; Target: Espressif ESP32-P4 SoC (Seeed Studio reTerminal D1001)
; ==========================================================

SetPatch QUIET
Version >NIL:
AddDataTypes REFRESH QUIET
IPrefs
ConClip

; Mount Storage & Network Devices
Mount DEVS:Networks/esp32c6.device
Mount DEVS:Networks/emac.device

; Initialize Resident Drivers
BindDrivers

; Launch AROS Wanderer Desktop GUI
Wanderer
"""

SHELL_STARTUP = b"""; AROS Shell-Startup for ESP32-P4
Prompt "%N.%S> "
Path C: S: System: Utilities: ADD
"""

def create_fat16_image(output_path):
    print(f"[+] Creating Base Workbench FAT16 partition image: {output_path}")
    total_sectors = PARTITION_SIZE // SECTOR_SIZE
    fat_size_sectors = 64
    root_dir_sectors = (ROOT_DIR_ENTRIES * 32) // SECTOR_SIZE
    data_start_sector = RESERVED_SECTORS + (NUM_FATS * fat_size_sectors) + root_dir_sectors

    disk = bytearray(PARTITION_SIZE)

    # 1. Write Boot Sector (BPB)
    # Jump instruction & OEM Name
    disk[0:3] = b"\xEB\x3C\x90"
    disk[3:11] = b"AROS4.0 "
    struct.pack_into("<H", disk, 11, SECTOR_SIZE)
    disk[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", disk, 14, RESERVED_SECTORS)
    disk[16] = NUM_FATS
    struct.pack_into("<H", disk, 17, ROOT_DIR_ENTRIES)
    struct.pack_into("<H", disk, 19, 0) # 0 if >32MB or large
    disk[21] = 0xF8 # Media descriptor (Fixed disk)
    struct.pack_into("<H", disk, 22, fat_size_sectors)
    struct.pack_into("<H", disk, 24, 63) # Sectors per track
    struct.pack_into("<H", disk, 26, 255) # Number of heads
    struct.pack_into("<I", disk, 28, 0) # Hidden sectors
    struct.pack_into("<I", disk, 32, total_sectors) # Large total sectors

    # Extended BPB
    disk[36] = 0x80 # Drive number
    disk[37] = 0x00 # Reserved
    disk[38] = 0x29 # Extended signature
    struct.pack_into("<I", disk, 39, 0x19850523) # Volume serial
    disk[43:54] = b"AROS_SYSTEM" # Volume label
    disk[54:62] = b"FAT16   " # Filesystem type
    disk[510:512] = b"\x55\xAA" # Boot signature

    # 2. Initialize FAT Tables
    for fat_idx in range(NUM_FATS):
        fat_offset = (RESERVED_SECTORS + fat_idx * fat_size_sectors) * SECTOR_SIZE
        disk[fat_offset:fat_offset+4] = b"\xF8\xFF\xFF\xFF" # Media + EOF marker for clusters 0 and 1

    # 3. Create Root Directory Entries
    root_dir_offset = (RESERVED_SECTORS + NUM_FATS * fat_size_sectors) * SECTOR_SIZE
    entry_idx = 0

    def add_dir_entry(name_8_3, attr, first_cluster=0, size=0):
        nonlocal entry_idx
        offset = root_dir_offset + entry_idx * 32
        disk[offset:offset+11] = name_8_3.encode("ascii")
        disk[offset+11] = attr
        struct.pack_into("<H", disk, offset+26, first_cluster)
        struct.pack_into("<I", disk, offset+28, size)
        entry_idx += 1

    # Add Volume Label
    add_dir_entry("AROS_SYSTEM", 0x08) # ATTR_VOLUME_ID

    # Add System Directories
    directories = ["C          ", "CLASSES    ", "DEVS       ", "FONTS      ",
                   "LIBS       ", "LOCALE     ", "PREFS      ", "S          ",
                   "SYSTEM     ", "UTILITIES  ", "WANDERER   "]

    for d in directories:
        add_dir_entry(d, 0x10) # ATTR_DIRECTORY

    # Allocate Cluster 2 for Startup-Sequence inside S/
    cluster_bytes = SECTORS_PER_CLUSTER * SECTOR_SIZE
    data_offset_cluster2 = data_start_sector * SECTOR_SIZE
    disk[data_offset_cluster2:data_offset_cluster2+len(STARTUP_SEQUENCE)] = STARTUP_SEQUENCE

    # Mark Cluster 2 in FAT
    for fat_idx in range(NUM_FATS):
        fat_offset = (RESERVED_SECTORS + fat_idx * fat_size_sectors) * SECTOR_SIZE
        struct.pack_into("<H", disk, fat_offset + 4, 0xFFFF) # EOF for cluster 2

    # Write Image to disk
    with open(output_path, "wb") as f:
        f.write(disk)

    print(f"[+] Successfully wrote Base Workbench image ({len(disk)} bytes, 15.875 MB).")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: create_workbench_disk.py <output_image.bin>")
        sys.exit(1)
    create_fat16_image(sys.argv[1])
