#!/usr/bin/env python3
"""Check the duplicated C, Make and ESP-IDF values of the D1001 profile."""

from pathlib import Path
import csv
import re
import sys


ROOT = Path(__file__).resolve().parents[3]
BOARD = Path(__file__).resolve().parent


def assignments(path: Path, pattern: str) -> dict[str, str]:
    return dict(re.findall(pattern, path.read_text(), re.MULTILINE))


def number(value: str) -> int:
    return int(value.rstrip("ULul"), 0)


make = assignments(BOARD / "board.mk", r"^([A-Z0-9_]+)\s*:?=\s*(\S+)")
if sys.argv[1:] == ["--counter-probe"]:
    # Deliberately corrupt only the in-memory candidate; no source is changed.
    make["P4_BOARD_FLASHDISK_OFFSET"] = "0xc10000"
elif sys.argv[1:]:
    raise SystemExit("usage: check-profile.py [--counter-probe]")
header = assignments(BOARD / "d1001.h", r"^#define\s+([A-Z0-9_]+)\s+([^\s]+)")
idf = assignments(
    ROOT / "arch/riscv-esp32p4/bootloader/project/sdkconfig.defaults",
    r"^([A-Z0-9_]+)=(\S+)",
)
table = ROOT / "arch/riscv-esp32p4/bootloader/partition-table.csv"
rows = list(csv.reader(line for line in table.read_text().splitlines()
                       if line.strip() and not line.lstrip().startswith("#")))
parts = {row[0].strip(): [field.strip() for field in row] for row in rows}

checks = {
    "flash size": number(header["P4_BOARD_FLASH_BYTES"])
    == number(make["P4_BOARD_FLASH_SIZE"].removesuffix("MB")) * 1024 * 1024,
    "partition-table offset": number(header["P4_BOARD_PARTITION_OFFSET"])
    == number(make["P4_BOARD_PARTITION_OFFSET"])
    == number(idf["CONFIG_PARTITION_TABLE_OFFSET"]),
    "BSP type": number(header["P4_BOARD_BSP_PART_TYPE"])
    == number(parts["arosbsp"][1]),
    "BSP label": header["P4_BOARD_BSP_PART_LABEL"].strip('"') == "arosbsp",
    "package limit": number(make["P4_BOARD_PKG_LIMIT"])
    == number(header["P4_BOARD_FLASHDISK_PART_OFFSET"]),
    "flashdisk location": number(parts["arosbsp"][3])
    + number(header["P4_BOARD_FLASHDISK_PART_OFFSET"])
    == number(make["P4_BOARD_FLASHDISK_OFFSET"]),
    "flashdisk size": number(header["P4_BOARD_FLASHDISK_SIZE"])
    == number(make["P4_BOARD_FLASHDISK_SIZE_MB"]) * 1024 * 1024,
    "flashdisk fits BSP": number(header["P4_BOARD_FLASHDISK_PART_OFFSET"])
    + number(header["P4_BOARD_FLASHDISK_SIZE"])
    <= number(parts["arosbsp"][4]),
    "flash layout fits chip": all(
        number(row[3]) + number(row[4]) <= number(header["P4_BOARD_FLASH_BYTES"])
        for row in rows
    ),
    "touch firmware filename": make["P4_BOARD_TOUCH_FW_FILENAME"]
    in header["P4_BOARD_TOUCH_FW_PATH"]
    and make["P4_BOARD_TOUCH_FW_FILENAME"]
    in header["P4_BOARD_TOUCH_FW_FALLBACK"],
}

for label, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'} {label}")

if not all(checks.values()):
    raise SystemExit(1)
