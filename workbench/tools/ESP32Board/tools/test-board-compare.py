#!/usr/bin/env python3
"""Compile and exercise compare_file.c with modeled source and target hooks."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


DEFAULT_EVIDENCE_ROOT = Path(
    "/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/wifi-2026-10-11"
)
SUMMARY = re.compile(r"RESULT cases=(\d+) checks=(\d+) failures=(\d+)")


def run_logged(command: list[str], cwd: Path, env: dict[str, str],
               log_path: Path) -> tuple[int, str, float]:
    started = time.monotonic()
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=180,
        )
        output = result.stdout
        exit_code = result.returncode
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        output += "\nTimed out after 180 seconds.\n"
        exit_code = 124
    elapsed = time.monotonic() - started
    log_path.write_text(
        "$ " + " ".join(command) + "\n" + output,
        encoding="utf-8",
    )
    return exit_code, output, elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--evidence-root",
        type=Path,
        default=DEFAULT_EVIDENCE_ROOT,
        help="parent directory for a fresh, private run directory",
    )
    parser.add_argument(
        "--cc",
        default=shutil.which("clang") or shutil.which("cc") or "cc",
        help="host C compiler (default: clang, then cc)",
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[4]
    source_dir = repo / "workbench/tools/ESP32Board"
    harness = source_dir / "tools/test-board-compare.c"
    inputs = {
        "compare_file": source_dir / "compare_file.c",
        "harness": harness,
    }
    missing = [str(path) for path in inputs.values() if not path.is_file()]
    if missing:
        print("Missing test inputs:", *missing, sep="\n  ", file=sys.stderr)
        return 2

    compiler = shutil.which(args.cc) or args.cc
    args.evidence_root.mkdir(parents=True, exist_ok=True)
    evidence_dir = Path(tempfile.mkdtemp(
        prefix="c6-compare-flow-tests.", dir=args.evidence_root
    ))

    base_env = os.environ.copy()
    base_env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    base_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    include_flags = ["-I", str(source_dir)]
    matrix = []
    all_passed = True

    for optimization in ("-O0", "-O2"):
        label = optimization[1:]
        run_dir = evidence_dir / label
        run_dir.mkdir()
        common = [
            compiler,
            "-std=c11",
            optimization,
            "-g",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            *include_flags,
        ]
        phase_records = []
        objects = []
        compile_ok = True

        for name in ("compare_file", "harness"):
            obj = run_dir / f"{name}.o"
            command = [*common, "-c", str(inputs[name]), "-o", str(obj)]
            exit_code, _, elapsed = run_logged(
                command, repo, base_env, run_dir / f"compile-{name}.log"
            )
            phase_records.append({
                "phase": f"compile-{name}",
                "exit_code": exit_code,
                "elapsed_seconds": round(elapsed, 3),
                "log": str(run_dir / f"compile-{name}.log"),
            })
            if exit_code != 0:
                compile_ok = False
                all_passed = False
                continue
            objects.append(str(obj))

        executable = run_dir / "test-board-compare"
        if compile_ok and len(objects) == len(inputs):
            link_command = [*common, *objects, "-o", str(executable)]
            exit_code, _, elapsed = run_logged(
                link_command, repo, base_env, run_dir / "link.log"
            )
            phase_records.append({
                "phase": "link",
                "exit_code": exit_code,
                "elapsed_seconds": round(elapsed, 3),
                "log": str(run_dir / "link.log"),
            })
            if exit_code != 0:
                all_passed = False
            else:
                test_exit, test_output, elapsed = run_logged(
                    [str(executable)], repo, base_env, run_dir / "test.log"
                )
                match = SUMMARY.search(test_output)
                summary = None
                if match:
                    summary = {
                        "cases": int(match.group(1)),
                        "checks": int(match.group(2)),
                        "failures": int(match.group(3)),
                    }
                phase_records.append({
                    "phase": "run",
                    "exit_code": test_exit,
                    "elapsed_seconds": round(elapsed, 3),
                    "summary": summary,
                    "log": str(run_dir / "test.log"),
                })
                if test_exit != 0 or not summary or summary["failures"] != 0:
                    all_passed = False
        else:
            phase_records.append({
                "phase": "link-and-run",
                "exit_code": None,
                "detail": "Skipped because a source compile failed.",
            })

        matrix.append({
            "optimization": label,
            "sanitizers": ["address", "undefined"],
            "warnings_as_errors": True,
            "phases": phase_records,
        })

    manifest = {
        "purpose": "Host-only control-flow tests of the actual C6 compare operation.",
        "compiler": compiler,
        "repository": str(repo),
        "evidence_root": str(args.evidence_root.resolve()),
        "evidence_dir": str(evidence_dir.resolve()),
        "production_sources_compiled": [str(inputs["compare_file"])],
        "fixture_scope": "Mocked owned-source, discovery and target-comparison hooks; no hardware, radio, flash, target claim or recovery claim.",
        "leak_check": "LeakSanitizer is disabled for the host ASan runtime; exact modeled allocation/free counts and active ownership are checked on every path.",
        "matrix": matrix,
        "exit_code": 0 if all_passed else 1,
    }
    (evidence_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    print(f"evidence_dir={evidence_dir.resolve()}")
    for entry in matrix:
        run = next((phase for phase in entry["phases"]
                    if phase["phase"] == "run"), None)
        if run:
            summary = run.get("summary") or {}
            print(
                f"{entry['optimization']}: exit={run['exit_code']} "
                f"cases={summary.get('cases', 'n/a')} "
                f"checks={summary.get('checks', 'n/a')} "
                f"failures={summary.get('failures', 'n/a')} "
                f"log={run['log']}"
            )
        else:
            failed = [phase for phase in entry["phases"]
                      if phase.get("exit_code") not in (0, None)]
            print(f"{entry['optimization']}: compile/link failed: {failed}")
    print(f"manifest={evidence_dir / 'manifest.json'}")
    print(f"exit={0 if all_passed else 1}")
    return 0 if all_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
