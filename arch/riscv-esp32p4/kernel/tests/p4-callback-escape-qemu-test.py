#!/usr/bin/env python3
"""Execute actual RV32 callback escape at O0/O2 with ABI register sentinels.

Generic QEMU virt only: runtime, CSR-depth and service ownership are fixtures,
not production P4 trap/registry/concurrency qualification.
"""
import hashlib
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[4]
    kernel = root / "arch/riscv-esp32p4/kernel"
    build = root.parent / "AROS-ESP32-build"
    tools = build / "bin/darwin-aarch64/tools/crosstools"
    fixtures = kernel / "tests/fixtures"
    evidence = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else (
        build / "evidence/smp-priority-2026-10-01/callback-escape-qemu")
    evidence.mkdir(parents=True, exist_ok=True)
    qemu = shutil.which("qemu-system-riscv32")
    if qemu is None:
        raise SystemExit("qemu-system-riscv32 is required")
    commands = []

    def run(command):
        commands.append(shlex.join(command))
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=15, check=False)
        if result.returncode != 0:
            raise SystemExit(f"FAIL {commands[-1]}\n{result.stdout}{result.stderr}")
        return result.stdout + result.stderr

    common = [str(tools / "riscv-aros-gcc"),
              f"--sysroot={build}/bin/esp32p4-riscv/AROS/Developer",
              "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc", "-mabi=ilp32f",
              "-mcmodel=medany", "-ffreestanding", "-fno-stack-protector",
              "-Wall", "-Wextra", "-Werror"]
    harness = evidence / "harness.o"
    run([*common, "-c", str(fixtures / "p4-callback-escape-harness.S"),
         "-o", str(harness)])
    transcript = []
    for opt in ("O0", "O2"):
        obj = evidence / f"callback-{opt}.o"
        image = evidence / f"callback-{opt}.elf"
        run([*common, f"-{opt}", "-c",
             str(fixtures / "p4-callback-escape-harness.c"), "-o", str(obj)])
        link_output = run([str(tools / "riscv-aros-ld.bfd"), "-m",
                           "elf32lriscv", "--build-id=none", "-T",
                           str(fixtures / "p4-callback-escape.ld"), "-o",
                           str(image), str(obj), str(harness)])
        data = image.read_bytes()
        if data[:4] != b"\x7fELF" or int.from_bytes(data[16:18], "little") != 2 or (
                int.from_bytes(data[24:28], "little") != 0x80000000):
            raise SystemExit("FAIL: fixture must be ET_EXEC at 0x80000000")
        output = run([qemu, "-machine", "virt", "-cpu", "max", "-m", "128M",
                      "-bios", "none", "-kernel", str(image), "-nographic",
                      "-monitor", "none", "-serial", "none", "-no-reboot"])
        transcript.append(f"{opt}: QEMU pass finisher, exit=0, size={len(data)}, "
                          f"sha256={hashlib.sha256(data).hexdigest()}\n"
                          f"{link_output}{output}")
    inputs = [kernel / "p4_callback_escape.c", kernel / "p4_callback_escape.h",
              fixtures / "p4-callback-escape-harness.c",
              fixtures / "p4-callback-escape-harness.S",
              fixtures / "p4-callback-escape.ld", Path(__file__)]
    evidence.joinpath("source.sha256").write_text("\n".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p}" for p in inputs) + "\n")
    evidence.joinpath("commands.txt").write_text("\n".join(commands) + "\n")
    evidence.joinpath("results.txt").write_text("\n".join(transcript))
    print("PASS: actual RV32 callback unwind O0/O2; hart-slot models; "
          "s1/s11/fs0/fs11 sentinels; ordinary transfer")
    print(f"Evidence: {evidence}")


if __name__ == "__main__":
    main()
