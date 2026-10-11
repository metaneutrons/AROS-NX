#!/usr/bin/env python3
"""Check board.mk, C profiles and ESP-IDF flash layout inputs.

usage: check-profile.py [--board NAME] [--layout legacy|ab] [--counter-probe]

By default all three boards are checked against both layouts. Make is
evaluated for each pair so no other profile's branch can supply its values.
"""

import argparse
import csv
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
BOARD = Path(__file__).resolve().parent
BOARDS = ("d1001", "jc1060p470c-v1", "jc1060p470c-v2")
LAYOUTS = ("legacy", "ab")
MAKE_VARIABLES = (
    "P4_BOARD", "P4_FLASH_LAYOUT", "P4_BOARD_DIR", "P4_BOARD_CPPFLAGS",
    "P4_BOARD_LAYOUT_FLAGS", "P4_BOARD_FLASH_SIZE", "P4_BOARD_REV_MIN",
    "P4_BOARD_REV_MAX", "P4_BOARD_BOOTLOADER_OFFSET",
    "P4_BOARD_PARTITION_OFFSET", "P4_BOARD_PKG_LIMIT",
    "P4_BOARD_CORE_LIMIT", "P4_BOARD_BOOT_REGION_BYTES",
    "P4_BOARD_KERNEL_SLOT_BYTES", "P4_BOARD_BSP_SLOT_BYTES",
    "P4_BOARD_FLASHDISK_SIZE_MB", "P4_BOARD_FLASHDISK_SIZE_BYTES",
    "P4_BOARD_FLASHDISK_OFFSET", "P4_BOARD_TOUCH_FW_FILENAME",
    "P4_BOARD_PARTITION_CSV", "P4_BOARD_SDKCONFIG",
    "P4_BOARD_TOUCH_MODULE", "P4_BOARD_SETUP",
)
MIB = 1024 * 1024
LOW_FLASH_LIMIT = 16 * MIB

LEGACY_ROWS = {
    "d1001": {
        "nvs": ("data", "nvs", 0x9000, 0x6000),
        "nvs_key": ("data", "nvs_keys", 0xF000, 0x1000),
        "otadata": ("data", "ota", 0x10000, 0x2000),
        "phy_init": ("data", "phy", 0x12000, 0x1000),
        "ota_0": ("app", "ota_0", 0x20000, 0x800000),
        "arosbsp": ("0x40", "0x00", 0x820000, 0x7E0000),
        "storage": ("data", "fat", 0x1020000, 0xFE0000),
    },
    "jc1060": {
        "nvs": ("data", "nvs", 0x9000, 0x6000),
        "nvs_key": ("data", "nvs_keys", 0xF000, 0x1000),
        "otadata": ("data", "ota", 0x10000, 0x2000),
        "phy_init": ("data", "phy", 0x12000, 0x1000),
        "ota_0": ("app", "ota_0", 0x20000, 0x800000),
        "arosbsp": ("0x40", "0x00", 0x820000, 0x7E0000),
    },
}

AB_ROWS = {
    "nvs": ("data", "nvs", 0x21000, 0x6000),
    "nvs_key": ("data", "nvs_keys", 0x27000, 0x1000),
    "otadata": ("data", "ota", 0x28000, 0x2000),
    "phy_init": ("data", "phy", 0x2A000, 0x1000),
    "KERNEL_A": ("app", "ota_0", 0x40000, 0x80000),
    "KERNEL_B": ("app", "ota_1", 0xC0000, 0x80000),
    "BSP_A": ("0x40", "0x00", 0x140000, 0x600000),
    "BSP_B": ("0x40", "0x01", 0x740000, 0x600000),
    "DEVELOP": ("0x41", "0x00", 0xD40000, 0x2C0000),
}


def number(value: str) -> int:
    return int(value.strip().rstrip("ULul"), 0)


def flash_bytes(value: str) -> int:
    match = re.fullmatch(r"(\d+)MB", value.strip())
    if not match:
        raise ValueError(f"unexpected flash size {value!r}")
    return int(match.group(1)) * MIB


def make_values(board: str, layout: str) -> dict[str, str]:
    script = "include %s\nprint:\n%s" % (
        BOARD / "board.mk",
        "".join('\t@echo "%s=$(%s)"\n' % (v, v) for v in MAKE_VARIABLES),
    )
    out = subprocess.run(
        [
            "gmake", "-s", "-f", "-", "print", "P4_BOARD=" + board,
            "P4_FLASH_LAYOUT=" + layout, "SRCDIR=" + str(ROOT),
        ],
        input=script,
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def header_values(make: dict[str, str]) -> dict[str, str]:
    out = subprocess.run(
        ["cc", "-E", "-dM", "-x", "c"]
        + shlex.split(make["P4_BOARD_CPPFLAGS"])
        + [str(BOARD / "board.h")],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return dict(
        re.findall(r"^#define\s+(P4_[A-Z0-9_]+)\s+(.*)$", out, re.MULTILINE)
    )


def sdkconfig_values(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text().splitlines():
        match = re.match(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", line.strip())
        if match:
            values[match.group(1)] = match.group(2).strip()
    return values


def partition_rows(path: Path) -> list[dict[str, object]]:
    rows = []
    for source_line in path.read_text().splitlines():
        if not source_line.strip() or source_line.lstrip().startswith("#"):
            continue
        fields = [field.strip() for field in next(csv.reader([source_line]))]
        if len(fields) < 5:
            raise ValueError(f"malformed partition row in {path}: {source_line}")
        rows.append(
            {
                "name": fields[0],
                "type": fields[1],
                "subtype": fields[2],
                "offset": number(fields[3]),
                "size": number(fields[4]),
                "flags": fields[5] if len(fields) > 5 else "",
            }
        )
    return rows


def same_enum(actual: str, expected: str) -> bool:
    try:
        return number(actual) == number(expected)
    except ValueError:
        return actual.strip().lower() == expected.strip().lower()


def normalized_rows(rows: list[dict[str, object]]) -> dict[str, tuple[str, str, int, int]]:
    return {
        str(row["name"]): (
            str(row["type"]), str(row["subtype"]), int(row["offset"]), int(row["size"])
        )
        for row in rows
    }


def row_sets_match(
    actual: dict[str, tuple[str, str, int, int]],
    expected: dict[str, tuple[str, str, int, int]],
) -> bool:
    if actual.keys() != expected.keys():
        return False
    return all(
        same_enum(actual[name][0], wanted[0])
        and same_enum(actual[name][1], wanted[1])
        and actual[name][2:] == wanted[2:]
        for name, wanted in expected.items()
    )


def macro_number(macros: dict[str, str], name: str) -> int:
    return number(macros[name])


def macro_string(macros: dict[str, str], name: str) -> str:
    return macros[name].strip().strip('"')


def expected_ab_rows(board: str) -> dict[str, tuple[str, str, int, int]]:
    rows = dict(AB_ROWS)
    if board == "d1001":
        rows["storage"] = ("data", "fat", 0x1020000, 0xFE0000)
    return rows


def check(board_name: str, layout: str, counter_probe: bool) -> bool:
    make = make_values(board_name, layout)
    board = make["P4_BOARD"]
    profile = "jc1060" if board.startswith("jc1060") else board
    macros = header_values(make)
    idf = sdkconfig_values(Path(make["P4_BOARD_SDKCONFIG"]))
    table_path = Path(make["P4_BOARD_PARTITION_CSV"])
    rows = partition_rows(table_path)
    labels = [str(row["name"]) for row in rows]
    parts = {str(row["name"]): row for row in rows}
    flash_size = flash_bytes(make["P4_BOARD_FLASH_SIZE"])
    firmware = make["P4_BOARD_TOUCH_FW_FILENAME"]
    expected_file = "partition-table-ab.csv" if layout == "ab" else "partition-table.csv"
    expected_config = "ab-sdkconfig.defaults" if layout == "ab" else "sdkconfig.defaults"

    checks: dict[str, bool] = {
        "profile name": macro_string(macros, "P4_BOARD_NAME") == board,
        "layout selection": make["P4_FLASH_LAYOUT"] == layout,
        "layout compiler flag": (
            ("-DP4_FLASH_LAYOUT_AB=1" in make["P4_BOARD_CPPFLAGS"])
            == (layout == "ab")
            and make["P4_BOARD_LAYOUT_FLAGS"]
            == ("-DP4_FLASH_LAYOUT_AB=1" if layout == "ab" else "")
        ),
        "selected table and sdkconfig": (
            table_path.name == expected_file
            and Path(make["P4_BOARD_SDKCONFIG"]).name == expected_config
            and table_path.parent == Path(make["P4_BOARD_DIR"])
            and Path(make["P4_BOARD_SDKCONFIG"]).parent == Path(make["P4_BOARD_DIR"])
        ),
        "flash size agreement": macro_number(macros, "P4_BOARD_FLASH_BYTES") == flash_size,
        "sdkconfig flash size": idf.get(f"CONFIG_ESPTOOLPY_FLASHSIZE_{flash_size // MIB}MB") == "y",
        "sdkconfig revision floor": idf.get(
            "CONFIG_ESP32P4_REV_MIN_" + make["P4_BOARD_REV_MIN"]
        ) == "y",
        "sdkconfig pre-v3 selection": (
            idf.get("CONFIG_ESP32P4_SELECTS_REV_LESS_V3") == "y"
        ) == (number(make["P4_BOARD_REV_MAX"]) < 300),
        "partition-table offset agreement": (
            macro_number(macros, "P4_BOARD_PARTITION_OFFSET")
            == number(make["P4_BOARD_PARTITION_OFFSET"])
            == number(idf["CONFIG_PARTITION_TABLE_OFFSET"])
        ),
        "bootloader offset": (
            macro_number(macros, "P4_BOARD_BOOTLOADER_OFFSET")
            == number(make["P4_BOARD_BOOTLOADER_OFFSET"])
            == 0x2000
        ),
        "flashdisk byte size agreement": (
            macro_number(macros, "P4_BOARD_FLASHDISK_SIZE")
            == macro_number(macros, "P4_BOARD_FLASHDISK_SIZE_BYTES")
            == number(make["P4_BOARD_FLASHDISK_SIZE_BYTES"])
        ),
        "package limit agreement": (
            number(make["P4_BOARD_PKG_LIMIT"]) == macro_number(macros, "P4_BOARD_PKG_LIMIT")
        ),
        "core limit": number(make["P4_BOARD_CORE_LIMIT"]) == (
            0x80000 if layout == "ab" else 0x800000
        ),
        "unique partition labels": len(labels) == len(set(labels)) and all(labels),
        "partition extents fit flash": all(
            int(row["offset"]) >= 0
            and int(row["size"]) > 0
            and int(row["offset"]) + int(row["size"]) <= flash_size
            for row in rows
        ),
        "partitions do not overlap": all(
            int(left["offset"]) + int(left["size"]) <= int(right["offset"])
            for left, right in zip(sorted(rows, key=lambda row: int(row["offset"])),
                                   sorted(rows, key=lambda row: int(row["offset"]))[1:])
        ),
        "touch firmware filename": (
            firmware in macros.get("P4_BOARD_TOUCH_FW_PATH", "")
            and firmware in macros.get("P4_BOARD_TOUCH_FW_FALLBACK", "")
        ) if firmware else (
            "P4_BOARD_TOUCH_FW_PATH" not in macros
            and "P4_BOARD_TOUCH_GSL3670" not in macros
        ),
        "board wiring file": (BOARD / (make["P4_BOARD_SETUP"] + ".c")).is_file(),
        "touch controller module": make["P4_BOARD_TOUCH_MODULE"] == (
            "gt911" if "P4_BOARD_TOUCH_GT911" in macros else "gsl3670"
        ),
        "rollback option matches layout": (
            idf.get("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE") == "y"
        ) == (layout == "ab"),
    }

    if counter_probe:
        # Corrupt only the in-memory Make result, leaving all files untouched.
        make["P4_BOARD_FLASHDISK_OFFSET"] = hex(number(make["P4_BOARD_FLASHDISK_OFFSET"]) + 0x1000)

    if layout == "legacy":
        expected = LEGACY_ROWS[profile]
        checks.update({
            "legacy partition rows unchanged": row_sets_match(normalized_rows(rows), expected),
            "legacy BSP type and label": (
                macro_number(macros, "P4_BOARD_BSP_PART_TYPE") == 0x40
                and macro_string(macros, "P4_BOARD_BSP_PART_LABEL") == "arosbsp"
            ),
            "legacy package and volume split": (
                macro_number(macros, "P4_BOARD_PKG_LIMIT") == 0x3E0000
                and macro_number(macros, "P4_BOARD_FLASHDISK_PART_OFFSET") == 0x3E0000
                and macro_number(macros, "P4_BOARD_FLASHDISK_SIZE") == 4 * MIB
                and number(make["P4_BOARD_FLASHDISK_SIZE_MB"]) == 4
                and number(make["P4_BOARD_FLASHDISK_SIZE_BYTES"]) == 4 * MIB
            ),
            "legacy absolute volume offset": (
                number(make["P4_BOARD_FLASHDISK_OFFSET"])
                == int(parts["arosbsp"]["offset"])
                + macro_number(macros, "P4_BOARD_FLASHDISK_PART_OFFSET")
            ),
            "legacy kernel slot and package limits": (
                number(make["P4_BOARD_CORE_LIMIT"]) == int(parts["ota_0"]["size"])
                and number(make["P4_BOARD_PKG_LIMIT"]) == 0x3E0000
                and int(parts["ota_0"]["offset"]) == 0x20000
            ),
            "legacy A/B-only C macros absent": not any(
                name in macros for name in (
                    "P4_BOARD_BOOT_REGION_BYTES", "P4_BOARD_KERNEL_SLOT_BYTES",
                    "P4_BOARD_DEVELOP_OFFSET", "P4_BOARD_KERNEL_SLOT_A_LABEL",
                )
            ),
        })
    else:
        expected = expected_ab_rows(board)
        managed_rows = {
            name: geometry for name, geometry in normalized_rows(rows).items()
            if name != "storage"
        }
        expected_managed = {name: geometry for name, geometry in expected.items()
                            if name != "storage"}
        checks.update({
            "A/B partition rows and exact extents": row_sets_match(
                normalized_rows(rows), expected
            ),
            "A/B package, kernel and BSP slot limits": (
                macro_number(macros, "P4_BOARD_PKG_LIMIT") == 0x600000
                and macro_number(macros, "P4_BOARD_PACKAGE_LIMIT_BYTES") == 0x600000
                and number(make["P4_BOARD_PKG_LIMIT"]) == 0x600000
                and number(make["P4_BOARD_CORE_LIMIT"]) == 0x80000
                and macro_number(macros, "P4_BOARD_KERNEL_SLOT_BYTES") == 0x80000
                and number(make["P4_BOARD_KERNEL_SLOT_BYTES"]) == 0x80000
                and macro_number(macros, "P4_BOARD_BSP_SLOT_BYTES") == 0x600000
                and number(make["P4_BOARD_BSP_SLOT_BYTES"]) == 0x600000
            ),
            "A/B boot prefix": (
                macro_number(macros, "P4_BOARD_BOOT_REGION_BYTES") == 0x40000
                and number(make["P4_BOARD_BOOT_REGION_BYTES"]) == 0x40000
                and number(make["P4_BOARD_PARTITION_OFFSET"]) == 0x20000
            ),
            "A/B slot labels, types and subtypes": (
                macro_string(macros, "P4_BOARD_KERNEL_SLOT_A_LABEL") == "KERNEL_A"
                and macro_string(macros, "P4_BOARD_KERNEL_SLOT_B_LABEL") == "KERNEL_B"
                and macro_string(macros, "P4_BOARD_BSP_SLOT_A_LABEL") == "BSP_A"
                and macro_string(macros, "P4_BOARD_BSP_SLOT_B_LABEL") == "BSP_B"
                and macro_number(macros, "P4_BOARD_KERNEL_PART_TYPE") == 0
                and macro_number(macros, "P4_BOARD_KERNEL_SLOT_A_SUBTYPE") == 0x10
                and macro_number(macros, "P4_BOARD_KERNEL_SLOT_B_SUBTYPE") == 0x11
                and macro_number(macros, "P4_BOARD_BSP_PART_TYPE") == 0x40
                and macro_number(macros, "P4_BOARD_BSP_SLOT_A_SUBTYPE") == 0
                and macro_number(macros, "P4_BOARD_BSP_SLOT_B_SUBTYPE") == 1
            ),
            "independent DEVELOP partition": (
                macro_string(macros, "P4_BOARD_FLASHDISK_PART_LABEL") == "DEVELOP"
                and macro_number(macros, "P4_BOARD_FLASHDISK_PART_TYPE") == 0x41
                and macro_number(macros, "P4_BOARD_FLASHDISK_PART_SUBTYPE") == 0
                and macro_number(macros, "P4_BOARD_FLASHDISK_PART_OFFSET") == 0
                and macro_number(macros, "P4_BOARD_DEVELOP_OFFSET") == 0xD40000
                and macro_number(macros, "P4_BOARD_DEVELOP_SIZE_BYTES") == 0x2C0000
                and number(make["P4_BOARD_FLASHDISK_OFFSET"]) == 0xD40000
                and number(make["P4_BOARD_FLASHDISK_SIZE_BYTES"]) == 0x2C0000
                and make["P4_BOARD_FLASHDISK_SIZE_MB"] == "2.75"
            ),
            "A/B mappings stay in low 16 MiB": all(
                int(row["offset"]) + int(row["size"]) <= LOW_FLASH_LIMIT
                for name, row in parts.items() if name != "storage"
            ),
            "D1001 upper storage retained separately": (
                board != "d1001"
                or ("storage" in parts
                    and int(parts["storage"]["offset"]) == 0x1020000
                    and int(parts["storage"]["size"]) == 0xFE0000
                    and parts["storage"]["type"] == "data"
                    and parts["storage"]["subtype"] == "fat")
            ),
            "A/B storage does not enter DEVELOP or BSP extents": (
                len(managed_rows) == len(expected_managed)
                and all(name != "arosbsp" for name in parts)
            ),
        })

    wifi = macro_number(macros, "P4_BOARD_WIFI_SDIO")
    checks["SDIO Wi-Fi declaration"] = wifi == int(board.startswith("jc1060"))
    if wifi:
        pins = [macro_number(macros, "P4_BOARD_WIFI_" + signal + "_GPIO")
                for signal in ("CLK", "CMD", "D0", "D1", "D2", "D3")]
        checks["Wi-Fi separate slot"] = macro_number(macros, "P4_BOARD_WIFI_SLOT") == 1
        checks["Wi-Fi P4-side reference pins"] = pins == [18, 19, 14, 15, 16, 17]
        checks["Wi-Fi pins distinct from SD storage"] = (
            len(set(pins)) == 6 and not set(pins).intersection(range(39, 45))
        )
        checks["Wi-Fi vendor CHIP_PU reset"] = (
            macro_number(macros, "P4_BOARD_WIFI_RESET_GPIO") == 54
            and macro_number(macros, "P4_BOARD_WIFI_RESET_ACTIVE_LEVEL") == 0
            and 54 not in pins and 54 not in range(39, 45)
        )

    for label, passed in checks.items():
        print(f"{'PASS' if passed else 'FAIL'} {board}/{layout}: {label}")
    return all(checks.values())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--board", choices=BOARDS)
    parser.add_argument("--layout", choices=LAYOUTS)
    parser.add_argument("--counter-probe", action="store_true")
    args = parser.parse_args()

    boards = (args.board,) if args.board else BOARDS
    layouts = (args.layout,) if args.layout else LAYOUTS
    results = [check(board, layout, args.counter_probe)
               for board in boards for layout in layouts]
    if not all(results):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
