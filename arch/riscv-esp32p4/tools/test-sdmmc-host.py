#!/usr/bin/env python3
"""Compile host fixtures around the current production SDMMC helpers."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_sdmmc_host.c"
INTERN = PORT / "sdcard" / "sdcard_esp32p4_intern.h"
REGISTERS = PORT / "sdmmc" / "registers.h"
HOST_SOURCE = PORT / "sdmmc" / "host.c"
HOST_HEADER = PORT / "sdmmc" / "host.h"
BUS_SOURCE = PORT / "sdcard" / "sdcard_esp32p4_bus.c"
GENERIC_BUS_SOURCE = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.c"
GENERIC_IO_SOURCE = ROOT / "rom" / "devs" / "sdcard" / "sdcard_ioops.c"
SDHC_HEADER = ROOT / "compiler" / "include" / "hardware" / "sdhc.h"
BOARD_HEADER = PORT / "board" / "d1001.h"
INSERT_MARKER = "/* INSERT_ACTUAL_SOURCE */"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def find_code_char(source: str, start: int, wanted: str) -> int:
    """Find a character outside C comments, strings and character literals."""
    state = "code"
    escaped = False
    index = start
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == wanted:
                return index
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise ValueError(f"could not find C token {wanted!r}")


def find_definition_end(source: str, opening: int) -> int:
    """Return the closing brace paired with a C definition's opening brace."""
    state = "code"
    escaped = False
    depth = 0
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index + 1
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise ValueError("unterminated C definition")


def extract_definition(path: Path, label: str, signature: str) -> str:
    source = path.read_text(encoding="utf-8")
    matches = list(re.finditer(signature, source, flags=re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(
            f"expected one {label} definition in {path}, found {len(matches)}"
        )
    start = matches[0].start()
    opening = find_code_char(source, matches[0].end(), "{")
    try:
        end = find_definition_end(source, opening)
    except ValueError as error:
        raise ValueError(f"{error} for {label} in {path}") from error
    return source[start:end]


def extract_constant_lines(path: Path, names: tuple[str, ...]) -> str:
    source = path.read_text(encoding="utf-8")
    lines: list[str] = []
    for name in names:
        pattern = re.compile(
            rf"(?m)^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b[^\n]*(?:\\\n[^\n]*)*"
        )
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise ValueError(
                f"expected one {name} definition in {path}, found {len(matches)}"
            )
        lines.append(matches[0].group(0))
    return "\n".join(lines)


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    fixture = FIXTURE.read_text(encoding="utf-8")
    if fixture.count(INSERT_MARKER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_MARKER}")

    definitions = [
        (
            "p4host_acquire",
            extract_definition(
                HOST_SOURCE,
                "p4host_acquire",
                r"\bstatic\s+BOOL\s+p4host_acquire\s*\(",
            ),
        ),
        (
            "p4host_release",
            extract_definition(
                HOST_SOURCE,
                "p4host_release",
                r"\bstatic\s+void\s+p4host_release\s*\(",
            ),
        ),
        (
            "p4host_invalidate",
            extract_definition(
                HOST_SOURCE,
                "p4host_invalidate",
                r"\bstatic\s+void\s+p4host_invalidate\s*\(",
            ),
        ),
    ]
    register_constants = extract_constant_lines(
        REGISTERS,
        (
            "P4_HPPERIPH0_BASE",
            "P4_SDMMC_BASE",
            "P4_LPAON_BASE",
            "P4SD_RINTSTS",
            "P4SD_INT_CMD_DATA_MASK",
            "P4SD_CMDARG",
            "P4SD_CTYPE",
            "P4SD_CTYPE_4BIT_SLOT0",
            "P4SD_CTYPE_8BIT_SLOT0",
        ),
    )
    card_constants = extract_constant_lines(
        INTERN,
        (
            "P4_PMU_BASE",
            "P4_PMU_LDO4_CTRL",
            "P4_PMU_LDO4_ANA",
            "P4_PMU_LDO_FORCE_SW",
            "P4_PMU_LDO_XPD",
            "P4_PMU_LDO_TIEH_SEL_M",
            "P4_PMU_LDO_3V3",
            "P4_PMU_LDO_EN_VDET",
            "P4SD_POWER_DELAY_US",
        ),
    )
    constants = "\n".join((register_constants, card_constants))
    host_policy_constant = extract_constant_lines(
        HOST_HEADER,
        ("P4_SDMMC_HOST_WAIT_US",),
    )
    host_abi_constant = extract_constant_lines(
        HOST_HEADER,
        ("P4_SDMMC_HOST_ABI",),
    )
    reset_constants = extract_constant_lines(
        SDHC_HEADER,
        ("SDHCI_RESET_ALL", "SDHCI_RESET_CMD", "SDHCI_RESET_DATA"),
    )
    board_constants = extract_constant_lines(
        BOARD_HEADER,
        ("P4_BOARD_SD_HAS_POWER_GPIO", "P4_BOARD_SD_POWER_GPIO"),
    )
    definitions.extend(
        [
            (
                "p4sd_set",
                extract_definition(
                    BUS_SOURCE,
                    "p4sd_set",
                    r"\bstatic\s+inline\s+void\s+p4sd_set\s*\(",
                ),
            ),
            (
                "P4SDBUS_BeginTransaction",
                extract_definition(
                    BUS_SOURCE,
                    "P4SDBUS_BeginTransaction",
                    r"\bBOOL\s+FNAME_P4SDBUS\s*\(\s*BeginTransaction\s*\)\s*\(",
                ),
            ),
            (
                "P4SDBUS_EndTransaction",
                extract_definition(
                    BUS_SOURCE,
                    "P4SDBUS_EndTransaction",
                    r"\bvoid\s+FNAME_P4SDBUS\s*\(\s*EndTransaction\s*\)\s*\(",
                ),
            ),
            (
                "p4sd_soft_reset_locked",
                extract_definition(
                    BUS_SOURCE,
                    "p4sd_soft_reset_locked",
                    r"\bstatic\s+void\s+p4sd_soft_reset_locked\s*\(",
                ),
            ),
            (
                "p4sd_set_power_locked",
                extract_definition(
                    BUS_SOURCE,
                    "p4sd_set_power_locked",
                    r"\bstatic\s+void\s+p4sd_set_power_locked\s*\(",
                ),
            ),
            (
                "p4sd_set_width_locked",
                extract_definition(
                    BUS_SOURCE,
                    "p4sd_set_width_locked",
                    r"\bstatic\s+void\s+p4sd_set_width_locked\s*\(",
                ),
            ),
        ]
    )
    inserted = "\n\n".join(
        [constants, host_policy_constant, reset_constants, board_constants]
        + [body for _, body in definitions]
    )
    assembled = fixture.replace(INSERT_MARKER, inserted)
    hashes = [(name, body.encode("utf-8")) for name, body in definitions]
    hashes.append(("register constants", register_constants.encode("utf-8")))
    hashes.append(("card constants", card_constants.encode("utf-8")))
    hashes.append(
        ("host policy constant", host_policy_constant.encode("utf-8"))
    )
    hashes.append(("host ABI constant", host_abi_constant.encode("utf-8")))
    hashes.append(("SDHCI reset constants", reset_constants.encode("utf-8")))
    hashes.append(("D1001 board constants", board_constants.encode("utf-8")))
    hashes.append(("assembled fixture", assembled.encode("utf-8")))
    return assembled, hashes


def audit_transaction_call_modes() -> list[tuple[str, str]]:
    p4_call = (
        r"FNAME_P4SDBUS\s*\(\s*BeginTransaction\s*\)\s*\("
        r"\s*bus\s*,\s*(TRUE|FALSE)\s*\)"
    )
    generic_call = r"SDCBUS_BeginTransaction\s*\(\s*bus\s*,\s*(TRUE|FALSE)\s*\)"
    specifications = [
        ("P4 SoftReset wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SDBUS\s*\(\s*SoftReset\s*\)\s*\(", p4_call, "FALSE"),
        ("P4 SetClock wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SDBUS\s*\(\s*SetClock\s*\)\s*\(", p4_call, "FALSE"),
        ("P4 SetPowerLevel wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SDBUS\s*\(\s*SetPowerLevel\s*\)\s*\(", p4_call, "FALSE"),
        ("P4 SetBusWidth wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SDBUS\s*\(\s*SetBusWidth\s*\)\s*\(", p4_call, "FALSE"),
        ("P4 SendCmd wrapper", BUS_SOURCE,
         r"\bULONG\s+FNAME_P4SDBUS\s*\(\s*SendCmd\s*\)\s*\(", p4_call, "FALSE"),
        ("P4 BusInit wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SD\s*\(\s*BusInit\s*\)\s*\(", p4_call, "TRUE"),
        ("P4 BusPostIRQInit wrapper", BUS_SOURCE,
         r"\bvoid\s+FNAME_P4SD\s*\(\s*BusPostIRQInit\s*\)\s*\(", p4_call, "TRUE"),
        ("32-bit outer read", GENERIC_IO_SOURCE,
         r"\bBYTE\s+FNAME_SDCIO\s*\(\s*ReadSector32\s*\)\s*\(", generic_call, "TRUE"),
        ("64-bit outer read", GENERIC_IO_SOURCE,
         r"\bBYTE\s+FNAME_SDCIO\s*\(\s*ReadSector64\s*\)\s*\(", generic_call, "TRUE"),
        ("generic RegisterUnit", GENERIC_BUS_SOURCE,
         r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*RegisterUnit\s*\)\s*\(", generic_call, "TRUE"),
        ("generic StartUnit", GENERIC_BUS_SOURCE,
         r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*StartUnit\s*\)\s*\(", generic_call, "FALSE"),
    ]
    results: list[tuple[str, str]] = []
    for label, path, signature, call_pattern, expected_mode in specifications:
        body = extract_definition(path, label, signature)
        observed_modes = re.findall(call_pattern, body)
        if observed_modes != [expected_mode]:
            raise ValueError(
                f"{label} must have one transaction acquisition with "
                f"may_wait={expected_mode}; found {observed_modes}"
            )
        results.append((label, sha256(body.encode("utf-8"))))
    return results


def audit_dma_abi() -> str:
    source = HOST_HEADER.read_text(encoding="utf-8")
    abi_match = re.search(r"(?m)^\s*#\s*define\s+P4_SDMMC_HOST_ABI\s+(\d+)\b", source)
    if not abi_match or abi_match.group(1) != "3":
        raise ValueError("P4 SDMMC host ABI must be version 3")
    struct_match = re.search(r"\bstruct\s+P4SDMMCHost\s*\{", source)
    if not struct_match:
        raise ValueError("P4 SDMMC host structure is missing")
    struct_end = source.find("\n};", struct_match.end())
    if struct_end < 0:
        raise ValueError("P4 SDMMC host structure is unterminated")
    host_struct = source[struct_match.end() : struct_end]
    fields = (
        r"(?m)^\s*UBYTE\s*\*\s*dma_data\s*;",
        r"(?m)^\s*ULONG\s+dma_length\s*;",
        r"(?m)^\s*BOOL\s+dma_writing\s*;",
        r"(?m)^\s*BOOL\s+dma_faulted\s*;",
    )
    if any(re.search(pattern, host_struct) is None for pattern in fields):
        raise ValueError("P4 SDMMC ABI 3 DMA identity/fault fields are incomplete")
    return sha256((abi_match.group(0) + host_struct).encode("utf-8"))


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble_fixture()
    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {sha256(content)}")
    print(f"  test fixture: {sha256(FIXTURE.read_bytes())}")
    print(
        "Hardware-path limitation: extracted locked functions run against "
        "mocked MMIO, GPIO, reset and delay helpers; this checks call absence, "
        "not peripheral behavior on silicon."
    )

    call_mode_checks = audit_transaction_call_modes()
    dma_abi_hash = audit_dma_abi()
    print(f"P4 SDMMC host ABI 3 DMA field check: PASS ({dma_abi_hash})")
    print("Transaction may_wait source checks:")
    for name, digest in call_mode_checks:
        print(f"  {name}: PASS ({digest})")

    with tempfile.TemporaryDirectory(prefix="p4-sdmmc-host-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sdmmc_host.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_sdmmc_host_O{optimization}"
            command = compiler + [
                "-std=c11",
                f"-O{optimization}",
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Wconversion",
                "-Wshadow",
                "-Wstrict-prototypes",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict warnings")
            build = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=60,
            )
            if build.returncode != 0:
                sys.stderr.write(build.stdout)
                sys.stderr.write(build.stderr)
                return build.returncode or 1
            if build.stdout:
                sys.stdout.write(build.stdout)
            if build.stderr:
                sys.stderr.write(build.stderr)

            print(f"Running O{optimization} (20-second timeout)")
            try:
                result = subprocess.run(
                    [str(binary)],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=20,
                    env=environment,
                )
            except subprocess.TimeoutExpired:
                print(
                    "FAIL: test binary exceeded 20 seconds; a stalled-clock "
                    "wait may be unbounded",
                    file=sys.stderr,
                )
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: test binary did not report its complete case count", file=sys.stderr)
                return 1

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
