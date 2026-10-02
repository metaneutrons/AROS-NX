#!/usr/bin/env python3
"""Execute the actual preprocessed P4 SMP/FPU trap entry under QEMU."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[4]
KERNEL = ROOT / "arch/riscv-esp32p4/kernel"
TESTS = KERNEL / "tests"
FIXTURES = TESTS / "fixtures"
EVIDENCE = ROOT.parent / "AROS-ESP32-build/evidence/smp-isr-entry-2026-10-01"
TOOLCHAIN = ROOT.parent / "AROS-ESP32-build/bin/darwin-aarch64/tools/crosstools"
SYSROOT = ROOT.parent / "AROS-ESP32-build/bin/esp32p4-riscv/AROS/Developer"
COMPILER = Path(os.environ.get("RISCV_AROS_GCC", TOOLCHAIN / "riscv-aros-gcc"))
LINKER = Path(os.environ.get("RISCV_AROS_LD", TOOLCHAIN / "riscv-aros-ld.bfd"))
QEMU = shutil.which("qemu-system-riscv32")

MARCH = "rv32imafc_zicsr_zifencei_zaamo_zalrsc"
MABI = "ilp32f"
MCMODEL = "medany"
FRAME_BYTES = 288


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def run(command: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    COMMANDS.append(shlex.join(command))
    try:
        return subprocess.run(command, check=False, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        fail(f"command timed out after {timeout}s: {shlex.join(command)}\n{exc.stdout or ''}")


COMMANDS: list[str] = []


def need_success(command: list[str], *, timeout: int = 30) -> str:
    result = run(command, timeout=timeout)
    if result.returncode != 0:
        fail(f"command exited {result.returncode}: {shlex.join(command)}\n{result.stdout}")
    return result.stdout


def check_preprocessed_sequence(source: str) -> None:
    source = re.sub(r"\s+", " ", source)
    required = (
        "csrrw sp, mscratch, sp",
        "bnez sp, .Lp4_isr_stack_selected",
        "csrr t0, mscratch",
        "sw t0, 4(sp)",
        "sw t0, 136(sp)",
        "sw zero, 136(sp)",
        "csrw mscratch, zero",
        "lw t0, 136(sp)",
        "csrw mscratch, t0",
        "andi t0, t0, -9",
        "lw sp, 4(sp)",
        "mret",
    )
    for token in required:
        if token not in source:
            fail(f"preprocessed production trap source lacks expected instruction: {token!r}")
    if "addi sp, sp, -288" not in source or "addi t0, sp, 288" not in source:
        fail("SMP+P4_E3_FPU preprocessing did not select the 288-byte frame")
    if "fsw f31, 124(t0)" not in source:
        fail("the actual FPU spill region was not selected")


def source_manifest(paths: list[Path]) -> str:
    lines = []
    for path in paths:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.relative_to(ROOT)}")
    return "\n".join(lines) + "\n"


def main() -> int:
    if not COMPILER.is_file():
        fail(f"RISC-V compiler not found: {COMPILER}")
    if not LINKER.is_file():
        fail(f"RISC-V linker not found: {LINKER}")
    if not QEMU:
        fail("qemu-system-riscv32 not found")

    traps = KERNEL / "traps.S"
    trap_header = KERNEL / "p4_trap_frame.h"
    harness = FIXTURES / "p4-isr-entry-harness.S"
    linker_script = FIXTURES / "p4-isr-entry.ld"
    for path in (traps, trap_header, harness, linker_script):
        if not path.is_file():
            fail(f"required input is missing: {path}")

    EVIDENCE.mkdir(parents=True, exist_ok=True)
    preprocessed = EVIDENCE / "traps-smp-fpu.preprocessed.S"
    trap_object = EVIDENCE / "traps-smp-fpu.rv32.o"
    fixture_object = EVIDENCE / "harness.rv32.o"
    elf = EVIDENCE / "p4-isr-entry.elf"
    map_file = EVIDENCE / "p4-isr-entry.map"

    common = [f"--sysroot={SYSROOT}", f"-march={MARCH}", f"-mabi={MABI}",
              f"-mcmodel={MCMODEL}"]
    preprocess = [str(COMPILER), *common, "-E", "-P", "-x",
                  "assembler-with-cpp", "-D__AROSEXEC_SMP__=1",
                  "-DP4_E3_FPU=1", "-I", str(KERNEL), str(traps),
                  "-o", str(preprocessed)]
    need_success(preprocess)
    preprocessed_source = preprocessed.read_text()
    check_preprocessed_sequence(preprocessed_source)

    assemble_traps = [str(COMPILER), *common, "-O2", "-Wall", "-Wextra",
                      "-Werror", "-x", "assembler", "-c", str(preprocessed),
                      "-o", str(trap_object)]
    assemble_fixture = [str(COMPILER), *common, "-O2", "-Wall", "-Wextra",
                        "-Werror", "-x", "assembler-with-cpp", "-c",
                        str(harness), "-o", str(fixture_object)]
    need_success(assemble_traps)
    need_success(assemble_fixture)
    # The AROS GCC driver uses collect-aros, which deliberately emits a
    # relocatable image. Link directly with ld.bfd for QEMU's executable ELF.
    link = [str(LINKER), "-m", "elf32lriscv", "--build-id=none",
            "-T", str(linker_script), "-Map", str(map_file),
            "-o", str(elf), str(fixture_object), str(trap_object)]
    need_success(link)
    image = elf.read_bytes()
    if len(image) < 28 or image[:4] != b"\x7fELF":
        fail("linked fixture is not an ELF image")
    elf_type = int.from_bytes(image[16:18], "little")
    entry = int.from_bytes(image[24:28], "little")
    if elf_type != 2 or entry != 0x80000000:
        fail(f"fixture ELF must be ET_EXEC at 0x80000000; got type={elf_type}, entry=0x{entry:08x}")
    (EVIDENCE / "image.identity.txt").write_text(
        f"elf_type=ET_EXEC ({elf_type})\nentry=0x{entry:08x}\n"
        f"size={len(image)}\nsha256={hashlib.sha256(image).hexdigest()}\n")

    emulator = [QEMU, "-machine", "virt", "-cpu", "max", "-m", "128M",
                "-bios", "none", "-kernel", str(elf), "-nographic",
                "-monitor", "none", "-serial", "none", "-no-reboot"]
    result = run(emulator, timeout=15)
    (EVIDENCE / "qemu.log").write_text(
        f"exit_status={result.returncode}\n{result.stdout}")
    if result.returncode != 0:
        fail(f"QEMU exit {result.returncode}; expected the pass finisher\n{result.stdout}")

    preprocessed_hash = hashlib.sha256(preprocessed.read_bytes()).hexdigest()
    manifest = source_manifest([traps, trap_header, harness, linker_script,
                                Path(__file__)])
    (EVIDENCE / "source.sha256").write_text(
        manifest + f"{preprocessed_hash}  {preprocessed.name}\n")
    (EVIDENCE / "commands.txt").write_text("\n".join(COMMANDS) + "\n")
    (EVIDENCE / "execution-model.txt").write_text(
        "This executes the preprocessed production traps.S in generic QEMU "
        "RISC-V virt, not ESP32-P4 silicon. The fixture injects a CLINT "
        "machine software interrupt to exercise nested entry and deferred "
        "MIE restoration. It does not qualify CLIC/NMI/fault behavior, P4 "
        "interrupt-controller routing, cache/interconnect timing, multicore "
        "coherency, or hardware startup. The RISC-V AROS GCC driver emits "
        "ET_REL for this target; the test links its RV32 objects with "
        "riscv-aros-ld.bfd and checks ET_EXEC/entry 0x80000000 before QEMU.\n")
    (EVIDENCE / "link-diagnosis.txt").write_text(
        "Initial attempt through riscv-aros-gcc/collect-aros produced ELF "
        "ET_REL with section VMAs and entry address zero; QEMU then repeatedly "
        "faulted fetching PC 0. The trace is retained as qemu-debug.log.gz. "
        "Using riscv-aros-ld.bfd directly produced ET_EXEC with entry "
        "0x80000000; the bounded rerun reached the pass finisher (QEMU exit "
        "status 0). This was a fixture-linking issue, not a production trap "
        "failure.\n")
    print("PASS: actual SMP+P4_E3_FPU trap entry executed under QEMU")
    print("PASS: outer/nested markers, interrupted SP, task migration, integer registers, MIE timing")
    print(f"Evidence: {EVIDENCE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
