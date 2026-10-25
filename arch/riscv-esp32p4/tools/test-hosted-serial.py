#!/usr/bin/env python3
"""Build and run the standalone pserial codec tests without touching a build tree."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
NETWORK = ROOT / "network"
SOURCES = (
    NETWORK / "hosted_serial.h",
    NETWORK / "hosted_serial.c",
    NETWORK / "test_hosted_serial.c",
    Path(__file__).resolve(),
)
RV32_CC = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-v3-build/bin/darwin-aarch64/"
    "tools/crosstools/riscv-aros-gcc"
)


def run(label: str, command: list[str], environment: dict[str, str] | None = None) -> int:
    print(f"RUN {label}: {shlex.join(command)}", flush=True)
    try:
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False,
                                env=environment, timeout=60)
    except subprocess.TimeoutExpired:
        print(f"FAIL {label}: exceeded 60 seconds", flush=True)
        return 1
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    print(f"EXIT {label}: {result.returncode}", flush=True)
    return result.returncode


def main() -> int:
    for path in SOURCES:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"SHA256 {path.relative_to(ROOT.parent)} {digest}")

    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        print("FAIL: CC is empty")
        return 1
    sanitizer_environment = os.environ.copy()
    sanitizer_environment["UBSAN_OPTIONS"] = "halt_on_error=1"
    sanitizer_environment["ASAN_OPTIONS"] = "halt_on_error=1"
    failures = 0
    with tempfile.TemporaryDirectory(prefix="hosted-serial-tests-") as temp:
        temp_path = Path(temp)
        for optimization in ("0", "2"):
            binary = temp_path / f"test-hosted-serial-o{optimization}"
            command = compiler + [
                "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                f"-O{optimization}", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-I", str(NETWORK),
                str(NETWORK / "hosted_serial.c"),
                str(NETWORK / "test_hosted_serial.c"), "-o", str(binary),
            ]
            compile_status = run(f"native-O{optimization}-compile", command)
            failures += compile_status != 0
            if compile_status == 0:
                failures += run(f"native-O{optimization}-test", [str(binary)],
                                sanitizer_environment) != 0

        if RV32_CC.is_file() and os.access(RV32_CC, os.X_OK):
            object_path = temp_path / "hosted_serial-rv32.o"
            command = [
                str(RV32_CC), "-std=c99", "-Wall", "-Wextra", "-Werror",
                "-pedantic", "-I", str(NETWORK), "-c",
                str(NETWORK / "hosted_serial.c"), "-o", str(object_path),
            ]
            rv32_status = run("rv32-standalone-compile", command)
            failures += rv32_status != 0
            if rv32_status == 0:
                print(f"RV32 object: {object_path.stat().st_size} bytes, "
                      f"SHA256 {hashlib.sha256(object_path.read_bytes()).hexdigest()}")
        else:
            print(f"SKIP rv32-standalone-compile: compiler unavailable ({RV32_CC})")

    print(f"RESULT {'FAIL' if failures else 'PASS'} ({failures} failed step(s))")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
