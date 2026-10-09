#!/usr/bin/env python3
"""Validate complete late [pq] reports; this does not prove reader absence.

The capture's process completion and no-reader procedure are separate gates.
Missing, contradictory, reset-lost or smoke results never qualify production.
"""
import argparse
import json
import math
import re
from pathlib import Path

FIELDS = {"state", "duration", "goal", "cpu0", "cpu1", "cpuerr0", "cpuerr1", "sdreads",
          "sdmismatch", "sderrors", "gfxupdates", "reason", "fixture"}
RECORD = re.compile(r"\[pq\] id=([0-9a-fA-F]{8}) (.*)")
FOOTER = re.compile(r"\[host end \+([0-9.]+)s bytes=([0-9]+) status=ok\]\s*\Z")


def validate(text, *, smoke=False):
    if "ESP-ROM:" in text:
        raise ValueError("ROM banner in late capture: continuity is not established")
    run_id = None
    values = {}
    for line in text.splitlines():
        match = RECORD.search(line)
        if not match:
            continue
        ident, payload = match.groups()
        if run_id is not None and ident.lower() != run_id:
            raise ValueError("multiple run identities")
        run_id = ident.lower()
        if len(match.group(0).encode("utf-8")) + 1 > 64:
            raise ValueError("report exceeds endpoint packet budget")
        for token in payload.split():
            pair = token.split("=", 1)
            if len(pair) != 2 or pair[0] not in FIELDS:
                raise ValueError("malformed report field")
            key, value = pair
            if key != "state":
                if not re.fullmatch(r"[0-9]+", value) or int(value) > 0xffffffff:
                    raise ValueError("invalid unsigned counter")
                value = int(value)
            if key in values and values[key] != value:
                raise ValueError("contradictory retained result")
            values[key] = value
    if set(values) != FIELDS:
        raise ValueError("missing retained fields: " + ",".join(sorted(FIELDS - set(values))))
    if values["state"] != ("SMOKE" if smoke else "PASS"):
        raise ValueError("terminal state does not satisfy requested gate")
    goal, duration = values["goal"], values["duration"]
    if not 30 <= goal <= 3600 or (smoke and goal >= 1800) or (not smoke and goal < 1800):
        raise ValueError("invalid goal for gate")
    if duration < goal:
        raise ValueError("incomplete duration")
    if any(values[key] for key in ("reason", "sderrors", "sdmismatch", "cpuerr0", "cpuerr1")):
        raise ValueError("nonzero failure result")
    if values["fixture"] != 1:
        raise ValueError("reference fixture was not verified")
    minimum = (duration + 1) // 2
    for key in ("cpu0", "cpu1", "sdreads", "gfxupdates"):
        if values[key] < minimum:
            raise ValueError("insufficient sustained coverage: " + key)
    return {"id": run_id, **values, "gate": "smoke" if smoke else "production"}


def validate_capture(text, *, smoke=False):
    footer = FOOTER.search(text)
    if footer is None or not text.startswith("[host start "):
        raise ValueError("missing successful completed late-capture envelope")
    seconds = float(footer[1])
    if not math.isfinite(seconds) or seconds < 40 or int(footer[2]) == 0:
        raise ValueError("late retrieval interval is incomplete or empty")
    return validate(text, smoke=smoke)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    try:
        result = validate_capture(args.capture.read_text(errors="replace"), smoke=args.smoke)
    except (OSError, ValueError) as error:
        parser.exit(1, "qualification rejected: %s\n" % error)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
