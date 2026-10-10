#!/usr/bin/env python3
"""Compile the production shared-clock engine against a host MMIO model."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_clock_engine.c"
CLOCK_SOURCE = PORT / "sdmmc" / "clock.c"
PLANNER_SOURCE = PORT / "sdmmc" / "clock_plan.c"
SDMMC = PORT / "sdmmc"
INSERT_MARKER = "/* INSERT_ACTUAL_CLOCK_SOURCE */"
INSERT_BUDGET_MARKER = "/* INSERT_ACTUAL_BUDGET_HELPER */"


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


def extract_definition(source: str, label: str, signature: str) -> str:
    matches = list(re.finditer(signature, source, flags=re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected one {label} definition, found {len(matches)}")
    start = matches[0].start()
    opening = find_code_char(source, matches[0].end(), "{")
    try:
        end = find_definition_end(source, opening)
    except ValueError as error:
        raise ValueError(f"{error} for {label}") from error
    return source[start:end]


def extract_constant(source: str, name: str) -> str:
    pattern = re.compile(
        rf"(?m)^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b[^\n]*"
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError(f"expected one {name} definition, found {len(matches)}")
    return matches[0].group(0)


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in (INSERT_MARKER, INSERT_BUDGET_MARKER):
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")
    source = CLOCK_SOURCE.read_text(encoding="utf-8")
    host_header_path = SDMMC / "host.h"
    host_header = host_header_path.read_text(encoding="utf-8")
    budget_helper = extract_definition(
        host_header,
        "p4host_budget_ok",
        r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\(",
    )
    constants = [
        extract_constant(source, "P4HOST_CLOCK_TIMEOUT_US"),
        extract_constant(source, "P4HOST_POLL_BOUND"),
    ]
    function_specs = (
        ("clock_owned", r"\bstatic\s+BOOL\s+clock_owned\s*\("),
        ("clock_wait_clear", r"\bstatic\s+BOOL\s+clock_wait_clear\s*\("),
        ("clock_delay", r"\bstatic\s+BOOL\s+clock_delay\s*\("),
        ("clock_source", r"\bstatic\s+BOOL\s+clock_source\s*\("),
        ("clock_update", r"\bstatic\s+BOOL\s+clock_update\s*\("),
        ("clock_fail", r"\bstatic\s+void\s+clock_fail\s*\("),
        ("p4host_reset", r"\bBOOL\s+p4host_reset\s*\("),
        ("p4host_set_clock", r"\bBOOL\s+p4host_set_clock\s*\("),
    )
    definitions = [
        (name, extract_definition(source, name, signature))
        for name, signature in function_specs
    ]

    # The real pointer-dereferencing accessors are audited and hashed, while
    # their names are supplied by the fixture's modeled register bank.
    low_level = [
        (
            "clock_read source (replaced by MMIO model)",
            extract_definition(source, "clock_read", r"\bstatic\s+ULONG\s+clock_read\s*\("),
        ),
        (
            "clock_write source (replaced by MMIO model)",
            extract_definition(source, "clock_write", r"\bstatic\s+void\s+clock_write\s*\("),
        ),
    ]
    bodies = "\n\n".join(constants + [body for _, body in definitions])
    assembled = fixture.replace(INSERT_MARKER, bodies)
    assembled = assembled.replace(INSERT_BUDGET_MARKER, budget_helper)
    hashes = [(name, body.encode("utf-8")) for name, body in definitions]
    hashes.extend((name, body.encode("utf-8")) for name, body in low_level)
    hashes.extend(
        (f"production {name} constant", value.encode("utf-8"))
        for name, value in zip(("poll timeout", "poll bound"), constants)
    )
    hashes.append(("actual planner source", PLANNER_SOURCE.read_bytes()))
    hashes.append(("actual host.h", host_header_path.read_bytes()))
    hashes.append(("p4host_budget_ok source", budget_helper.encode("utf-8")))
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
        "Host-only modeled MMIO; production timeout and poll-bound "
        "constants are used. This is not a hardware pass."
    )

    with tempfile.TemporaryDirectory(prefix="p4-clock-engine-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_clock_engine.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_clock_engine_O{optimization}"
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
                str(source_path),
                str(PLANNER_SOURCE),
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

            print(f"Running O{optimization} (30-second timeout)")
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
                    "FAIL: clock fixture exceeded 30 seconds; a bounded wait "
                    "may be stalled",
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
