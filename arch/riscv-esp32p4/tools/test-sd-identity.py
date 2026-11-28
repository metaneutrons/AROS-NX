#!/usr/bin/env python3
"""Compile host tests around the current generic SD identity functions."""

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
TOOLS = PORT / "tools"
FIXTURE = PORT / "network" / "test_sd_identity.c"
GENERIC_BUS = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.c"
BUS_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.h"
UNIT_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_unit.h"
BASE_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_base.h"
MMC_HEADER = ROOT / "compiler" / "include" / "hardware" / "mmc.h"
SDHC_HEADER = ROOT / "compiler" / "include" / "hardware" / "sdhc.h"
DOS_HEADER = ROOT / "compiler" / "include" / "dos" / "filehandler.h"
MEMORY_HEADER = ROOT / "compiler" / "include" / "exec" / "memory.h"
HOST_HELPERS = TOOLS / "test-sdmmc-host.py"
CONSTANT_MARKER = "/* INSERT_ACTUAL_SD_CONSTANTS */"
TYPE_MARKER = "/* INSERT_ACTUAL_SD_TYPES */"
PRODUCTION_MARKER = "/* INSERT_ACTUAL_SD_PRODUCTION */"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_extraction_helpers():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "p4_sdmmc_host_test_helpers", HOST_HELPERS
    )
    if spec is None or spec.loader is None:
        raise ValueError(f"could not load extraction helpers from {HOST_HELPERS}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract_struct(helper, path: Path, name: str) -> str:
    body = helper.extract_definition(
        path, name, rf"\bstruct\s+{re.escape(name)}\s*(?=\{{)"
    )
    return body + ";"


def extract_constants(path: Path, names: tuple[str, ...]) -> str:
    # Several long-standing compiler headers are ISO-8859-1. Macro lines are
    # ASCII, so decode losslessly before selecting the requested definitions.
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


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    helper = load_extraction_helpers()
    fixture = FIXTURE.read_text(encoding="utf-8")
    markers = (CONSTANT_MARKER, TYPE_MARKER, PRODUCTION_MARKER)
    for marker in markers:
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    sdhc_constants = extract_constants(
        SDHC_HEADER,
        (
            "SDHCI_PS_CMD_INHIBIT",
            "SDHCI_PS_DATA_INHIBIT",
            "OCR_HCS",
            "OCR_BUSY",
            "SD_CMD_SEND_RELATIVE_ADDR",
            "SD_CMD_SEND_IF_COND",
            "SD_CMD_APP_SEND_OP_COND",
        ),
    )
    constants = "\n".join(
        (
            extract_constants(
                MMC_HEADER,
                (
                    "MMC_STATUS_MASK",
                    "MMC_STATUS_ERROR",
                    "MMC_RSP_NONE",
                    "MMC_RSP_PRESENT",
                    "MMC_RSP_136",
                    "MMC_RSP_CRC",
                    "MMC_RSP_OPCODE",
                    "MMC_RSP_R1",
                    "MMC_RSP_R2",
                    "MMC_RSP_R3",
                    "MMC_RSP_R6",
                    "MMC_RSP_R7",
                    "MMC_CMD_GO_IDLE_STATE",
                    "MMC_CMD_ALL_SEND_CID",
                    "MMC_CMD_SEND_CSD",
                    "MMC_CMD_APP_CMD",
                ),
            ),
            sdhc_constants,
            extract_constants(
                BASE_HEADER,
                (
                    "SDCARD_TAGBASE",
                    "SDCARD_TAG_CMD",
                    "SDCARD_TAG_ARG",
                    "SDCARD_TAG_RSPTYPE",
                    "SDCARD_TAG_RSP",
                ),
            ),
            extract_constants(
                UNIT_HEADER,
                (
                    "AB_Card_HighCapacity",
                    "AB_Card_WriteProtect",
                    "AF_Card_HighCapacity",
                    "AF_Card_WriteProtect",
                ),
            ),
            extract_constants(
                BUS_HEADER,
                ("AB_Bus_MediaPresent", "AF_Bus_MediaPresent"),
            ),
            extract_constants(
                DOS_HEADER,
                (
                    "DE_TABLESIZE",
                    "DE_SIZEBLOCK",
                    "DE_NUMHEADS",
                    "DE_SECSPERBLOCK",
                    "DE_BLKSPERTRACK",
                    "DE_RESERVEDBLKS",
                    "DE_LOWCYL",
                    "DE_HIGHCYL",
                    "DE_NUMBUFFERS",
                    "DE_BUFMEMTYPE",
                    "DE_MAXTRANSFER",
                    "DE_MASK",
                    "DE_BOOTBLOCKS",
                ),
            ),
            extract_constants(
                MEMORY_HEADER,
                ("MEMB_PUBLIC", "MEMF_PUBLIC", "MEMB_31BIT", "MEMF_31BIT"),
            ),
        )
    )

    structures = "\n\n".join(
        (
            extract_struct(helper, BUS_HEADER, "sdcard_CardInfo"),
            extract_struct(helper, BUS_HEADER, "sdcard_BusUnits"),
            extract_struct(helper, UNIT_HEADER, "sdcard_Unit"),
        )
    )
    function_specs = (
        (
            "sdcard_IdentifyCommand",
            r"\bstatic\s+BOOL\s+sdcard_IdentifyCommand\s*\(",
        ),
        (
            "sdcard_DecodeCardInfo",
            r"\bstatic\s+BOOL\s+sdcard_DecodeCardInfo\s*\(",
        ),
        (
            "IdentifyCardLocked",
            r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*IdentifyCardLocked\s*\)\s*\(",
        ),
        (
            "sdcard_RegisterUnitLocked",
            r"\bstatic\s+BOOL\s+sdcard_RegisterUnitLocked\s*\(",
        ),
        (
            "Rsp136Unpack",
            r"\bULONG\s+FNAME_SDCBUS\s*\(\s*Rsp136Unpack\s*\)\s*\(",
        ),
    )
    production = {
        name: helper.extract_definition(GENERIC_BUS, name, signature)
        for name, signature in function_specs
    }
    generic_text = GENERIC_BUS.read_text(encoding="utf-8")
    str_mmc0_matches = re.findall(
        r'(?m)^[ \t]*static\s+const\s+char\s+\*\s*str_mmc0\s*=\s*"MMC0"\s*;',
        generic_text,
    )
    if len(str_mmc0_matches) != 1:
        raise ValueError(
            f"expected one str_mmc0 declaration in {GENERIC_BUS}, "
            f"found {len(str_mmc0_matches)}"
        )
    production["str_mmc0 constant"] = str_mmc0_matches[0]
    assembled = fixture.replace(CONSTANT_MARKER, constants)
    assembled = assembled.replace(TYPE_MARKER, structures)
    assembled = assembled.replace(
        PRODUCTION_MARKER,
        "\n\n".join(
            [production["str_mmc0 constant"]]
            + [production[name] for name in (
                "Rsp136Unpack",
                "sdcard_IdentifyCommand",
                "sdcard_DecodeCardInfo",
                "IdentifyCardLocked",
                "sdcard_RegisterUnitLocked",
            )]
        ),
    )

    hashes = [
        (f"production {name}", body.encode("utf-8"))
        for name, body in production.items()
    ]
    hashes.extend(
        (f"production {name} type", body.encode("utf-8"))
        for name, body in zip(
            ("sdcard_CardInfo", "sdcard_BusUnits", "sdcard_Unit"),
            structures.split("\n\n"),
        )
    )
    hashes.extend(
        [
            ("production protocol/ABI constants", constants.encode("utf-8")),
            ("assembled fixture", assembled.encode("utf-8")),
            ("test fixture", FIXTURE.read_bytes()),
        ]
    )
    return assembled, hashes


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble_fixture()
    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {sha256(content)}")
    print(
        "Host model: production identification, CSD decoding and locked unit "
        "registration execute against scripted command responses and mocked "
        "allocation/boot-node calls. It verifies the 1000 ACMD41 attempt cap, "
        "not an overall wall-clock deadline or hardware behavior."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sd-identity-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sd_identity.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_sd_identity_O{optimization}"
            command = compiler + [
                "-std=c99",
                f"-O{optimization}",
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Wshadow",
                "-Wstrict-prototypes",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with strict C99 and ASan/UBSan")
            start = time.monotonic()
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
            print(f"Compile elapsed: {time.monotonic() - start:.3f}s")
            if build.returncode != 0:
                sys.stderr.write(build.stdout)
                sys.stderr.write(build.stderr)
                return build.returncode or 1
            if build.stdout:
                sys.stdout.write(build.stdout)
            if build.stderr:
                sys.stderr.write(build.stderr)

            print(f"Running O{optimization} (30-second timeout)")
            start = time.monotonic()
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
                print(
                    "FAIL: test binary exceeded 30 seconds; the host fixture "
                    "did not complete within its runtime bound",
                    file=sys.stderr,
                )
                return 1
            print(f"Runtime elapsed: {time.monotonic() - start:.3f}s")
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
