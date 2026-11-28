#!/usr/bin/env python3
"""Compile extracted SD transaction functions against a small mock bus."""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"could not find {signature!r}")

    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError(f"could not find body for {signature!r}")

    depth = 0
    state = "code"
    escaped = False
    index = brace
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""

        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line-comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block-comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
        index += 1

    raise RuntimeError(f"unterminated body for {signature!r}")


def main() -> None:
    bus_header = (ROOT / "rom/devs/sdcard/sdcard_bus.h").read_text()
    bus_source = (ROOT / "rom/devs/sdcard/sdcard_bus.c").read_text()
    io_source = (ROOT / "rom/devs/sdcard/sdcard_ioops.c").read_text()
    init_source = (ROOT / "rom/devs/sdcard/sdcard_init.c").read_text()
    unit_header = (ROOT / "rom/devs/sdcard/sdcard_unit.h").read_text()
    harness = (ROOT / "arch/riscv-esp32p4/network/test_sd_transactions.c").read_text()
    chunk_define = re.search(r"(?m)^#define SD_CHUNK_BLOCKS\s+\d+\s*$", io_source)
    if chunk_define is None:
        raise RuntimeError("could not find production SD_CHUNK_BLOCKS definition")

    unit_flags = []
    for name in (
        "AB_Card_HighCapacity",
        "AB_Card_Active",
        "AB_Card_Configuring",
        "AB_Card_4bitData",
        "AB_Card_HighSpeed",
        "AB_Card_HighSpeed52",
        "AF_Card_HighCapacity",
        "AF_Card_Active",
        "AF_Card_Configuring",
        "AF_Card_4bitData",
        "AF_Card_HighSpeed",
        "AF_Card_HighSpeed52",
    ):
        definition = re.search(
            rf"(?m)^#define[ \t]+{re.escape(name)}\b[^\n]*$", unit_header
        )
        if definition is None:
            raise RuntimeError(f"could not find production {name} definition")
        unit_flags.append(definition.group(0))

    signatures = (
        (bus_header, "static inline BOOL SDCBUS_BeginTransaction"),
        (bus_header, "static inline void SDCBUS_EndTransaction"),
        (io_source, "static BOOL sdcard_StopRead"),
        (io_source, "BYTE FNAME_SDCIO(ReadSector32)"),
        (io_source, "BYTE FNAME_SDCIO(ReadSector64)"),
        (bus_source, "static void sdcard_StageUnit"),
        (bus_source, "static void sdcard_PublishUnit"),
        (bus_source, "BOOL FNAME_SDCBUS(StartUnit)"),
        (bus_source, "BOOL FNAME_SDCBUS(RegisterUnit)"),
        (init_source, "static int FNAME_SDC(Open)"),
    )
    actual_functions = chunk_define.group(0) + "\n\n" + "\n\n".join(
        extract_function(source, signature) for source, signature in signatures
    )
    marker = "/* INSERT_EXTRACTED_SDCARD_FUNCTIONS */"
    if harness.count(marker) != 1:
        raise RuntimeError("test harness must have exactly one extraction marker")
    flags_marker = "/* INSERT_ACTUAL_UNIT_FLAGS */"
    if harness.count(flags_marker) != 1:
        raise RuntimeError("test harness must have exactly one unit-flag marker")
    generated_source = harness.replace(flags_marker, "\n".join(unit_flags))
    generated_source = generated_source.replace(marker, actual_functions)

    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="sd-transaction-tests-") as temp:
        temp_path = Path(temp)
        source_path = temp_path / "test_sd_transactions.c"
        source_path.write_text(generated_source)

        for optimization in ("0", "2"):
            binary = temp_path / f"test-o{optimization}"
            command = compiler + [
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Wno-unused-parameter",
                "-Wno-sign-compare",
                "-Werror",
                "-pedantic",
                f"-O{optimization}",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
