#!/usr/bin/env python3
"""Strict sanitized tests around the actual Wi-Fi SDIO transfer adapters."""

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
MEMORY_HEADER = PORT / "sdmmc" / "memory.h"
PROBE_HEADER = PORT / "network" / "sdio_probe.h"
COMMAND_HEADER = PORT / "sdmmc" / "command.h"
COMMAND_SOURCE = PORT / "network" / "sdio_commands.c"
BOARD_HEADER = PORT / "board" / "jc1060p470c.h"
FIXTURE = PORT / "network" / "test_wifi_transfer.c"

INSERT_MARKERS = {
    "constants": "/* INSERT_ACTUAL_CONSTANTS */",
    "command_types": "/* INSERT_ACTUAL_COMMAND_TYPES */",
    "budget_helpers": "/* INSERT_ACTUAL_BUDGET_HELPERS */",
    "transfer_functions": "/* INSERT_ACTUAL_TRANSFER_FUNCTIONS */",
}


def read_helpers():
    """Reuse the repository's C-aware definition and constant extractors."""
    helper = PORT / "tools" / "test-sdmmc-host.py"
    namespace = runpy.run_path(str(helper))
    return namespace["extract_definition"], namespace["extract_constant_lines"]


def declaration(extract_definition, path: Path, label: str, signature: str) -> str:
    return extract_definition(path, label, signature) + ";"


def assemble_fixture() -> tuple[str, bytes]:
    extract_definition, extract_constant_lines = read_helpers()
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in INSERT_MARKERS.values():
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    constants = "\n".join(
        (
            extract_constant_lines(
                PROBE_HEADER, ("P4_SDIO_PROBE_R5_ERROR_MASK",)
            ),
            extract_constant_lines(BOARD_HEADER, ("P4_BOARD_WIFI_SLOT",)),
        )
    )
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
        )
    )
    budget_helpers = "\n\n".join(
        (
            extract_definition(
                HOST_HEADER,
                "p4host_budget_ok",
                r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\(",
            ),
            extract_definition(
                MEMORY_HEADER,
                "p4host_zero",
                r"\bstatic\s+inline\s+void\s+p4host_zero\s*\(",
            ),
        )
    )
    transfer_functions = "\n\n".join(
        extract_definition(
            RUNTIME_SOURCE,
            name,
            signature,
        )
        for name, signature in (
            ("wifi_budget", r"\bstatic\s+int\s+wifi_budget\s*\("),
            ("wifi_transfer", r"\bstatic\s+int\s+wifi_transfer\s*\("),
            ("wifi_word_read", r"\bstatic\s+int\s+wifi_word_read\s*\("),
            ("wifi_word_write", r"\bstatic\s+int\s+wifi_word_write\s*\("),
            ("wifi_fifo_read", r"\bstatic\s+int\s+wifi_fifo_read\s*\("),
        )
    )

    replacements = {
        "constants": constants,
        "command_types": command_types,
        "budget_helpers": budget_helpers,
        "transfer_functions": transfer_functions,
    }
    assembled = fixture
    for key, marker in INSERT_MARKERS.items():
        assembled = assembled.replace(marker, replacements[key])

    digest_input = "\n\n".join(
        (constants, command_types, budget_helpers, transfer_functions)
    ).encode("utf-8") + COMMAND_SOURCE.read_bytes()
    return assembled, digest_input


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, extracted = assemble_fixture()
    digest = hashlib.sha256(extracted).hexdigest()
    print(
        "Extracted production source: five runtime adapters, actual command "
        f"types/helpers and sdio_commands.c (SHA-256 {digest})"
    )

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    with tempfile.TemporaryDirectory(prefix="p4-wifi-transfer-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_wifi_transfer.c"
        source_path.write_text(assembled, encoding="utf-8")

        for optimization in (0, 2):
            binary = directory / f"test_wifi_transfer_O{optimization}"
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
                "-I",
                str(PORT / "network"),
                str(source_path),
                str(COMMAND_SOURCE),
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
                print("FAIL: model did not report its complete case count", file=sys.stderr)
                return 1

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
