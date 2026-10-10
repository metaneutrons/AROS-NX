#!/usr/bin/env python3
"""Compile and exercise the extracted P4 SD shared command adapter."""

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


PORT = Path(__file__).resolve().parents[1]
TOOLS = PORT / "tools"
FIXTURE = PORT / "network" / "test_sd_command_adapter.c"
BUS_SOURCE = PORT / "sdcard" / "sdcard_esp32p4_bus.c"
COMMAND_HEADER = PORT / "sdmmc" / "command.h"
INTERN_HEADER = PORT / "sdcard" / "sdcard_esp32p4_intern.h"
MMC_HEADER = PORT.parents[1] / "compiler" / "include" / "hardware" / "mmc.h"
HOST_TEST_HELPERS = TOOLS / "test-sdmmc-host.py"
INSERT_MARKER = "/* INSERT_ACTUAL_SOURCE */"
INSERT_TYPES_MARKER = "/* INSERT_ACTUAL_COMMAND_TYPES */"
INSERT_CONSTANTS_MARKER = "/* INSERT_ACTUAL_SD_CONSTANTS */"
EXPECTED_DENYLIST = {20, 24, 25, 26, 27, 28, 29, 32, 33, 35, 36, 38, 42, 56}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_extraction_helpers():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(
        "p4_sdmmc_host_test_helpers", HOST_TEST_HELPERS
    )
    if spec is None or spec.loader is None:
        raise ValueError(f"could not load extraction helpers from {HOST_TEST_HELPERS}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def extract_production_bodies(helper) -> tuple[str, dict[str, str]]:
    command_type_signatures = {
        "P4HostCommand": r"\bstruct\s+P4HostCommand\s*(?=\{)",
        "P4HostCommandStatus": r"\benum\s+P4HostCommandStatus\s*(?=\{)",
        "P4HostCommandResult": r"\bstruct\s+P4HostCommandResult\s*(?=\{)",
    }
    signatures = {
        "p4sd_media_mutating_command":
            r"\bstatic\s+BOOL\s+p4sd_media_mutating_command\s*\(",
        "p4sd_shared_command": r"\bstatic\s+ULONG\s+p4sd_shared_command\s*\(",
        "p4sd_send_cmd_locked": r"\bstatic\s+ULONG\s+p4sd_send_cmd_locked\s*\(",
    }
    bodies = {
        name: helper.extract_definition(BUS_SOURCE, name, signature)
        for name, signature in signatures.items()
    }
    for name, signature in command_type_signatures.items():
        bodies[name] = helper.extract_definition(COMMAND_HEADER, name, signature)
    fixture = FIXTURE.read_text(encoding="utf-8")
    if fixture.count(INSERT_MARKER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_MARKER}")
    if fixture.count(INSERT_TYPES_MARKER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_TYPES_MARKER}")
    if fixture.count(INSERT_CONSTANTS_MARKER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_CONSTANTS_MARKER}")
    constants = "\n".join(
        (
            helper.extract_constant_lines(
                MMC_HEADER,
                (
                    "MMC_CMD_STOP_TRANSMISSION",
                    "MMC_CMD_READ_SINGLE_BLOCK",
                    "MMC_CMD_READ_MULTIPLE_BLOCK",
                    "MMC_DATA_WRITE",
                    "MMC_DATA_READ",
                    "MMC_DATA_STREAM",
                ),
            ),
            helper.extract_constant_lines(
                INTERN_HEADER,
                ("P4SD_MAX_READ_BLOCKS", "P4SD_MAX_DATA_LEN"),
            ),
        )
    )
    bodies["sd_constants"] = constants
    assembled = fixture.replace(INSERT_CONSTANTS_MARKER, constants)
    assembled = assembled.replace(
        INSERT_TYPES_MARKER,
        "\n\n".join(
            bodies[name] + ";"
            for name in ("P4HostCommand", "P4HostCommandStatus", "P4HostCommandResult")
        ),
    )
    assembled = assembled.replace(
        INSERT_MARKER,
        "\n\n".join(
            (bodies["p4sd_media_mutating_command"], bodies["p4sd_shared_command"])
        ),
    )
    return assembled, bodies


def audit_denylist(body: str) -> set[int]:
    observed = {int(value, 10) for value in re.findall(r"\bcase\s+(\d+)\s*:", body)}
    if observed != EXPECTED_DENYLIST:
        raise ValueError(
            "production media-mutating denylist changed: "
            f"expected {sorted(EXPECTED_DENYLIST)}, found {sorted(observed)}"
        )
    return observed


def audit_dispatch(body: str) -> None:
    checks = [
        (r"if\s*\(\s*!priv->host_ready\s*\)", "host readiness check"),
        (r"if\s*\(\s*command_index\s*>\s*0x3F\s*\)", "command-index bound"),
        (
            r"if\s*\(\s*p4sd_media_mutating_command\s*\(\s*command_index\s*\)\s*\)",
            "media-mutating denylist",
        ),
    ]
    positions: list[tuple[str, int]] = []
    for pattern, label in checks:
        match = re.search(pattern, body)
        if match is None:
            raise ValueError(f"p4sd_send_cmd_locked is missing its {label}")
        positions.append((label, match.start()))

    normal = re.search(
        r"(?m)^\s*#if\s+!P4_SDCARD_TRACE\s*&&\s*"
        r"P4_SDCARD_FAULT_INJECT\s*==\s*P4SD_FAULT_NONE\s*$\s*"
        r"return\s+p4sd_shared_command\s*\((?P<args>.*?)\);\s*"
        r"#else\b",
        body,
        flags=re.DOTALL,
    )
    if normal is None:
        raise ValueError("normal builds no longer dispatch every request to the shared adapter")
    normal_args = re.sub(r"\s+", " ", normal.group("args")).strip()
    if normal_args != (
        "bus, command_index, argument, rsp_type, response, data, data_len, data_flags"
    ):
        raise ValueError(f"normal shared-adapter arguments changed: {normal_args}")
    positions.append(("normal all-request dispatch", normal.start()))
    ordered_positions = [position for _, position in positions]
    if ordered_positions != sorted(ordered_positions):
        order = " -> ".join(label for label, _ in positions)
        raise ValueError(f"shared dispatch precedes a safety gate ({order})")

    diagnostic = re.search(
        r"if\s*\(\s*data_len\s*==\s*0(?P<guard>.*?)\)\s*"
        r"return\s+p4sd_shared_command\s*\((?P<args>.*?)\);",
        body[normal.end() :],
        flags=re.DOTALL,
    )
    if diagnostic is None:
        raise ValueError("diagnostic branch lost its legacy no-data shared-adapter path")
    guard = diagnostic.group("guard")
    if "#if P4_SDCARD_TRACE" not in guard or "&& !p4sd_trace.active" not in guard:
        raise ValueError("diagnostic no-data path no longer preserves active CMD18 tracing")
    if len(re.findall(r"p4sd_trace\.active", guard)) != 1:
        raise ValueError("diagnostic no-data guard has extra trace-state conditions")
    diagnostic_args = re.sub(r"\s+", " ", diagnostic.group("args")).strip()
    if diagnostic_args != (
        "bus, command_index, argument, rsp_type, response, NULL, 0, 0"
    ):
        raise ValueError(f"diagnostic no-data arguments changed: {diagnostic_args}")

    data_branch = re.search(
        r"if\s*\(\s*data_len\s*!=\s*0\s*\)",
        body[normal.end() :],
    )
    if data_branch is None or diagnostic.end() > data_branch.start():
        raise ValueError("diagnostic legacy data path is not after its no-data dispatch")
    legacy = body[normal.end() + data_branch.start() :]
    for fragment, label in (
        ("MMC_DATA_WRITE | MMC_DATA_STREAM", "read-only data-flag validation"),
        ("p4sd_reset_fifo", "legacy FIFO preparation"),
        ("p4sd_dma_prepare_read", "legacy read transfer"),
        ("p4sd_recover_multiblock", "legacy CMD18 recovery"),
    ):
        if fragment not in legacy:
            raise ValueError(f"diagnostic legacy data path lost {label}")

    call_count = len(re.findall(r"\bp4sd_shared_command\s*\(", body))
    if call_count != 2:
        raise ValueError(
            "expected normal all-request and diagnostic no-data adapter call sites; "
            f"found {call_count}"
        )


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")

    helper = load_extraction_helpers()
    assembled, bodies = extract_production_bodies(helper)
    denylist = audit_denylist(bodies["p4sd_media_mutating_command"])
    audit_dispatch(bodies["p4sd_send_cmd_locked"])

    print("Extracted production source (SHA-256):")
    for name in (
        "sd_constants",
        "P4HostCommand",
        "P4HostCommandStatus",
        "P4HostCommandResult",
        "p4sd_shared_command",
        "p4sd_media_mutating_command",
        "p4sd_send_cmd_locked",
    ):
        print(f"  {name}: {sha256(bodies[name].encode('utf-8'))}")
    print(f"  fixture: {sha256(FIXTURE.read_bytes())}")
    print(f"  actual denylist values: {sorted(denylist)}")
    print(
        "Source coverage: shared adapter, denylist, SD data constants and "
        "command/request/result type definitions execute from production. "
        "The SendCmd wrapper's normal dispatch and diagnostic exception are "
        "structurally audited, not executed. Host, CMD13 confirmation, "
        "CMD18 recovery, TagItem/bus/private state, logging and memory helpers "
        "are fixture stand-ins. No MMIO or hardware behavior is modeled."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sd-command-adapter-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sd_command_adapter.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_sd_command_adapter_O{optimization}"
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
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict C99 warnings")
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
                print("FAIL: compiler exceeded 60 seconds", file=sys.stderr)
                return 1
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
                print("FAIL: test binary exceeded 30 seconds", file=sys.stderr)
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
