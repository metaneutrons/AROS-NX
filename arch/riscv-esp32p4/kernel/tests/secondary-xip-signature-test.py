#!/usr/bin/env python3
"""Assemble and inspect the optional P4 secondary XIP signature leaf."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
KERNEL = ROOT / "arch/riscv-esp32p4/kernel"
SOURCE = KERNEL / "secondary_xip_signature.S"
BUILD = ROOT.parent / "AROS-ESP32-build"
TOOLBIN = BUILD / "bin/darwin-aarch64/tools/crosstools"
DEFAULT_ARTIFACTS = (
    BUILD / "evidence/smp-priority-2026-10-01/"
    "secondary-xip-signature-author-20261002"
)

MARCH = "rv32imafc_zicsr_zifencei_zaamo_zalrsc"
MABI = "ilp32f"
XOR_BEFORE = 0x13579BDF
ROTATE_LEFT = 7
XOR_AFTER = 0x2A5
KNOWN_INPUT = 0xC001D00D
KNOWN_OUTPUT = 0xAB25EBCC


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, check=False, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=20,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command exited {completed.returncode}: {shlex.join(command)}\n"
            f"{completed.stdout}"
        )
    return completed.stdout


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def host_model(value: int) -> int:
    value = (value ^ XOR_BEFORE) & 0xFFFFFFFF
    value = ((value << ROTATE_LEFT) | (value >> (32 - ROTATE_LEFT))) & 0xFFFFFFFF
    return (value ^ XOR_AFTER) & 0xFFFFFFFF


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def instructions_from_dump(dump: str) -> list[tuple[str, str]]:
    body = False
    instructions: list[tuple[str, str]] = []
    for line in dump.splitlines():
        if re.search(r"<__p4_secondary_xip_signature>:$", line):
            body = True
            continue
        if body and re.search(r"^[0-9a-fA-F]+\s+<[^>]+>:$", line.strip()):
            break
        if not body:
            continue
        match = re.match(
            r"^\s*[0-9a-fA-F]+:\s+"
            r"(?:[0-9a-fA-F]{2,8}\s+)+"
            r"([a-z][a-z0-9.]*)\s*(.*?)\s*$",
            line,
        )
        if match:
            operands = match.group(2).split("#", 1)[0].rstrip()
            instructions.append((match.group(1), operands))
    return instructions


def section_rows(sections: str) -> list[tuple[str, int, str]]:
    lines = sections.splitlines()
    rows: list[tuple[str, int, str]] = []
    for index, line in enumerate(lines):
        match = re.match(r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]+)\s+", line)
        if not match:
            continue
        flags = lines[index + 1].strip() if index + 1 < len(lines) else ""
        rows.append((match.group(1), int(match.group(2), 16), flags))
    return rows


def symbol_value(symbols: str, name: str) -> tuple[int, str]:
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) >= 5 and fields[-1] == name:
            return int(fields[0], 16), fields[-3]
    raise AssertionError(f"cannot find symbol table entry {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cc", type=Path,
        default=Path(os.environ.get("RISCV_AROS_GCC", TOOLBIN / "riscv-aros-gcc")),
    )
    parser.add_argument(
        "--objdump", type=Path,
        default=Path(os.environ.get("RISCV_AROS_OBJDUMP", TOOLBIN / "riscv-aros-objdump")),
    )
    parser.add_argument("--artifact-dir", type=Path, default=DEFAULT_ARTIFACTS)
    args = parser.parse_args()

    require(args.cc.is_file(), f"compiler not found: {args.cc}")
    require(args.objdump.is_file(), f"objdump not found: {args.objdump}")
    require(SOURCE.is_file(), f"assembly source not found: {SOURCE}")
    args.artifact_dir.mkdir(parents=True, exist_ok=True)

    obj = args.artifact_dir / "secondary-xip-signature-author-20261002.o"
    dump_path = args.artifact_dir / "secondary-xip-signature-author-20261002.dis.txt"
    sections_path = args.artifact_dir / "secondary-xip-signature-author-20261002.sections.txt"
    symbols_path = args.artifact_dir / "secondary-xip-signature-author-20261002.symbols.txt"
    relocations_path = args.artifact_dir / "secondary-xip-signature-author-20261002.relocations.txt"

    compile_command = [
        str(args.cc), f"-march={MARCH}", f"-mabi={MABI}", "-mcmodel=medany",
        "-mno-relax", "-c", str(SOURCE), "-o", str(obj),
    ]
    run(compile_command)
    dump = run([str(args.objdump), "-dr", "-M", "no-aliases", str(obj)])
    sections = run([str(args.objdump), "-h", str(obj)])
    symbols = run([str(args.objdump), "-t", str(obj)])
    relocations = run([str(args.objdump), "-r", str(obj)])
    dump_path.write_text(dump)
    sections_path.write_text(sections)
    symbols_path.write_text(symbols)
    relocations_path.write_text(relocations)

    # The dedicated input section is caught by the existing XIP script's
    # *(.text*) wildcard; zero-size default ELF sections are harmless.
    rows = section_rows(sections)
    sig_rows = [row for row in rows if row[0] == ".text.secondary_xip_signature"]
    require(len(sig_rows) == 1, "dedicated signature text section is missing/duplicated")
    require(sig_rows[0][1] > 0 and
            sig_rows[0][2] == "CONTENTS, ALLOC, LOAD, READONLY, CODE",
            f"signature section is not allocatable executable code: {sig_rows[0]}")
    nonempty_data = [row for row in rows
                     if row[1] and re.fullmatch(r"\.(?:s?data|s?rodata)(?:\..*)?", row[0])]
    require(not nonempty_data, f"unexpected data/read-only-data sections: {nonempty_data}")
    require("*(.text*)" in (KERNEL / "ldscript-xip.lds").read_text(),
            "existing XIP linker script no longer collects .text* input sections")

    required_symbols = (
        "__p4_secondary_xip_signature_start",
        "__p4_secondary_xip_signature",
        "__p4_secondary_xip_signature_end",
    )
    symbol_rows = {symbol: symbol_value(symbols, symbol) for symbol in required_symbols}
    start, start_section = symbol_rows[required_symbols[0]]
    function, function_section = symbol_rows[required_symbols[1]]
    end, end_section = symbol_rows[required_symbols[2]]
    require(start == function and end > start and
            start_section == function_section == end_section == ".text.secondary_xip_signature",
            f"function range is not contained by the signature section: {symbol_rows}")
    require(end - start == sig_rows[0][1],
            f"symbol range size differs from section size: range={end-start}, section={sig_rows[0][1]}")
    require("RELOCATION RECORDS FOR" not in relocations and "R_RISCV_" not in relocations,
            f"signature object has relocation records:\n{relocations}")

    instructions = instructions_from_dump(dump)
    require(instructions, "cannot parse the function disassembly")
    mnemonics = [mnemonic for mnemonic, _ in instructions]
    expected_mnemonics = ["lui", "addi", "xor", "slli", "srli", "or", "xori", "jalr"]
    require(mnemonics == expected_mnemonics,
            f"unexpected instruction sequence or missing ret encoding: {instructions}")
    require(mnemonics.count("jalr") == 1 and mnemonics[-1] == "jalr",
            "unexpected indirect control transfer")
    require(not any(mnemonic.startswith("c.") for mnemonic in mnemonics),
            "compressed instruction found despite .option norvc")
    allowed_mnemonics = {"lui", "addi", "xor", "slli", "srli", "or", "xori", "jalr"}
    require(set(mnemonics) <= allowed_mnemonics,
            f"unexpected instruction in leaf: {instructions}")
    require(instructions == [
        ("lui", "t0,0x1357a"),
        ("addi", "t0,t0,-1057"),
        ("xor", "a0,a0,t0"),
        ("slli", "t0,a0,0x7"),
        ("srli", "a0,a0,0x19"),
        ("or", "a0,a0,t0"),
        ("xori", "a0,a0,677"),
        ("jalr", "zero,0(ra)"),
    ], f"disassembly does not match the fixed transform and return: {instructions}")
    require(end - start == 4 * len(instructions),
            "function contains a non-32-bit instruction despite .option norvc")
    allowed_registers = {"a0", "t0", "zero", "ra"}
    for mnemonic, operands in instructions:
        registers = re.findall(r"\b(?:zero|ra|sp|gp|tp|[ast]\d+|x\d+)\b", operands)
        require(set(registers) <= allowed_registers,
                f"instruction uses a register outside a0/t0/return state: {mnemonic} {operands}")
    require("__p4_secondary_xip_signature" in dump,
            "function label missing from disassembly")
    require(host_model(KNOWN_INPUT) == KNOWN_OUTPUT,
            f"host model changed: {host_model(KNOWN_INPUT):#010x}")

    outputs = [host_model(value) for value in (
        0, 0xFFFFFFFF, 0x12345678, KNOWN_INPUT,
    )]
    print(f"source={SOURCE}")
    print(f"source_sha256={sha256(SOURCE)}")
    print(f"compiler={args.cc}")
    print(f"objdump={args.objdump}")
    print(f"march={MARCH} mabi={MABI} mcmodel=medany -mno-relax")
    print(f"compile_command={shlex.join(compile_command)}")
    print(f"artifact_dir={args.artifact_dir}")
    print(f"object={obj} bytes={obj.stat().st_size} sha256={sha256(obj)}")
    print(f"dump={dump_path} sha256={sha256(dump_path)}")
    print(f"sections={sections_path} sha256={sha256(sections_path)}")
    print(f"symbols={symbols_path} sha256={sha256(symbols_path)}")
    print(f"relocations={relocations_path} sha256={sha256(relocations_path)}")
    print(f"instructions={instructions}")
    print(f"known_vector={KNOWN_INPUT:#010x}->{KNOWN_OUTPUT:#010x}")
    print("other_host_model_outputs=" + ",".join(f"{value:#010x}" for value in outputs))
    print("PASS: executable .text-only leaf, no relocations, fixed a0/t0 body")
    print("LIMIT: assembly/object inspection and host model only; not executed on hart1")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
