#!/usr/bin/env python3
"""Check saved ESP32-P4 E2 primitive logs without hardware access."""

import argparse
import json
import re
import sys
from pathlib import Path


STAGE_LINES = {
    "psram": b"[smp-e2] PSRAM PASS; shared/physical=66 publication-refusal=1 refusals=6 guards=ok",
    "atomics": b"[smp-e2] ATOMICS PASS; AMO/LRSC/lock=16384 each forced-contention=1",
    "ipi_fence": b"[smp-e2] IPI/FENCE PASS; harts=0,1 wrong-destination/absent-trigger/stale-ack refused remote-code=37,53",
    "cache": b"[smp-e2] CACHE PASS; missing-park refused SRAM stacks remote resume PSRAM reload=ok",
}
STAGE_PATTERNS = {
    name: re.compile(rb"(?m)^" + re.escape(line) + rb"\r?$")
    for name, line in STAGE_LINES.items()
}
MISA_RE = re.compile(
    rb"(?m)^\[smp-e2\] misa0=0x([0-9a-fA-F]{8}) misa1=0x([0-9a-fA-F]{8})\r?$"
)
CONCURRENT_RETRIES_RE = re.compile(
    rb"(?m)^\[smp-e2\] concurrent retries0=0x([0-9a-fA-F]{8}) "
    rb"retries1=0x([0-9a-fA-F]{8})\r?$"
)
SUPPRESSED_RE = re.compile(
    rb"(?m)^\[smp\] suppressed state=0x00000000 hart=0x00000000 "
    rb"echo=0x00000000 cause=0x00000000 pc=0x00000000 "
    rb"result=0x00000000 reset=held clock=off\r?$"
)
RELEASE_RE = re.compile(
    rb"(?mi)^\[smp\] release state=0x00000001 hart=0x00000001 "
    rb"echo=(0xe1000002|0xe2000003) cause=0x00000000 pc=0x00000000 "
    rb"result=0x00000001 reset=held clock=off\r?$"
)
PRIMITIVES_PASS_RE = re.compile(
    rb"(?m)^\[smp-e2\] PRIMITIVES PASS; epochs=2 secondary stopped\r?$"
)
READY_RE = re.compile(rb"(?m)^\[smp-e2\] READY; primary parked before Exec\r?$")
FAILURE_RE = re.compile(rb"(?i)\bfail\b|\[trap\]|\bfault\b|panic|guru meditation|\bunsafe\b")


def analyze_log(data):
    """Return marker counts, order checks and overall E2 log result."""
    stage_matches = {name: list(pattern.finditer(data)) for name, pattern in STAGE_PATTERNS.items()}
    stage_positions = {name: [m.start() for m in matches] for name, matches in stage_matches.items()}
    misa_matches = list(MISA_RE.finditer(data))
    retry_matches = list(CONCURRENT_RETRIES_RE.finditer(data))
    misa_reports = [
        {"misa0": int(match.group(1), 16), "misa1": int(match.group(2), 16)}
        for match in misa_matches
    ]
    retry_reports = []
    for match in retry_matches:
        retries0 = int(match.group(1), 16)
        retries1 = int(match.group(2), 16)
        retry_reports.append({
            "retries0": retries0,
            "retries1": retries1,
            "total": retries0 + retries1,
        })
    suppressed = list(SUPPRESSED_RE.finditer(data))
    releases = list(RELEASE_RE.finditer(data))
    pass_markers = list(PRIMITIVES_PASS_RE.finditer(data))
    ready_markers = list(READY_RE.finditer(data))
    release_echoes = [match.group(1).lower() for match in releases]

    stages_twice = all(len(stage_matches[name]) == 2 for name in STAGE_LINES)
    misa_has_a_extension = (
        len(misa_reports) == 2
        and all((report["misa0"] & 1) and (report["misa1"] & 1) for report in misa_reports)
    )
    concurrent_contention_observed = (
        len(retry_reports) == 2 and all(report["total"] > 0 for report in retry_reports)
    )
    release_pair = len(releases) == 2 and release_echoes == [b"0xe1000002", b"0xe2000003"]
    ordered = False
    if (len(suppressed) == len(pass_markers) == len(ready_markers) == 1
            and release_pair and stages_twice and len(misa_matches) == 2
            and len(retry_matches) == 2):
        positions = [suppressed[0].start()]
        for epoch in range(2):
            positions.extend((
                stage_positions["psram"][epoch],
                misa_matches[epoch].start(),
                retry_matches[epoch].start(),
                stage_positions["atomics"][epoch],
                stage_positions["ipi_fence"][epoch],
                stage_positions["cache"][epoch],
            ))
            if epoch == 0:
                positions.append(releases[0].start())
        positions.extend([releases[1].start(), pass_markers[0].start(), ready_markers[0].start()])
        ordered = all(left < right for left, right in zip(positions, positions[1:]))

    no_failure = FAILURE_RE.search(data) is None
    criteria = {
        "one_suppressed_result_zero_report": len(suppressed) == 1,
        "two_positive_releases_held_clock_off": release_pair,
        "every_primitive_stage_once_per_epoch": stages_twice,
        "two_misa_reports_with_a_extension": misa_has_a_extension,
        "two_concurrent_retry_reports_with_contention": concurrent_contention_observed,
        "two_epochs_in_stage_order": ordered,
        "final_primitives_pass_once": len(pass_markers) == 1,
        "ready_after_final_pass_once": len(ready_markers) == 1 and ordered,
        "no_fail_fault_or_unsafe_marker": no_failure,
    }
    counts = {
        "suppressed_result_zero_reports": len(suppressed),
        "positive_release_reports": len(releases),
        "positive_release_echoes": [echo.decode("ascii") for echo in release_echoes],
        "stage_reports": {name: len(matches) for name, matches in stage_matches.items()},
        "stage_positions": stage_positions,
        "misa_reports": misa_reports,
        "misa_positions": [match.start() for match in misa_matches],
        "concurrent_retry_reports": retry_reports,
        "concurrent_retry_positions": [match.start() for match in retry_matches],
        "primitives_pass_reports": len(pass_markers),
        "ready_reports": len(ready_markers),
        "failure_markers": len(FAILURE_RE.findall(data)),
    }
    return {"passed": all(criteria.values()), "criteria": criteria, "counts": counts}


def check_file(path):
    try:
        data = Path(path).read_bytes()
    except OSError as exc:
        return {"path": str(path), "passed": False, "error": f"{type(exc).__name__}: {exc}"}
    return {"path": str(path), **analyze_log(data)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="one or more saved UART log files")
    args = parser.parse_args(argv)
    results = [check_file(path) for path in args.logs]
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
