#!/usr/bin/env python3
"""Strict sanitized tests around the actual Wi-Fi reset-status adapter."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import runpy
import shlex
import subprocess
import sys
import tempfile


PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
RUNTIME_SOURCE = PORT / "network" / "runtime_probe.c"
HOST_HEADER = PORT / "sdmmc" / "host.h"
COMMAND_HEADER = PORT / "sdmmc" / "command.h"
REGISTER_HEADER = PORT / "sdmmc" / "registers.h"
PROBE_HEADER = PORT / "network" / "sdio_probe.h"
COMMAND_SOURCE = PORT / "network" / "sdio_commands.c"
BOARD_HEADER = PORT / "board" / "jc1060p470c.h"
FIXTURE = PORT / "network" / "test_wifi_reset_status.c"

INSERT_MARKERS = {
    "constants": "/* INSERT_ACTUAL_CONSTANTS */",
    "types": "/* INSERT_ACTUAL_COMMAND_TYPES */",
    "budget_helper": "/* INSERT_ACTUAL_BUDGET_HELPER */",
    "cmd52": "/* INSERT_ACTUAL_CMD52 */",
    "wifi_budget": "/* INSERT_ACTUAL_WIFI_BUDGET */",
    "wifi_failure": "/* INSERT_ACTUAL_WIFI_COMMAND_FAILURE */",
    "wifi_command": "/* INSERT_ACTUAL_WIFI_COMMAND */",
    "register_adapters": "/* INSERT_ACTUAL_WIFI_REGISTER_ADAPTERS */",
}


def read_helper():
    """Reuse the repository's C-aware source extraction routines."""
    helper = PORT / "tools" / "test-sdmmc-host.py"
    namespace = runpy.run_path(str(helper))
    return namespace["extract_definition"], namespace["extract_constant_lines"]


def declaration(extract_definition, path: Path, label: str, signature: str) -> str:
    return extract_definition(path, label, signature) + ";"


def assemble_fixture() -> tuple[str, bytes]:
    extract_definition, extract_constant_lines = read_helper()
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in INSERT_MARKERS.values():
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    constants = "\n".join(
        (
            extract_constant_lines(
                REGISTER_HEADER,
                (
                    "P4SD_INT_CMD_DATA_MASK",
                    "P4SD_INT_RESP_ERR",
                    "P4SD_INT_CMD_DONE",
                    "P4SD_INT_DATA_OVER",
                    "P4SD_INT_RXDR",
                    "P4SD_INT_RCRC",
                    "P4SD_INT_DCRC",
                    "P4SD_INT_RTO",
                    "P4SD_INT_DTO",
                    "P4SD_INT_HTO",
                    "P4SD_INT_FRUN",
                    "P4SD_INT_HLE",
                    "P4SD_INT_SBE",
                    "P4SD_INT_EBE",
                    "P4SD_IDSTS_FBE",
                ),
            ),
            extract_constant_lines(
                PROBE_HEADER,
                (
                    "P4_SDIO_PROBE_R5_ERROR_MASK",
                    "P4_SDIO_COMMAND_RESPONSE_TIMEOUT",
                    "P4_SDIO_COMMAND_RESPONSE_CRC",
                ),
            ),
            extract_constant_lines(BOARD_HEADER, ("P4_BOARD_WIFI_SLOT",)),
        )
    )

    response_type = extract_definition(
        PROBE_HEADER,
        "p4_sdio_response_type",
        r"(?m)^typedef\s+enum\s+p4_sdio_response_type\b",
    ) + " p4_sdio_response_type_t;"
    command_types = "\n\n".join(
        (
            declaration(
                extract_definition,
                COMMAND_HEADER,
                "P4HostCommandFault",
                r"(?m)^enum\s+P4HostCommandFault\b",
            ),
            declaration(
                extract_definition,
                COMMAND_HEADER,
                "P4HostCommandObservation",
                r"(?m)^enum\s+P4HostCommandObservation\b",
            ),
            "struct P4HostCommandResult;",
            declaration(
                extract_definition,
                COMMAND_HEADER,
                "P4HostCommand",
                r"(?m)^struct\s+P4HostCommand(?=\s*\{)",
            ),
            declaration(
                extract_definition,
                COMMAND_HEADER,
                "P4HostCommandStatus",
                r"(?m)^enum\s+P4HostCommandStatus\b",
            ),
            declaration(
                extract_definition,
                COMMAND_HEADER,
                "P4HostCommandResult",
                r"(?m)^struct\s+P4HostCommandResult(?=\s*\{)",
            ),
            response_type,
        )
    )

    functions = {
        "budget_helper": extract_definition(
            HOST_HEADER,
            "p4host_budget_ok",
            r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\(",
        ),
        "cmd52": extract_definition(
            COMMAND_SOURCE,
            "p4_sdio_cmd52",
            r"\bint\s+p4_sdio_cmd52\s*\(",
        ),
        "wifi_budget": extract_definition(
            RUNTIME_SOURCE,
            "wifi_budget",
            r"\bstatic\s+int\s+wifi_budget\s*\(",
        ),
        "wifi_failure": extract_definition(
            RUNTIME_SOURCE,
            "wifi_command_failure",
            r"\bstatic\s+int\s+wifi_command_failure\s*\(",
        ),
        "wifi_command": extract_definition(
            RUNTIME_SOURCE,
            "wifi_command",
            r"\bstatic\s+int\s+wifi_command\s*\(",
        ),
        "register_adapters": "\n\n".join(
            (
                extract_definition(
                    RUNTIME_SOURCE,
                    "wifi_register_read",
                    r"\bstatic\s+int\s+wifi_register_read\s*\(",
                ),
                extract_definition(
                    RUNTIME_SOURCE,
                    "wifi_register_write",
                    r"\bstatic\s+int\s+wifi_register_write\s*\(",
                ),
            )
        ),
    }

    replacements = {
        "constants": constants,
        "types": command_types,
        **functions,
    }
    assembled = fixture
    for key, marker in INSERT_MARKERS.items():
        assembled = assembled.replace(marker, replacements[key])

    digest_input = "\n\n".join(
        [constants, command_types]
        + [f"{key}\n{body}" for key, body in functions.items()]
    ).encode("utf-8")
    return assembled, digest_input


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, extracted = assemble_fixture()
    digest = hashlib.sha256(extracted).hexdigest()
    print(
        "Extracted production source: five runtime functions, CMD52 builder, "
        f"command types and register constants (SHA-256 {digest})"
    )

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="p4-wifi-reset-status-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_wifi_reset_status.c"
        source_path.write_text(assembled, encoding="utf-8")

        for optimization in (0, 2):
            binary = directory / f"test_wifi_reset_status_O{optimization}"
            command = compiler + [
                "-std=c99",
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
                "-I",
                str(ROOT / "compiler" / "include"),
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"Building O{optimization} with strict C99 and ASan/UBSan")
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
                print("FAIL: extracted-source model exceeded 20 seconds", file=sys.stderr)
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: model did not report its full case count", file=sys.stderr)
                return 1

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
