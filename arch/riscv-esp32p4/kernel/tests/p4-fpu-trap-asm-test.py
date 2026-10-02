#!/usr/bin/env python3
"""Assemble and inspect both P4 trap-entry modes without touching a build tree."""

import pathlib
import re
import subprocess
import sys
import tempfile


ISA = "rv32imafc_zicsr_zifencei_zaamo_zalrsc"
FPU_REGISTER_ABI_NAMES = [
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7",
    "fs0", "fs1", "fa0", "fa1", "fa2", "fa3", "fa4", "fa5",
    "fa6", "fa7", "fs2", "fs3", "fs4", "fs5", "fs6", "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11",
]


def run(command):
    return subprocess.run(command, check=True, text=True,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT).stdout


def normalized(line):
    return re.sub(r"\s+", " ", line.strip())


def assemble_and_dump(toolbin, source, enabled, tempdir):
    suffix = "fpu" if enabled else "baseline"
    obj = tempdir / f"traps-{suffix}.o"
    command = [
        str(toolbin / "riscv-aros-gcc"),
        "-march=" + ISA,
        "-mabi=ilp32f",
        "-mcmodel=medany",
    ]
    if enabled:
        command.append("-DP4_E3_FPU=1")
    command += ["-c", str(source), "-o", str(obj)]
    run(command)
    return run([str(toolbin / "riscv-aros-objdump"), "-dr", str(obj)])


def check_baseline(dump):
    lines = [normalized(line) for line in dump.splitlines()]
    if not any(re.search(r"addi sp,sp,-144\b", line) for line in lines):
        raise AssertionError("baseline trap frame is not 144 bytes")
    fp_instruction = re.compile(
        r"\b(?:fsw|flw|fmv\.[ws]\.x|(?:fr|fs)csr|csrr\s+\w+,fcsr|"
        r"csrw\s+fcsr)\b")
    if any(fp_instruction.search(line) for line in lines):
        raise AssertionError("baseline trap entry unexpectedly contains FPU ops")


def check_fpu_mode(dump):
    lines = [normalized(line) for line in dump.splitlines()]
    if not any(re.search(r"addi sp,sp,-288\b", line) for line in lines):
        raise AssertionError("P4_E3_FPU trap frame is not 288 bytes")
    call_lines = [i for i, line in enumerate(lines)
                  if re.search(r"R_RISCV_CALL(?:_PLT)?\s+krnTrapHandler\b", line)]
    if not call_lines:
        raise AssertionError("cannot find krnTrapHandler call relocation")
    call = call_lines[0]

    saves = [i for i, line in enumerate(lines)
             if re.search(r"\bfsw\b", line)]
    if len(saves) != 32 or max(saves) >= call:
        raise AssertionError("all 32 F registers must be spilled before C")
    save_slots = [(match.group(1), int(match.group(2)))
                  for line in lines
                  if (match := re.search(
                      r"\bfsw\s+(\w+),(-?\d+)\(t0\)", line))]
    expected_slots = [(register, index * 4)
                      for index, register in enumerate(
                          FPU_REGISTER_ABI_NAMES)]
    if save_slots != expected_slots:
        raise AssertionError(
            "entry spill must map f0-f31 to their exact 4-byte slots")
    fcsr_capture = [i for i, line in enumerate(lines)
                    if i < call and re.search(r"\bfrcsr\s+t1\b", line)]
    fcsr_spill = [i for i, line in enumerate(lines)
                  if i < call and re.search(r"\bsw\s+t1,128\(t0\)", line)]
    if not fcsr_capture or not fcsr_spill or max(fcsr_capture) >= min(fcsr_spill):
        raise AssertionError("fcsr is not captured before C")
    fcsr_reset = [i for i, line in enumerate(lines)
                  if i < call and re.search(r"\bfscsr\s+zero\b", line)]
    if not fcsr_reset or max(fcsr_spill) >= max(fcsr_reset) or \
            max(fcsr_reset) >= call:
        raise AssertionError("kernel C must start after entry fcsr reset")
    if sum(1 for i, line in enumerate(lines)
           if i < call and re.search(r"\bfmv\.[ws]\.x\b", line)) != 32:
        raise AssertionError("FS=OFF entry must initialize all F registers")
    entry_zero = [match.group(1)
                  for i, line in enumerate(lines)
                  if i < call and (match := re.search(
                      r"\bfmv\.[ws]\.x\s+(\w+),zero\b", line))]
    if entry_zero != FPU_REGISTER_ABI_NAMES:
        raise AssertionError("FS=OFF entry must zero each F register")

    restores = [i for i, line in enumerate(lines)
                if i > call and re.search(r"\bflw\b", line)]
    if len(restores) != 32:
        raise AssertionError("all 32 F registers must be restored after C")
    restore_slots = [(match.group(1), int(match.group(2)))
                     for i, line in enumerate(lines)
                     if i > call and (match := re.search(
                         r"\bflw\s+(\w+),(-?\d+)\(t0\)", line))]
    if restore_slots != expected_slots:
        raise AssertionError(
            "dispatch restore must map all F registers to exact slots")
    if sum(1 for i, line in enumerate(lines)
           if i > call and re.search(r"\bfmv\.[ws]\.x\b", line)) != 32:
        raise AssertionError("NULL task image must initialize all F registers")
    exit_zero = [match.group(1)
                 for i, line in enumerate(lines)
                 if i > call and (match := re.search(
                     r"\bfmv\.[ws]\.x\s+(\w+),zero\b", line))]
    if exit_zero != FPU_REGISTER_ABI_NAMES:
        raise AssertionError("NULL task image must zero each F register")
    if not any(i > call and re.search(r"\b(?:fcsr|fscsr)\b", line) and
               re.search(r"\b(?:csrw|fscsr)\b", line)
               for i, line in enumerate(lines)):
        raise AssertionError("fcsr is not restored after C")
    fpu_restore_ops = [i for i, line in enumerate(lines)
                       if i > call and re.search(
                           r"\b(?:flw|fmv\.[ws]\.x|(?:csrw|fscsr).*fcsr)\b",
                           line)]
    mstatus_writes = [i for i, line in enumerate(lines)
                      if i > call and "mstatus" in line and
                      re.search(r"\bcsrw\b", line)]
    if not mstatus_writes or mstatus_writes[-1] <= max(fpu_restore_ops):
        raise AssertionError("selected mstatus must be restored after F state")
    if not any(re.search(r"addi t0,sp,144\b", line) for line in lines):
        raise AssertionError("entry FPU spill must begin at byte 144")
    if not any(re.search(r"sw t0,128\(sp\)", line) for line in lines):
        raise AssertionError("entry spill pointer is not installed in context")
    if not any(re.search(r"lw sp,4\(sp\)", line) for line in lines):
        raise AssertionError("selected task stack pointer is not restored")


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CROSS_TOOL_BIN", file=sys.stderr)
        return 2

    toolbin = pathlib.Path(sys.argv[1]).resolve()
    kernel = pathlib.Path(__file__).resolve().parents[1]
    source = kernel / "traps.S"
    with tempfile.TemporaryDirectory(prefix="p4-fpu-trap-") as temp:
        tempdir = pathlib.Path(temp)
        baseline = assemble_and_dump(toolbin, source, False, tempdir)
        fpu = assemble_and_dump(toolbin, source, True, tempdir)
        check_baseline(baseline)
        check_fpu_mode(fpu)

    print("PASS P4 trap assembly (baseline and P4_E3_FPU)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
