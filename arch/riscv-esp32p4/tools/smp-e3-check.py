#!/usr/bin/env python3
"""Check saved ESP32-P4 E3 CPU-local diagnostic logs."""

import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path


E2_CHECKER_PATH = Path(__file__).with_name("smp-e2-check.py")
E2_SPEC = importlib.util.spec_from_file_location("smp_e2_check", E2_CHECKER_PATH)
if E2_SPEC is None or E2_SPEC.loader is None:
    raise ImportError(f"cannot load E2 checker from {E2_CHECKER_PATH}")
E2_CHECKER = importlib.util.module_from_spec(E2_SPEC)
sys.modules[E2_SPEC.name] = E2_CHECKER
E2_SPEC.loader.exec_module(E2_CHECKER)


CPU_LOCAL_PASS_LINE = (
    b"[smp-e3] CPU-LOCAL PASS; harts=0,1 iterations=8192 "
    b"owner/bounds/nesting=ok"
)
CPU_LOCAL_PASS_RE = re.compile(
    rb"(?m)^" + re.escape(CPU_LOCAL_PASS_LINE) + rb"\r?$"
)
CPU_LOCAL_FAIL_RE = re.compile(rb"(?mi)^\[smp-e3\] CPU-LOCAL FAIL;[^\r\n]*\r?$")
RELEASE_LINE_RE = re.compile(rb"(?mi)^\[smp\] release\b[^\r\n]*\r?$")
SPINLOCK_PASS_LINE = (
    b"[smp-e3] SPINLOCK PASS; harts=0,1 writes=8192 readers=2 placement/refusal=ok"
)
SPINLOCK_PASS_RE = re.compile(rb"(?m)^" + re.escape(SPINLOCK_PASS_LINE) + rb"\r?$")
SPINLOCK_FAIL_RE = re.compile(rb"(?mi)^\[smp-e3\] SPINLOCK FAIL;[^\r\n]*\r?$")
SOFT_ATOMIC_PASS_LINE = (
    b"[smp-e3] SOFT-ATOMIC PASS; harts=0,1 cached-PSRAM adds=8192 "
    b"old-values/widths/guards/refusal=ok"
)
SOFT_ATOMIC_PASS_RE = re.compile(
    rb"(?m)^" + re.escape(SOFT_ATOMIC_PASS_LINE) + rb"\r?$"
)
SOFT_ATOMIC_RECORD_RE = re.compile(
    rb"(?mi)^\[smp-e3\] SOFT-ATOMIC(?:\s|;)[^\r\n]*\r?$"
)
SOFT_ATOMIC_FAIL_RE = re.compile(
    rb"(?mi)^\[smp-e3\] SOFT-ATOMIC FAIL;[^\r\n]*\r?$"
)


def analyze_log(data, require_spinlock=False, require_soft_atomic=False):
    """Return E2/E3 checks, adding optional spinlock and soft-atomic gates."""
    e2 = E2_CHECKER.analyze_log(data)
    pass_markers = list(CPU_LOCAL_PASS_RE.finditer(data))
    fail_markers = list(CPU_LOCAL_FAIL_RE.finditer(data))
    pass_positions = [match.start() for match in pass_markers]

    atomic_positions = e2["counts"]["stage_positions"]["atomics"]
    ipi_positions = e2["counts"]["stage_positions"]["ipi_fence"]
    each_epoch_between_stages = (
        len(pass_markers) == 2
        and len(atomic_positions) == 2
        and len(ipi_positions) == 2
        and all(
            atomic_positions[epoch] < pass_positions[epoch] < ipi_positions[epoch]
            for epoch in range(2)
        )
    )

    counts = e2["counts"]
    release_echoes = counts["positive_release_echoes"]
    exactly_two_releases = (
        len(RELEASE_LINE_RE.findall(data)) == 2
        and counts["positive_release_reports"] == 2
        and release_echoes == ["0xe1000002", "0xe2000003"]
    )
    final_e2_ready = (
        counts["primitives_pass_reports"] == 1
        and counts["ready_reports"] == 1
        and e2["criteria"]["ready_after_final_pass_once"]
    )

    criteria = {
        "e2_log_passes": e2["passed"],
        "exactly_two_e2_release_epochs": exactly_two_releases,
        "final_e2_pass_and_ready_once": final_e2_ready,
        "two_cpu_local_passes": len(pass_markers) == 2,
        "cpu_local_pass_once_between_atomics_and_ipi_per_epoch": each_epoch_between_stages,
        "no_cpu_local_failure_marker": len(fail_markers) == 0,
    }
    if require_spinlock:
        positions = [m.start() for m in SPINLOCK_PASS_RE.finditer(data)]
        criteria["spinlock_pass_once_after_cpu_local_before_ipi_per_epoch"] = (
            len(positions) == 2 and len(pass_positions) == 2 and len(ipi_positions) == 2
            and all(pass_positions[i] < positions[i] < ipi_positions[i] for i in range(2))
        )
        criteria["no_spinlock_failure_marker"] = not SPINLOCK_FAIL_RE.search(data)

    result = {
        "passed": all(criteria.values()),
        "criteria": criteria,
        "counts": {
            "e2": counts,
            "e2_release_lines": len(RELEASE_LINE_RE.findall(data)),
            "cpu_local_pass_reports": len(pass_markers),
            "cpu_local_pass_positions": pass_positions,
            "cpu_local_fail_reports": len(fail_markers),
        },
    }
    if require_soft_atomic:
        soft_records = list(SOFT_ATOMIC_RECORD_RE.finditer(data))
        soft_passes = list(SOFT_ATOMIC_PASS_RE.finditer(data))
        soft_failures = list(SOFT_ATOMIC_FAIL_RE.finditer(data))
        soft_positions = [match.start() for match in soft_passes]
        spin_positions = [m.start() for m in SPINLOCK_PASS_RE.finditer(data)]
        soft_once_per_epoch = (
            len(soft_records) == 2
            and len(soft_passes) == 2
            and len(spin_positions) == 2
            and len(ipi_positions) == 2
            and all(
                spin_positions[epoch] < soft_positions[epoch] < ipi_positions[epoch]
                for epoch in range(2)
            )
        )
        criteria["soft_atomic_requires_spinlock"] = require_spinlock
        criteria["two_soft_atomic_passes"] = len(soft_passes) == 2
        criteria["soft_atomic_pass_once_after_spinlock_before_ipi_per_epoch"] = (
            soft_once_per_epoch
        )
        criteria["no_soft_atomic_failure_or_malformed_record"] = (
            len(soft_records) == 2
            and len(soft_passes) == len(soft_records)
            and len(soft_failures) == 0
        )
        result["passed"] = all(criteria.values())
        result["counts"].update({
            "soft_atomic_records": len(soft_records),
            "soft_atomic_pass_reports": len(soft_passes),
            "soft_atomic_pass_positions": soft_positions,
            "soft_atomic_fail_reports": len(soft_failures),
            "soft_atomic_malformed_reports": len(soft_records) - len(soft_passes),
        })
    return result


def check_file(path, require_spinlock=False, require_soft_atomic=False):
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        return {"path": str(path), "passed": False, "error": f"{type(exc).__name__}: {exc}"}
    return {
        "path": str(path),
        **analyze_log(data, require_spinlock, require_soft_atomic),
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spinlock", action="store_true", help="also require the E3 SRAM read/write lock stage")
    parser.add_argument(
        "--soft-atomic",
        action="store_true",
        help="also require the two-hart cached-PSRAM software-atomic stage (requires --spinlock)",
    )
    parser.add_argument("logs", nargs="+", type=Path, help="one or more saved UART log files")
    args = parser.parse_args(argv)
    if args.soft_atomic and not args.spinlock:
        parser.error("--soft-atomic requires --spinlock")
    results = [
        check_file(path, args.spinlock, args.soft_atomic) for path in args.logs
    ]
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
