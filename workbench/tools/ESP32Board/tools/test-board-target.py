#!/usr/bin/env python3
"""Compile and run host tests against the actual ESP32Board target comparator."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


SUMMARY = re.compile(r"RESULT cases=(\d+) checks=(\d+) failures=(\d+)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cc",
        default=(os.environ.get("CC") or shutil.which("clang") or "cc"),
        help="host C compiler (default: CC, then clang, then cc)",
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[4]
    app = repo / "workbench/tools/ESP32Board"
    boardtool = repo / "arch/riscv-esp32p4/boardtool"
    sources = [
        app / "target.c",
        boardtool / "discovery.c",
        app / "tools/test-board-target.c",
    ]
    missing = [path for path in sources if not path.is_file()]
    if missing:
        print("Missing test inputs:", *missing, sep="\n  ", file=sys.stderr)
        return 2

    compiler = shlex.split(args.cc)
    if not compiler:
        parser.error("--cc must name a compiler")
    compiler[0] = shutil.which(compiler[0]) or compiler[0]

    env = os.environ.copy()
    # Allocation ownership is not part of this comparator; ASan/UBSan remain
    # active while avoiding LeakSanitizer's unsupported mode on Apple Clang.
    env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    all_passed = True
    with tempfile.TemporaryDirectory(prefix="board-target-tests-") as temp:
        temp_dir = Path(temp)
        for optimization in ("-O0", "-O2"):
            executable = temp_dir / f"test-board-target-{optimization[2:]}"
            command = [
                *compiler,
                "-std=c11",
                optimization,
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I",
                str(app),
                "-I",
                str(boardtool),
                *(str(path) for path in sources),
                "-o",
                str(executable),
            ]
            print("$ " + shlex.join(command))
            try:
                built = subprocess.run(
                    command,
                    cwd=repo,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=120,
                )
            except subprocess.TimeoutExpired:
                print(f"{optimization}: compile timed out", file=sys.stderr)
                return 1
            if built.returncode:
                print(built.stdout, end="", file=sys.stderr)
                print(f"{optimization}: compile failed ({built.returncode})",
                    file=sys.stderr)
                return built.returncode

            run_command = [str(executable)]
            print("$ " + shlex.join(run_command))
            try:
                ran = subprocess.run(
                    run_command,
                    cwd=repo,
                    env=env,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    check=False,
                    timeout=60,
                )
            except subprocess.TimeoutExpired:
                print(f"{optimization}: test run timed out", file=sys.stderr)
                return 1
            match = SUMMARY.search(ran.stdout)
            if ran.returncode or not match or int(match.group(3)):
                print(ran.stdout, end="", file=sys.stderr)
                print(f"{optimization}: test run failed ({ran.returncode})",
                    file=sys.stderr)
                all_passed = False
                continue
            print(
                f"{optimization}: {match.group(1)} cases, "
                f"{match.group(2)} checks; ASan/UBSan passed"
            )

    print("result=" + ("PASS" if all_passed else "FAIL"))
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
