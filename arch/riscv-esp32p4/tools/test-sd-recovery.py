#!/usr/bin/env python3
"""Compile bounded extracted-source tests for generic SD recovery."""

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


ROOT = Path(__file__).resolve().parents[3]
PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_sd_recovery.c"
TOOLS = PORT / "tools"
GENERIC_BUS = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.c"
BUS_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.h"
UNIT_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_unit.h"
BASE_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_base.h"
INIT_SOURCE = ROOT / "rom" / "devs" / "sdcard" / "sdcard_init.c"
ERRORS_HEADER = ROOT / "compiler" / "include" / "exec" / "errors.h"
SPINLOCK_HEADER = ROOT / "compiler" / "include" / "aros" / "types" / "spinlock_s.h"
HOST_HELPERS = TOOLS / "test-sdmmc-host.py"

CONSTANT_MARKER = "/* INSERT_ACTUAL_SD_CONSTANTS */"
MACRO_MARKER = "/* INSERT_ACTUAL_SD_FUNCTION_MACROS */"
TYPE_MARKER = "/* INSERT_ACTUAL_SD_TYPES */"
FUNCTION_MARKER = "/* INSERT_ACTUAL_SD_FUNCTIONS */"


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


def extract_struct(helper, path: Path, name: str) -> str:
    return helper.extract_definition(
        path, f"struct {name}", rf"\bstruct\s+{re.escape(name)}\s*(?=\{{)"
    ) + ";"


def extract_function(helper, path: Path, name: str, signature: str) -> str:
    return helper.extract_definition(path, name, signature)


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    helper = load_helpers()
    fixture = FIXTURE.read_text(encoding="utf-8")
    markers = (CONSTANT_MARKER, MACRO_MARKER, TYPE_MARKER, FUNCTION_MARKER)
    for marker in markers:
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    constants = "\n".join(
        (
            extract_constants(
                UNIT_HEADER,
                (
                    "AB_Card_Active",
                    "AB_Card_Configuring",
                    "AB_Card_HighSpeed52",
                    "AB_Card_HighSpeed",
                    "AB_Card_HighCapacity",
                    "AB_Card_4bitData",
                    "AB_Card_MMC",
                    "AB_Card_WriteProtect",
                    "AB_Card_Locked",
                    "AF_Card_Active",
                    "AF_Card_Configuring",
                    "AF_Card_HighSpeed52",
                    "AF_Card_HighSpeed",
                    "AF_Card_HighCapacity",
                    "AF_Card_4bitData",
                    "AF_Card_MMC",
                    "AF_Card_WriteProtect",
                    "AF_Card_Locked",
                ),
            ),
            extract_constants(ERRORS_HEADER, ("IOERR_OPENFAIL",)),
            extract_constants(SPINLOCK_HEADER, ("SPINLOCK_MODE_WRITE",)),
        )
    )
    macros = "\n".join(
        (
            extract_constants(BUS_HEADER, ("FNAME_SDCBUS",)),
            extract_constants(UNIT_HEADER, ("FNAME_SDCUNIT",)),
            extract_constants(BASE_HEADER, ("FNAME_SDC",)),
        )
    )
    types = "\n\n".join(
        (
            extract_struct(helper, BUS_HEADER, "sdcard_CardInfo"),
            extract_struct(helper, BUS_HEADER, "sdcard_BusUnits"),
            extract_struct(helper, UNIT_HEADER, "sdcard_Unit"),
        )
    )

    functions = "\n\n".join(
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
            extract_function(
                helper,
                GENERIC_BUS,
                "SDCBUS_LockUnit",
                r"\bvoid\s+SDCBUS_LockUnit\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "SDCBUS_UnlockUnit",
                r"\bvoid\s+SDCBUS_UnlockUnit\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "sdcard_StageUnit",
                r"\bstatic\s+void\s+sdcard_StageUnit\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "sdcard_PublishUnit",
                r"\bstatic\s+void\s+sdcard_PublishUnit\s*\(",
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
                "sdcard_SameCard",
                r"\bstatic\s+BOOL\s+sdcard_SameCard\s*\(",
            ),
            extract_function(
                helper,
                GENERIC_BUS,
                "RecoverUnit",
                r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*RecoverUnit\s*\)\s*\(",
            ),
            extract_function(
                helper,
                INIT_SOURCE,
                "Open",
                r"\bstatic\s+int\s+FNAME_SDC\s*\(\s*Open\s*\)\s*\(",
            ),
            extract_function(
                helper,
                INIT_SOURCE,
                "Close",
                r"\bstatic\s+int\s+FNAME_SDC\s*\(\s*Close\s*\)\s*\(",
            ),
        )
    )

    assembled = fixture.replace(CONSTANT_MARKER, constants)
    assembled = assembled.replace(MACRO_MARKER, macros)
    assembled = assembled.replace(TYPE_MARKER, types)
    assembled = assembled.replace(FUNCTION_MARKER, functions)
    hashes = [
        ("production constants", constants.encode("utf-8")),
        ("production function macros", macros.encode("utf-8")),
        ("production structs", types.encode("utf-8")),
        ("production functions", functions.encode("utf-8")),
        ("assembled fixture", assembled.encode("utf-8")),
        ("test fixture", FIXTURE.read_bytes()),
    ]
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
        "Model: the production recovery/publication/open/close bodies and real "
        "unit/card structs execute with scripted identify/activation callbacks. "
        "Metadata Forbid/spinlock calls are instrumented; this is not an SMP "
        "interleaving or controller/hardware test."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sd-recovery-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sd_recovery.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for smp in (False, True):
            mode = "smp" if smp else "non-smp"
            print(f"Guard model: {mode}")
            for optimization in (0, 2):
                binary = directory / f"test_sd_recovery_{mode}_O{optimization}"
                command = compiler + [
                    "-std=c99",
                    f"-O{optimization}",
                    "-g",
                    "-Wall",
                    "-Wextra",
                    "-Wno-unused-parameter",
                    "-Werror",
                    "-pedantic",
                ]
                if smp:
                    command.append("-D__AROSEXEC_SMP__=1")
                command.extend(
                    [
                        "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer",
                        str(source_path),
                        "-o",
                        str(binary),
                    ]
                )
                subprocess.run(command, check=True)
                subprocess.run([str(binary)], check=True, env=environment)
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
