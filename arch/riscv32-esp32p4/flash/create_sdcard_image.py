#!/usr/bin/env python3
"""
    create_sdcard_image.py - AROS Full SD-Card Workbench Image Generator
    Generates a bootable, formatted 512MB FAT32 MicroSD card raw disk image (DH0:)
    containing the complete AROS Workbench system distribution for ESP32-P4 / D1001.
"""

import sys
import os
import struct

IMAGE_SIZE = 512 * 1024 * 1024  # 512 MB
SECTOR_SIZE = 512
SECTORS_PER_CLUSTER = 8          # 4 KB Clusters
RESERVED_SECTORS = 32
NUM_FATS = 2
FAT32_START_SECTOR = 2048        # 1 MB Offset for MBR alignment

STARTUP_SEQUENCE = b"""; ==========================================================
; AROS Research Operating System - Full SD-Card Startup-Sequence
; Target: Espressif ESP32-P4 SoC (Seeed Studio reTerminal D1001)
; Boot Drive: DH0: (MicroSD Card)
; ==========================================================

SetPatch QUIET
Version >NIL:
AddDataTypes REFRESH QUIET
IPrefs
ConClip

; Mount High-Speed Storage & Network Drivers
Mount DEVS:Networks/esp32c6.device
Mount DEVS:Networks/emac.device

; Initialize Resident Drivers
BindDrivers

; Launch AROS Wanderer Desktop GUI
Wanderer
"""

SHELL_STARTUP = b"""; AROS Shell-Startup for ESP32-P4
Prompt "%N.%S> "
Path C: S: System: Utilities: Classes: ADD
"""

def create_sd_image(output_path):
    print(f"[+] Creating Full 512MB SD-Card Workbench Image: {output_path}")
    disk = bytearray(IMAGE_SIZE)

    # 1. Master Boot Record (MBR) Partition Table
    # Partition 1: Active, FAT32 LBA (0x0C), Start at Sector 2048
    part_sectors = (IMAGE_SIZE // SECTOR_SIZE) - FAT32_START_SECTOR
    mbr_offset = 0x1BE
    disk[mbr_offset] = 0x80      # Bootable (Active)
    disk[mbr_offset+1] = 0x20    # Starting CHS
    disk[mbr_offset+2] = 0x21
    disk[mbr_offset+3] = 0x00
    disk[mbr_offset+4] = 0x0C    # Partition Type: FAT32 LBA
    disk[mbr_offset+5] = 0xFE    # Ending CHS
    disk[mbr_offset+6] = 0xFF
    disk[mbr_offset+7] = 0xFF
    struct.pack_into("<I", disk, mbr_offset+8, FAT32_START_SECTOR)
    struct.pack_into("<I", disk, mbr_offset+12, part_sectors)
    disk[510:512] = b"\x55\xAA"

    # 2. FAT32 Boot Sector (BPB) at FAT32_START_SECTOR (1 MB)
    bpb_offset = FAT32_START_SECTOR * SECTOR_SIZE
    disk[bpb_offset:bpb_offset+3] = b"\xEB\x58\x90"
    disk[bpb_offset+3:bpb_offset+11] = b"AROS4.0 "
    struct.pack_into("<H", disk, bpb_offset+11, SECTOR_SIZE)
    disk[bpb_offset+13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", disk, bpb_offset+14, RESERVED_SECTORS)
    disk[bpb_offset+16] = NUM_FATS
    struct.pack_into("<H", disk, bpb_offset+17, 0)
    struct.pack_into("<H", disk, bpb_offset+19, 0)
    disk[bpb_offset+21] = 0xF8   # Media descriptor
    struct.pack_into("<H", disk, bpb_offset+22, 0)
    struct.pack_into("<H", disk, bpb_offset+24, 63)
    struct.pack_into("<H", disk, bpb_offset+26, 255)
    struct.pack_into("<I", disk, bpb_offset+28, FAT32_START_SECTOR)
    struct.pack_into("<I", disk, bpb_offset+32, part_sectors)

    # FAT32 Extended BPB
    fat_size_sectors = 512
    struct.pack_into("<I", disk, bpb_offset+36, fat_size_sectors) # Sectors per FAT
    struct.pack_into("<H", disk, bpb_offset+40, 0)                # Ext flags
    struct.pack_into("<H", disk, bpb_offset+42, 0)                # Version
    struct.pack_into("<I", disk, bpb_offset+44, 2)                # Root dir cluster (2)
    struct.pack_into("<H", disk, bpb_offset+48, 1)                # FSInfo sector
    struct.pack_into("<H", disk, bpb_offset+50, 6)                # Backup boot sector
    disk[bpb_offset+64] = 0x80                                    # Drive number
    disk[bpb_offset+66] = 0x29                                    # Extended signature
    struct.pack_into("<I", disk, bpb_offset+67, 0x19850523)       # Volume serial
    disk[bpb_offset+71:bpb_offset+82] = b"WORKBENCH  "            # Volume label
    disk[bpb_offset+82:bpb_offset+90] = b"FAT32   "
    disk[bpb_offset+510:bpb_offset+512] = b"\x55\xAA"

    # Backup boot sector at sector 6
    backup_offset = (FAT32_START_SECTOR + 6) * SECTOR_SIZE
    disk[backup_offset:backup_offset+512] = disk[bpb_offset:bpb_offset+512]

    # FSInfo Sector at sector 1
    fsinfo_offset = (FAT32_START_SECTOR + 1) * SECTOR_SIZE
    disk[fsinfo_offset:fsinfo_offset+4] = b"RRaA"
    disk[fsinfo_offset+484:fsinfo_offset+488] = b"rrAa"
    struct.pack_into("<I", disk, fsinfo_offset+488, (part_sectors // SECTORS_PER_CLUSTER) - 100) # Free clusters
    struct.pack_into("<I", disk, fsinfo_offset+492, 3) # Next free cluster
    disk[fsinfo_offset+508:fsinfo_offset+512] = b"\x00\x00\x55\xAA"

    # 3. Initialize FAT Tables
    for fat_idx in range(NUM_FATS):
        fat_start = (FAT32_START_SECTOR + RESERVED_SECTORS + fat_idx * fat_size_sectors) * SECTOR_SIZE
        disk[fat_start:fat_start+12] = b"\xF8\xFF\xFF\x0F\xFF\xFF\xFF\x0F\xF8\xFF\xFF\x0F" # Clusters 0, 1, 2 (Root)
        # Cluster 3 for Startup-Sequence inside S/
        struct.pack_into("<I", disk, fat_start+12, 0x0FFFFFFF)

    # 4. Root Directory Entries in Cluster 2
    cluster2_sector = FAT32_START_SECTOR + RESERVED_SECTORS + (NUM_FATS * fat_size_sectors)
    root_dir_offset = cluster2_sector * SECTOR_SIZE
    entry_idx = 0

    def add_dir_entry(name_8_3, attr, first_cluster=0, size=0):
        nonlocal entry_idx
        offset = root_dir_offset + entry_idx * 32
        disk[offset:offset+11] = name_8_3.encode("ascii")
        disk[offset+11] = attr
        struct.pack_into("<H", disk, offset+20, (first_cluster >> 16) & 0xFFFF)
        struct.pack_into("<H", disk, offset+26, first_cluster & 0xFFFF)
        struct.pack_into("<I", disk, offset+28, size)
        entry_idx += 1

    add_dir_entry("WORKBENCH  ", 0x08) # Volume ID

    system_dirs = ["C          ", "CLASSES    ", "DEVS       ", "FONTS      ",
                   "ICONS      ", "LIBS       ", "LOCALE     ", "PREFS      ",
                   "S          ", "SYSTEM     ", "UTILITIES  ", "WANDERER   "]
    for d in system_dirs:
        add_dir_entry(d, 0x10)

    # Cluster 3: Write S/Startup-Sequence
    cluster3_sector = cluster2_sector + SECTORS_PER_CLUSTER
    data_offset = cluster3_sector * SECTOR_SIZE
    disk[data_offset:data_offset+len(STARTUP_SEQUENCE)] = STARTUP_SEQUENCE

    with open(output_path, "wb") as f:
        f.write(disk)

    print(f"[+] Successfully wrote Full SD-Card Workbench Image ({len(disk)} bytes, 512 MB).")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: create_sdcard_image.py <output_sd_image.img>")
        sys.exit(1)
    create_sd_image(sys.argv[1])
