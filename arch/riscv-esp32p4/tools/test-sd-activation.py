#!/usr/bin/env python3
"""Compile bounded host tests around generic SD activation and frequency code."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[3]
PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_sd_activation.c"
TOOLS = PORT / "tools"
GENERIC_BUS = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.c"
BUS_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.h"
UNIT_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_unit.h"
BASE_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_base.h"
TAG_HEADER = ROOT / "compiler" / "include" / "utility" / "tagitem.h"
MMC_HEADER = ROOT / "compiler" / "include" / "hardware" / "mmc.h"
SDHC_HEADER = ROOT / "compiler" / "include" / "hardware" / "sdhc.h"
SDSC_SOURCE = ROOT / "rom" / "devs" / "sdcard" / "sdcard_sdscunit.c"
MMC_SOURCE = ROOT / "rom" / "devs" / "sdcard" / "sdcard_mmcunit.c"
HOST_HELPERS = TOOLS / "test-sdmmc-host.py"

MARKERS = (
    "/* INSERT_ACTUAL_SD_CONSTANTS */",
    "/* INSERT_ACTUAL_TAG_ITEM_TYPE */",
    "/* INSERT_ACTUAL_SD_UNIT_TYPE */",
    "/* INSERT_ACTUAL_SD_FUNCTION_MACROS */",
    "/* INSERT_ACTUAL_BUS_TRANSACTION_HELPERS */",
    "/* INSERT_ACTUAL_ACTIVATION_PRODUCTION */",
    "/* INSERT_ACTUAL_FREQUENCY_PRODUCTION */",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_helpers():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "p4_sdmmc_host_test_helpers", HOST_HELPERS
    )
    if spec is None or spec.loader is None:
        raise ValueError(f"could not load extraction helpers from {HOST_HELPERS}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract_constants(path: Path, names: tuple[str, ...]) -> str:
    source = path.read_bytes().decode("latin-1")
    lines: list[str] = []
    for name in names:
        pattern = re.compile(
            rf"(?m)^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b"
            r"[^\n]*(?:\\\n[^\n]*)*"
        )
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise ValueError(
                f"expected one {name} definition in {path}, found {len(matches)}"
            )
        lines.append(matches[0].group(0))
    return "\n".join(lines)


def extract_tag_item_type() -> str:
    source = TAG_HEADER.read_bytes().decode("latin-1")
    typedef = re.search(r"(?m)^typedef\s+ULONG\s+Tag\s*;\s*$", source)
    structure = re.search(
        r"(?s)\bstruct\s+TagItem\s*\{.*?\}\s*"
        r"__attribute__\s*\(\(packed\)\)\s*;",
        source,
    )
    if typedef is None or structure is None:
        raise ValueError(f"could not extract the production TagItem type from {TAG_HEADER}")
    return typedef.group(0) + "\n" + structure.group(0)


def extract_struct(helper, path: Path, name: str) -> str:
    return helper.extract_definition(
        path, f"struct {name}", rf"\bstruct\s+{re.escape(name)}\s*(?=\{{)"
    ) + ";"


def extract_function(helper, path: Path, name: str, signature: str) -> str:
    return helper.extract_definition(path, name, signature)


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    helper = load_helpers()
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in MARKERS:
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    constants = "\n".join(
        (
            extract_constants(
                MMC_HEADER,
                (
                    "MMC_STATUS_MASK",
                    "MMC_STATUS_RDY_FOR_DATA",
                    "MMC_STATUS_STATE_MASK",
                    "MMC_STATUS_STATE_TRAN",
                    "MMC_STATUS_STATE_PRG",
                    "MMC_STATUS_ILLEGAL_COMMAND",
                    "MMC_RSP_PRESENT",
                    "MMC_RSP_CRC",
                    "MMC_RSP_OPCODE",
                    "MMC_RSP_BUSY",
                    "MMC_RSP_R1",
                    "MMC_RSP_R1b",
                    "MMC_CMD_SELECT_CARD",
                    "MMC_CMD_SEND_STATUS",
                    "MMC_CMD_SET_BLOCKLEN",
                    "MMC_CMD_APP_CMD",
                    "MMC_CMD_SEND_EXT_CSD",
                    "MMC_CMD_SWITCH",
                    "MMC_SWITCH_WRITE_BYTE",
                    "EXT_CSD_HS_TIMING",
                    "MMC_HS_52MHZ",
                    "MMC_DATA_READ",
                ),
            ),
            extract_constants(
                SDHC_HEADER,
                (
                    "SDHCI_PRESENT_STATE",
                    "SDHCI_PS_CMD_INHIBIT",
                    "SDHCI_PS_DATA_INHIBIT",
                    "SD_CMD_APP_SET_BUS_WIDTH",
                    "SD_CMD_SWITCH_FUNC",
                    "SD_CMD_APP_SEND_SCR",
                    "SD_SCR_HIGHSPEED",
                    "SD_SCR_DATA4BIT",
                ),
            ),
            extract_constants(
                BASE_HEADER,
                (
                    "SDCARD_TAGBASE",
                    "SDCARD_TAG_CMD",
                    "SDCARD_TAG_ARG",
                    "SDCARD_TAG_RSPTYPE",
                    "SDCARD_TAG_RSP",
                    "SDCARD_TAG_DATA",
                    "SDCARD_TAG_DATALEN",
                    "SDCARD_TAG_DATAFLAGS",
                ),
            ),
            extract_constants(TAG_HEADER, ("TAG_DONE", "TAG_USER")),
            extract_constants(
                UNIT_HEADER,
                (
                    "AB_Card_HighSpeed52",
                    "AB_Card_HighSpeed",
                    "AB_Card_HighCapacity",
                    "AB_Card_4bitData",
                    "AB_Card_MMC",
                    "AF_Card_HighSpeed52",
                    "AF_Card_HighSpeed",
                    "AF_Card_HighCapacity",
                    "AF_Card_4bitData",
                    "AF_Card_MMC",
                ),
            ),
            extract_constants(BUS_HEADER, ("AB_Bus_SPI", "AF_Bus_SPI")),
        )
    )
    unit_type = extract_struct(helper, UNIT_HEADER, "sdcard_Unit")
    function_macros = "\n".join(
        (
            extract_constants(BUS_HEADER, ("FNAME_SDCBUS",)),
            extract_constants(UNIT_HEADER, ("FNAME_SDCUNIT",)),
            extract_constants(
                BUS_HEADER,
                (
                    "SDCBUS_SendCmd",
                    "SDCBUS_WaitCmd",
                    "SDCBUS_SetClock",
                ),
            ),
        )
    )

    transaction_helpers = "\n\n".join(
        (
            extract_function(
                helper,
                BUS_HEADER,
                "SDCBUS_BeginTransaction",
                r"\bstatic\s+inline\s+BOOL\s+SDCBUS_BeginTransaction\s*\(",
            ),
            extract_function(
                helper,
                BUS_HEADER,
                "SDCBUS_EndTransaction",
                r"\bstatic\s+inline\s+void\s+SDCBUS_EndTransaction\s*\(",
            ),
        )
    )
    activation_functions = "\n\n".join(
        (
            extract_function(
                helper,
                GENERIC_BUS,
                "sdcard_StartUnitLocked",
                r"\bstatic\s+BOOL\s+sdcard_StartUnitLocked\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "StartUnit",
                r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*StartUnit\s*\)\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "WaitStatus",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*WaitStatus\s*\)\s*\(",
            ),
        )
    )
    frequency_functions = "\n\n".join(
        (
            extract_function(
                helper,
                GENERIC_BUS,
                "WaitStatus",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*WaitStatus\s*\)\s*\(",
            ),
            extract_function(
                helper,
                SDSC_SOURCE,
                "SDSCSwitch",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*SDSCSwitch\s*\)\s*\(",
            ),
            extract_function(
                helper,
                SDSC_SOURCE,
                "SDSCChangeFrequency",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*SDSCChangeFrequency\s*\)\s*\(",
            ),
            extract_function(
                helper,
                MMC_SOURCE,
                "MMCSwitch",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*MMCSwitch\s*\)\s*\(",
            ),
            extract_function(
                helper,
                MMC_SOURCE,
                "MMCChangeFrequency",
                r"\bULONG\s+FNAME_SDCUNIT\s*\(\s*MMCChangeFrequency\s*\)\s*\(",
            ),
        )
    )

    replacements = {
        MARKERS[0]: constants,
        MARKERS[1]: extract_tag_item_type(),
        MARKERS[2]: unit_type,
        MARKERS[3]: function_macros,
        MARKERS[4]: transaction_helpers,
        MARKERS[5]: activation_functions,
        MARKERS[6]: frequency_functions,
    }
    assembled = fixture
    for marker, text in replacements.items():
        assembled = assembled.replace(marker, text)

    hashes = [
        ("production constants", constants.encode("utf-8")),
        ("production TagItem type", replacements[MARKERS[1]].encode("utf-8")),
        ("production sdcard_Unit type", unit_type.encode("utf-8")),
        ("production dispatch macros", function_macros.encode("utf-8")),
        ("production transaction helpers", transaction_helpers.encode("utf-8")),
        ("production activation functions", activation_functions.encode("utf-8")),
        ("production frequency functions", frequency_functions.encode("utf-8")),
        ("assembled fixture", assembled.encode("utf-8")),
        ("test fixture", FIXTURE.read_bytes()),
    ]
    return assembled, hashes


def audit_public_start(helper) -> str:
    body = extract_function(
        helper,
        GENERIC_BUS,
        "StartUnit",
        r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*StartUnit\s*\)\s*\(",
    )
    acquire = re.findall(
        r"SDCBUS_BeginTransaction\s*\(\s*bus\s*,\s*FALSE\s*\)", body
    )
    release = re.findall(r"SDCBUS_EndTransaction\s*\(\s*bus\s*\)", body)
    work = body.find("sdcard_StartUnitLocked(sdcUnit)")
    unlock = body.find("SDCBUS_EndTransaction(bus)")
    if len(acquire) != 1 or len(release) != 1 or work < 0 or unlock < work:
        raise ValueError("public StartUnit must acquire nonblocking, run, then release once")
    return sha256(body.encode("utf-8"))


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble_fixture()
    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {sha256(content)}")
    helper = load_helpers()
    start_hash = audit_public_start(helper)
    print(f"Public StartUnit nonblocking acquisition/release source check: PASS ({start_hash})")
    print(
        "Host model: actual activation and frequency functions execute against a "
        "scripted command bus. Activation substitutes frequency callbacks to "
        "isolate the generic state transition; frequency mode executes the "
        "actual SDSC/MMC helper bodies. No controller silicon or media is used."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sd-activation-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sd_activation.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for mode, extra_flags in (
            ("activation", []),
            ("frequency", ["-DTEST_FREQUENCY_ONLY=1"]),
        ):
            for optimization in (0, 2):
                binary = directory / f"test_sd_activation_{mode}_O{optimization}"
                command = compiler + [
                    "-std=c99",
                    f"-O{optimization}",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Wpedantic",
                    "-Wshadow",
                    "-Wstrict-prototypes",
                    "-Wno-sign-compare",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer",
                ] + extra_flags + [str(source_path), "-o", str(binary)]
                print(f"\nBuilding {mode} mode O{optimization} with strict C99 and ASan/UBSan")
                started = time.monotonic()
                try:
                    build = subprocess.run(
                        command,
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=60,
                    )
                except subprocess.TimeoutExpired:
                    print("FAIL: compile exceeded 60 seconds", file=sys.stderr)
                    return 1
                print(f"Compile elapsed: {time.monotonic() - started:.3f}s")
                if build.returncode != 0:
                    sys.stderr.write(build.stdout)
                    sys.stderr.write(build.stderr)
                    return build.returncode or 1
                sys.stdout.write(build.stdout)
                sys.stderr.write(build.stderr)

                print(f"Running {mode} mode O{optimization} (30-second timeout)")
                started = time.monotonic()
                try:
                    result = subprocess.run(
                        [str(binary)],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        check=False,
                        timeout=30,
                        env=environment,
                    )
                except subprocess.TimeoutExpired:
                    print("FAIL: fixture exceeded 30 seconds", file=sys.stderr)
                    return 1
                print(f"Runtime elapsed: {time.monotonic() - started:.3f}s")
                sys.stdout.write(result.stdout)
                sys.stderr.write(result.stderr)
                if result.returncode != 0:
                    return result.returncode
                if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                    print("FAIL: fixture did not report its complete case count", file=sys.stderr)
                    return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
