#!/usr/bin/env python3
"""Check the duplicated C, Make and ESP-IDF values of each board profile.

usage: check-profile.py [--board NAME] [--counter-probe]

Without --board every supported profile is checked. The Make values are
taken by evaluating board.mk with GNU make for that board, so a second
profile's branch can never be read in place of the first.
"""

from pathlib import Path
import csv
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
BOARD = Path(__file__).resolve().parent
BOARDS = ["d1001", "jc1060p470c-v1", "jc1060p470c-v2"]
VARIABLES = [
    "P4_BOARD", "P4_BOARD_CPPFLAGS", "P4_BOARD_FLASH_SIZE",
    "P4_BOARD_REV_MIN", "P4_BOARD_REV_MAX", "P4_BOARD_PARTITION_OFFSET",
    "P4_BOARD_PKG_LIMIT", "P4_BOARD_FLASHDISK_SIZE_MB",
    "P4_BOARD_FLASHDISK_OFFSET", "P4_BOARD_TOUCH_FW_FILENAME",
    "P4_BOARD_PARTITION_CSV", "P4_BOARD_SDKCONFIG",
    "P4_BOARD_TOUCH_MODULE", "P4_BOARD_SETUP",
]


def number(value: str) -> int:
    return int(value.rstrip("ULul"), 0)


def make_values(board: str) -> dict[str, str]:
    script = "include %s\nprint:\n%s" % (
        BOARD / "board.mk",
        "".join('\t@echo "%s=$(%s)"\n' % (v, v) for v in VARIABLES))
    out = subprocess.run(
        ["gmake", "-s", "-f", "-", "print", "P4_BOARD=" + board,
         "SRCDIR=" + str(ROOT)],
        input=script, capture_output=True, text=True, check=True).stdout
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def header_values(make: dict[str, str]) -> dict[str, str]:
    # The profile as the compiler sees it: board.h with this board's
    # P4_BOARD_CPPFLAGS through the host preprocessor, so variant
    # conditionals and board.h's own checks apply exactly as in a build.
    out = subprocess.run(
        ["cc", "-E", "-dM", "-x", "c", str(BOARD / "board.h")]
        + make["P4_BOARD_CPPFLAGS"].split(),
        capture_output=True, text=True, check=True).stdout
    return dict(re.findall(r"^#define\s+(P4_[A-Z0-9_]+)\s+(.*)$",
                           out, re.MULTILINE))


def check(board: str, counter_probe: bool) -> bool:
    make = make_values(board)
    board = make["P4_BOARD"]
    if counter_probe:
        # Deliberately corrupt only the in-memory candidate; no source changes.
        make["P4_BOARD_FLASHDISK_OFFSET"] = "0xc10000"
    header = header_values(make)
    idf = dict(re.findall(r"^([A-Z0-9_]+)=(\S+)",
                          Path(make["P4_BOARD_SDKCONFIG"]).read_text(),
                          re.MULTILINE))
    table = Path(make["P4_BOARD_PARTITION_CSV"])
    rows = list(csv.reader(line for line in table.read_text().splitlines()
                           if line.strip() and not line.lstrip().startswith("#")))
    rows = [[field.strip() for field in row] for row in rows]
    parts = {row[0]: row for row in rows}
    flash_mb = number(make["P4_BOARD_FLASH_SIZE"].removesuffix("MB"))
    fw = make["P4_BOARD_TOUCH_FW_FILENAME"]

    checks = {
        "profile name": header["P4_BOARD_NAME"].strip('"') == board,
        "flash size": number(header["P4_BOARD_FLASH_BYTES"])
        == flash_mb * 1024 * 1024,
        "sdkconfig flash size":
        idf.get("CONFIG_ESPTOOLPY_FLASHSIZE_%dMB" % flash_mb) == "y",
        "sdkconfig revision floor":
        idf.get("CONFIG_ESP32P4_REV_MIN_%s" % make["P4_BOARD_REV_MIN"]) == "y",
        "sdkconfig pre-v3 gate": (number(make["P4_BOARD_REV_MAX"]) >= 300)
        != (idf.get("CONFIG_ESP32P4_SELECTS_REV_LESS_V3") == "y"),
        "partition-table offset": number(header["P4_BOARD_PARTITION_OFFSET"])
        == number(make["P4_BOARD_PARTITION_OFFSET"])
        == number(idf["CONFIG_PARTITION_TABLE_OFFSET"]),
        "BSP type": number(header["P4_BOARD_BSP_PART_TYPE"])
        == number(parts["arosbsp"][1]),
        "BSP label": header["P4_BOARD_BSP_PART_LABEL"].strip('"') == "arosbsp",
        "core at 0x20000": number(parts["ota_0"][3]) == 0x20000,
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
        "BSP within 16 MB cache map": number(parts["arosbsp"][3])
        + number(parts["arosbsp"][4]) <= 0x1000000,
        "flash layout fits chip": all(
            number(row[3]) + number(row[4])
            <= number(header["P4_BOARD_FLASH_BYTES"]) for row in rows),
        "touch firmware filename": (
            fw in header.get("P4_BOARD_TOUCH_FW_PATH", "")
            and fw in header.get("P4_BOARD_TOUCH_FW_FALLBACK", ""))
        if fw else ("P4_BOARD_TOUCH_FW_PATH" not in header
                    and "P4_BOARD_TOUCH_GSL3670" not in header),
        # esp32p4board.resource links the board's wiring file, and the
        # package carries the driver for the profile's controller.
        "board wiring file": (BOARD / (make["P4_BOARD_SETUP"] + ".c"))
        .is_file(),
        "touch controller module": make["P4_BOARD_TOUCH_MODULE"]
        == ("gt911" if "P4_BOARD_TOUCH_GT911" in header else "gsl3670"),
    }

    for label, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {board}: {label}")
    return all(checks.values())


def main() -> None:
    args = sys.argv[1:]
    counter_probe = "--counter-probe" in args
    args = [a for a in args if a != "--counter-probe"]
    boards = BOARDS
    if args[:1] == ["--board"] and len(args) == 2:
        boards = [args[1]]
    elif args:
        raise SystemExit(__doc__.strip().splitlines()[2])
    results = [check(board, counter_probe) for board in boards]
    if not all(results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
