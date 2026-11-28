#!/usr/bin/env python3
"""Compile the extracted production SDMMC command engine against host models."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
from runpy import run_path
import shlex
import subprocess
import sys
import tempfile
import re


PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
FIXTURE = PORT / "network" / "test_command_engine.c"
COMMAND_SOURCE = PORT / "sdmmc" / "command.c"
SDMMC = PORT / "sdmmc"
HOST_HEADER = SDMMC / "host.h"
INCLUDE = ROOT / "compiler" / "include"
INSERT_MARKER = "/* INSERT_ACTUAL_COMMAND_ENGINE */"
INSERT_BUDGET_MARKER = "/* INSERT_ACTUAL_BUDGET_HELPER */"
INSERT_WIDTH_MARKER = "/* INSERT_HOST_WIDTH_CONSTANTS */"
BUILD_TIMEOUT_SECONDS = 60
RUN_TIMEOUT_SECONDS = 30

CLOCK_EXTRACTION = run_path(str(PORT / "tools" / "test-clock-engine.py"))
extract_definition = CLOCK_EXTRACTION["extract_definition"]
materialize_ulong_macros = CLOCK_EXTRACTION["materialize_ulong_macros"]
production_macro_candidates = CLOCK_EXTRACTION["production_macro_candidates"]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def replace_once(body: str, label: str, old: str, new: str) -> str:
    if body.count(old) != 1:
        raise ValueError(f"expected one host substitution in {label}")
    return body.replace(old, new)


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in (INSERT_MARKER, INSERT_BUDGET_MARKER, INSERT_WIDTH_MARKER):
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")
    source = COMMAND_SOURCE.read_text(encoding="utf-8")

    function_specs = (
        ("command_wait", r"\bstatic\s+BOOL\s+command_wait\s*\("),
        ("command_valid", r"\bstatic\s+BOOL\s+command_valid\s*\("),
        ("p4host_command", r"\bBOOL\s+p4host_command\s*\("),
    )
    memory_source = (SDMMC / "memory.h").read_text(encoding="utf-8")
    definitions = [
        (name, extract_definition(memory_source, name,
                                 rf"\bstatic\s+inline\s+void\s+{name}\s*\("))
        for name in ("p4host_zero", "p4host_move")
    ] + [
        (name, extract_definition(source, name, signature))
        for name, signature in function_specs
    ]
    host_header = HOST_HEADER.read_text(encoding="utf-8")
    abi_match = re.search(
        r"(?m)^\s*#\s*define\s+P4_SDMMC_HOST_ABI\s+8\s*$", host_header
    )
    if not abi_match:
        raise ValueError("command fixture requires production host ABI 8")
    abi = abi_match.group(0)
    budget_helper = extract_definition(
        host_header,
        "p4host_budget_ok",
        r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\(",
    )
    macro_headers = [
        ("command.h", (SDMMC / "command.h").read_text(encoding="utf-8")),
        ("dma.h", (SDMMC / "dma.h").read_text(encoding="utf-8")),
        ("registers.h", (SDMMC / "registers.h").read_text(encoding="utf-8")),
        ("host.h", host_header),
    ]
    width_originals, width_overrides = materialize_ulong_macros(
        macro_headers, production_macro_candidates(fixture + source)
    )

    command_read = extract_definition(
        source, "command_read", r"\bstatic\s+ULONG\s+command_read\s*\("
    )
    command_read = replace_once(
        command_read,
        "command_read",
        "return *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset);",
        "return mock_mmio_read(offset);",
    )
    command_write = extract_definition(
        source, "command_write", r"\bstatic\s+void\s+command_write\s*\("
    )
    command_write = replace_once(
        command_write,
        "command_write",
        "*(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset) = value;",
        "mock_mmio_write(offset, value);",
    )
    command_write = replace_once(
        command_write,
        "command_write",
        '__asm__ volatile("fence iorw, iorw" ::: "memory");',
        "mock_mmio_barrier();",
    )

    assembled = fixture.replace(
        INSERT_MARKER,
        "\n\n".join(
            [command_read, command_write]
            + [body for _, body in definitions]
        ),
    )
    assembled = assembled.replace(INSERT_BUDGET_MARKER, budget_helper)
    assembled = assembled.replace(INSERT_WIDTH_MARKER, width_overrides)
    hashes = [(name, body.encode("utf-8")) for name, body in definitions]
    hashes.extend((name, body.encode("utf-8")) for name, body in width_originals)
    hashes.append(("target-width register/command overrides", width_overrides.encode("utf-8")))
    hashes.extend(
        (name, body.encode("utf-8"))
        for name, body in (
            ("command_read source", extract_definition(
                source, "command_read", r"\bstatic\s+ULONG\s+command_read\s*\("
            )),
            ("command_write source", extract_definition(
                source, "command_write", r"\bstatic\s+void\s+command_write\s*\("
            )),
        )
    )
    hashes.extend(
        (f"actual {path.name}", path.read_bytes())
        for path in (
            COMMAND_SOURCE,
            HOST_HEADER,
            SDMMC / "command.h",
            SDMMC / "dma.h",
            SDMMC / "registers.h",
            INCLUDE / "hardware" / "mmc.h",
        )
    )
    hashes.append(("p4host_budget_ok source", budget_helper.encode("utf-8")))
    hashes.append(("production ABI 8 constant", abi.encode("utf-8")))
    hashes.append(("assembled fixture", assembled.encode("utf-8")))
    return assembled, hashes


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
        "Production command and diagnostic-observer call sites are extracted; "
        "only MMIO access, the RISC-V fence, callback, and DMA endpoints are "
        "modeled. Native host pointers are not an RV32 ABI test or hardware "
        "evidence; register/command constants are explicitly materialized as "
        "target ULONGs, and the fake DMA endpoint is not a DMA privacy proof."
    )

    with tempfile.TemporaryDirectory(prefix="p4-command-engine-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_command_engine.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_command_engine_O{optimization}"
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
                str(SDMMC),
                "-I",
                str(INCLUDE),
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict C99 warnings")
            build = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=BUILD_TIMEOUT_SECONDS,
            )
            if build.returncode != 0:
                sys.stderr.write(build.stdout)
                sys.stderr.write(build.stderr)
                return build.returncode or 1
            if build.stdout:
                sys.stdout.write(build.stdout)
            if build.stderr:
                sys.stderr.write(build.stderr)

            print(f"Running O{optimization} ({RUN_TIMEOUT_SECONDS}-second timeout)")
            try:
                result = subprocess.run(
                    [str(binary)],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=RUN_TIMEOUT_SECONDS,
                    env=environment,
                )
            except subprocess.TimeoutExpired:
                print(
                    f"FAIL: command fixture exceeded {RUN_TIMEOUT_SECONDS} "
                    "seconds; a bounded "
                    "wait may be stalled",
                    file=sys.stderr,
                )
                return 1
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
