#!/usr/bin/env python3
"""Validate the saved serial-log record from the opt-in P4 FPU probe.

This checks only the hart-0 task-context-switch diagnostic record.  It does
not verify SMP concurrency, display output, or visual acceptance.
"""

import argparse
from pathlib import Path
import re
import sys


RECORD_RE = re.compile(
    r"P4_E3_FPU_PROBE=(?P<status>PASS|FAIL)"
    r"\s+scope=hart0-context-switch-only"
    r"\s+task0_iterations=(?P<task0_iterations>[0-9]+)"
    r"\s+task0_ticks=(?P<task0_ticks>[0-9]+)"
    r"\s+task0_dispatch_delta=(?P<task0_dispatch_delta>[0-9]+)"
    r"\s+task0_peer_interleaved=(?P<task0_peer_interleaved>[0-9]+)"
    r"\s+task0_detail=(?P<task0_detail>[0-9]+)"
    r"\s+task1_iterations=(?P<task1_iterations>[0-9]+)"
    r"\s+task1_ticks=(?P<task1_ticks>[0-9]+)"
    r"\s+task1_dispatch_delta=(?P<task1_dispatch_delta>[0-9]+)"
    r"\s+task1_peer_interleaved=(?P<task1_peer_interleaved>[0-9]+)"
    r"\s+task1_detail=(?P<task1_detail>[0-9]+)"
    r"\s+smp_concurrency=not-tested"
)

FATAL_TRAP_RE = re.compile(
    r"\[trap\]\s*fatal\b|\b(?:fatal\s+trap|trap\s+fatal)\b",
    re.IGNORECASE,
)
FATAL_SYSTEM_RE = re.compile(
    r"\b(?:alert|guru(?:\s+meditation)?|panic)\b", re.IGNORECASE
)
PROBE_FAIL_RE = re.compile(r"P4_E3_FPU_PROBE=FAIL\b", re.IGNORECASE)
STANDALONE_FAIL_LINE_RE = re.compile(
    r"^\s*(?:\[(?:FAIL|FAILED)\]|(?:FAIL|FAILED))"
    r"(?:\s*[:=-].*)?\s*$",
    re.IGNORECASE | re.MULTILINE,
)
SERIAL_PASS_RE = re.compile(
    r"\bserial(?:[_ -](?:run|execution))?\s*(?:=|:)\s*"
    r"(?:1|true|yes|pass)\b",
    re.IGNORECASE,
)

POSITIVE_FIELDS = (
    "task0_iterations",
    "task0_ticks",
    "task0_dispatch_delta",
    "task1_iterations",
    "task1_ticks",
    "task1_dispatch_delta",
)


class ValidationError(ValueError):
    """The saved log does not contain one valid FPU probe record."""


def validate_log(text):
    """Return the parsed record if *text* contains one valid probe result."""
    if FATAL_TRAP_RE.search(text) or FATAL_SYSTEM_RE.search(text):
        raise ValidationError("fatal trap, Alert, Guru, or panic marker present")
    if PROBE_FAIL_RE.search(text) or STANDALONE_FAIL_LINE_RE.search(text):
        raise ValidationError("FAIL marker present")
    if SERIAL_PASS_RE.search(text):
        raise ValidationError("serial-execution marker present")

    markers = list(re.finditer(r"P4_E3_FPU_PROBE=", text))
    if not markers:
        raise ValidationError("missing P4_E3_FPU_PROBE record")
    if len(markers) != 1:
        raise ValidationError("expected exactly one P4_E3_FPU_PROBE record")

    marker = markers[0]
    line_start = text.rfind("\n", 0, marker.start()) + 1
    line_end = text.find("\n", marker.start())
    if line_end < 0:
        line_end = len(text)
    line = text[line_start:line_end].strip().rstrip("\r")
    match = RECORD_RE.fullmatch(line)
    if not match:
        raise ValidationError("incomplete or malformed P4_E3_FPU_PROBE record")

    values = match.groupdict()
    if values["status"] != "PASS":
        raise ValidationError("probe status is not PASS")

    numeric = {key: int(value) for key, value in values.items()
               if key != "status"}
    if any(value > 0xFFFFFFFF for value in numeric.values()):
        raise ValidationError("counter is outside the uint32 range")
    for field in POSITIVE_FIELDS:
        if numeric[field] <= 0:
            raise ValidationError("{} must be positive".format(field))

    for task_index in (0, 1):
        if numeric["task{}_peer_interleaved".format(task_index)] != 1:
            raise ValidationError(
                "task{} did not observe a running peer".format(task_index)
            )
        if numeric["task{}_detail".format(task_index)] != 0:
            raise ValidationError(
                "task{} reported a nonzero detail code".format(task_index)
            )

    return numeric


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=(
            "Validate the saved P4 hart-0 FPU context-switch probe record; "
            "this does not verify SMP concurrency or visual acceptance."
        )
    )
    parser.add_argument("logs", nargs="+", type=Path, metavar="LOG")
    args = parser.parse_args(argv)

    failed = False
    for path in args.logs:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
            validate_log(text)
        except (OSError, ValidationError) as error:
            print("FAIL: {}: {}".format(path, error), file=sys.stderr)
            failed = True
        else:
            print(
                "VALID: {}: one hart-0 FPU context-switch record; "
                "SMP concurrency not tested; no visual acceptance implied".format(path)
            )

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
