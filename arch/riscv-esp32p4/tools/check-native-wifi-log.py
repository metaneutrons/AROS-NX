#!/usr/bin/env python3
"""Check one complete passive native WiFi diagnostic report, not WLAN readiness.

Fresh C6 power, device/artifact identity and successful reader process exit
are separate gates. This parser cannot prove rail-off, physical deadlines,
installed firmware lineage, association, traffic or shared-controller safety.
ROM output in this passive capture prevents accepting continuity.
"""
import argparse
import json
import math
import re
from pathlib import Path


def validate(text, require_scan=False):
    if any(signature.lower() in text.lower() for signature in (
            "ESP-ROM:", "waiting for download", "Guru Meditation",
            "Software Failure", "panic", "unhandled trap", "[trap]",
            "*** alert ***", "SW_SYS_RESET",
            "WDT_SYS_RESET", "HP_SYS_HP_WDT_RESET",
            "CPU has been reset by WDT", "watchdog reset")):
        raise ValueError("fault/reset signature or unestablished continuity")
    if text.count("[host start ") != 1 or text.count("[host end ") != 1:
        raise ValueError("capture must have exactly one start and end")
    head = re.match(r"\[host start [^\r\n]+\]\n", text)
    end = re.search(r"\n\[host end \+([0-9.]+)s bytes=([0-9]+) status=ok\]\n?\Z", text)
    if not head or not end:
        raise ValueError("missing successful completed passive capture")
    duration = float(end[1])
    payload = text[head.end():end.start()]
    if not math.isfinite(duration) or duration < 30:
        raise ValueError("capture window shorter than 30 seconds")
    if int(end[2]) != len(payload.encode("utf-8")) or not payload:
        raise ValueError("capture byte count inconsistent or empty")

    def report(prefix, fields):
        first = next(iter(fields))
        lines = [line for line in payload.splitlines()
                 if line.startswith("[WiFi] " + prefix + first + "=")]
        if len(lines) != 1:
            raise ValueError("missing or duplicate report: " + prefix)
        tokens = lines[0][len("[WiFi] " + prefix):].split()
        values = {}
        for token in tokens:
            pair = token.split("=", 1)
            if len(pair) != 2 or pair[0] in values:
                raise ValueError("malformed report: " + prefix)
            values[pair[0]] = pair[1]
        if set(values) != set(fields):
            raise ValueError("missing/unknown fields: " + prefix)
        result = {}
        for key, base in fields.items():
            pattern = r"[0-9a-fA-F]+" if base == 16 else r"-?[0-9]+"
            if not re.fullmatch(pattern, values[key]):
                raise ValueError("invalid integer: " + prefix + key)
            result[key] = int(values[key], base)
            if not 0 <= result[key] <= 0xffffffff:
                raise ValueError("integer out of range: " + prefix + key)
        return result

    def expect(prefix, fields, expected):
        values = report(prefix, fields)
        if any(values[key] != value for key, value in expected.items()):
            raise ValueError("unsuccessful diagnostic: " + prefix)
        return values

    def decimals(names):
        return dict.fromkeys(names.split(), 10)

    expect("slot1 probe ", decimals("ok stage cmd"), {"ok": 1})
    # This report has no separating label before its first key.
    expect("", decimals("error status"), {"error": 0, "status": 0})
    expect("setup ", decimals("attempted ok stage error"),
           {"attempted": 1, "ok": 1, "error": 0})
    expect("restore ", decimals("ok stage error mutated"),
           {"ok": 1, "error": 0, "mutated": 1})
    expect("INIT ", decimals("attempted ok stage error"),
           {"attempted": 1, "ok": 1, "error": 0})
    expect("INIT ", decimals("preopen ambiguous poisoned"),
           {"preopen": 0, "ambiguous": 0, "poisoned": 0})
    expect("INIT ", {"chip": 16, "caps": 16, "bytes": 10}, {"chip": 13})
    expect("INIT cleanup ", {**decimals("ok error"), "flags": 16},
           # hosted_open.h: NOT_RUN=0, CLEANUP_OK=1. This full setup
           # profile also requires all14 attempted/verified cleanup flags.
           {"ok": 1, "error": 1, "flags": 0x3fff})
    expect("reply ", decimals("attempted ok stage error polls"),
           {"attempted": 1, "ok": 1, "error": 0})
    expect("reply ", decimals("started complete bytes sent poison"),
           {"started": 1, "complete": 1, "poison": 0})
    expect("WLAN ", dict.fromkeys(("attempted", "valid", "written"), 16),
           {"attempted": 31, "valid": 31, "written": 31})
    expect("WLAN ", decimals("init mode start stop deinit cleanup"),
           dict.fromkeys(("init", "mode", "start", "stop", "deinit", "cleanup"), 1))
    for label in ("status", "error"):
        expect("WLAN " + label + " ", decimals("init mode start stop deinit"),
               dict.fromkeys(("init", "mode", "start", "stop", "deinit"), 0))
    expect("MAC ", decimals("attempted ok error polls"),
           {"attempted": 1, "ok": 1, "error": 0})
    expect("MAC ", decimals("matched status events unmatched"),
           {"matched": 1, "status": 0, "unmatched": 0})
    if sum(line.startswith("[WiFi] MAC valid=") for line in payload.splitlines()) != 1:
        raise ValueError("missing or duplicate MAC report")
    macs = re.findall(r"^\[WiFi\] MAC valid=1 ([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})$",
                      payload, re.MULTILINE)
    if len(macs) != 1:
        raise ValueError("missing or duplicate valid MAC")
    mac = bytes.fromhex(macs[0].replace(":", ""))
    if not any(mac) or mac[0] & 1:
        raise ValueError("invalid station MAC")
    # Dynamic status/command/input registers must not be compared for equality.
    for index in range(24):
        values = report("reg%d " % index, {"before": 16, "after": 16})
        if index < 6 and values["before"] != values["after"]:
            raise ValueError("shared-controller configuration not restored")
    result = {"mac": macs[0].lower(), "ap_count": None,
              "record_count": None, "empty_workaround": None,
              "scope": "native diagnostic only; external hardware gates remain"}
    if require_scan:
        scan = expect("SCAN ", decimals("attempted ok aps records empty_workaround cleanup"),
                      {"attempted": 1, "ok": 1, "cleanup": 1})
        if not 0 <= scan["aps"] <= 65535 or scan["records"] != min(scan["aps"], 8):
            raise ValueError("scan count/records mismatch")
        empty = int(scan["aps"] == 0)
        if scan["empty_workaround"] != empty:
            raise ValueError("invalid empty-result workaround")
        expect("SCAN valid ", decimals("start count records stop"),
               {"start": 1, "count": 1, "records": 1 - empty, "stop": 0})
        expect("SCAN status ", decimals("start count records stop"),
               dict.fromkeys(("start", "count", "records", "stop"), 0))
        expect("SCAN error ", decimals("start count records stop polls"),
               dict.fromkeys(("start", "count", "records", "stop"), 0))
        result.update(ap_count=scan["aps"], record_count=scan["records"],
                      empty_workaround=bool(empty))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--scan", action="store_true")
    args = parser.parse_args()
    try:
        result = validate(args.capture.read_bytes().decode("utf-8"), args.scan)
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, "native diagnostic rejected: %s\n" % error)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
