#!/usr/bin/env python3
"""Reproduce checked-bridge host and RV32 macro/object checks (no hardware)."""
import argparse
import hashlib
import pathlib
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", required=True, type=pathlib.Path)
parser.add_argument("--evidence", required=True, type=pathlib.Path)
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parents[4]
evidence = args.evidence.resolve()
evidence.mkdir(parents=True, exist_ok=True)
build = args.build.resolve()
tool = build / "bin/darwin-aarch64/tools/crosstools/riscv-aros-"
generated = build / "bin/esp32p4-riscv/gen/include"
sdk = build / "bin/esp32p4-riscv/AROS/Developer"
kernel = source / "arch/riscv-esp32p4/kernel"


def run(command, name):
    result = subprocess.run([str(x) for x in command], cwd=source,
                            capture_output=True, text=True, check=False)
    (evidence / (name + ".log")).write_text(
        "$ " + shlex.join([str(x) for x in command]) + "\n" +
        result.stdout + result.stderr + f"\nexit={result.returncode}\n")
    if result.returncode:
        raise RuntimeError(f"{name} failed; inspect its retained log")
    return result.stdout


headers = {
    "aros/platform_atomic.h": source / "arch/riscv-esp32p4/include/aros/platform_atomic.h",
    "aros/atomic_ops.h": source / "compiler/include/aros/atomic_ops.h",
    "aros/atomic.h": source / "compiler/arossupport/include/atomic.h",
    "aros/kernel.h": source / "compiler/include/aros/kernel.h",
}
for relative, original in headers.items():
    for destination in (generated, sdk / "include"):
        if (destination / relative).read_bytes() != original.read_bytes():
            raise RuntimeError(f"stale generated header: {destination / relative}")

host = evidence / "atomic-bridge-host"
run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
     "-fsanitize=address,undefined", "-g", "-I", source / "compiler/include",
     kernel / "tests/p4-atomic-ops-test.c", "-o", host], "atomic-bridge-host-build")
print(run([host], "atomic-bridge-host-run").strip())

flags = [str(tool) + "gcc", f"--sysroot={sdk}",
         "-march=rv32imafc_zicsr_zifencei_zaamo_zalrsc", "-mabi=ilp32f",
         "-mcmodel=medany", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-O2", "-I", generated]
identities = []
for name, filename, defines in (
    ("bridge", "p4_atomic_ops.c", []),
    ("macro-core", "tests/p4-atomic-macro-compile.c",
     ["-D__AROSEXEC_SMP__=1", "-DP4_ATOMIC_CORE=1"]),
    ("macro-public", "tests/p4-atomic-macro-compile.c",
     ["-D__AROSEXEC_SMP__=1"]),
):
    obj = evidence / ("atomic-bridge-" + name + ".o")
    run(flags + defines + ["-c", kernel / filename, "-o", obj],
        "atomic-bridge-" + name + "-compile")
    disassembly = run([str(tool) + "objdump", "-dr", obj],
                      "atomic-bridge-" + name + "-disassembly")
    if re.search(r"\b(?:amo\w*\.|lr\.|sc\.)", disassembly):
        raise RuntimeError(f"direct RMW instruction in {name}")
    symbols = run([str(tool) + "nm", "-u", obj],
                  "atomic-bridge-" + name + "-undefined")
    if name == "macro-public" and symbols.split() != ["U", "SysBase"]:
        raise RuntimeError("public macro gained unresolved core/runtime symbols")
    if name == "macro-core" and symbols.split() != ["U", "krnP4AtomicCheckedOperate"]:
        raise RuntimeError("core macro does not use the direct checked bridge")
    data = obj.read_bytes()
    identities.append(f"{hashlib.sha256(data).hexdigest()}  {obj.name}  {len(data)} bytes")
(evidence / "atomic-bridge-identities.txt").write_text("\n".join(identities) + "\n")
print("PASS: copied headers, checked host bridge, three RV32 objects and RMW exclusion")
