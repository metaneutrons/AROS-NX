#!/usr/bin/env python3
#
#   Judge a P4_GIANT headless capture against fixed criteria.
#
#   usage: giant-check.py <log> [--seconds N] [--stress] [--tests N]
#
#   Passes (exit 0) only if:
#     - the self-test printed "SUMMARY pass=<tests> fail=0";
#     - with --seconds N, every statistics line t=10, 20, ... N is present;
#     - every statistics line reports contimeout=0;
#     - with --stress, the stress line appears at least N/10 - 2 times, its
#       rounds never decrease, errors stay 0, and both the Forbid() and the
#       semaphore total equal the sum of the workers' own counts within the
#       four workers that may be inside the section at sampling time;
#     - both harts keep dispatching: in every 60 s window of statistics
#       lines each hart's dispatch count grows (a frozen hart is a stall,
#       even if everything else looks clean);
#     - with --stress, both harts' share of stress rounds grows in every
#       60 s window as well;
#     - none of the failure markers appears.
#
#   The runtime console is lossy outside these lines, so the absence of an
#   ordinary boot message proves nothing and is not checked here.
#

import re
import sys

FAILURES = [
    "[trap]", "fatal", "Alert", "[MM]", "did not park", "park refused",
    "lacks F", "eviction still pending", "spawn failed",
]


def progress(what, series, window=60):
    """Both values must grow within every window; returns problem strings."""
    out = []
    times = sorted(series)
    for hart in (0, 1):
        for i, t in enumerate(times):
            later = [u for u in times[i + 1:] if u - t >= window]
            if not later:
                break
            u = later[0]
            if series[u][hart] <= series[t][hart]:
                out.append("%s of hart %d did not grow from t=%d to t=%d (%d)"
                           % (what, hart, t, u, series[t][hart]))
                break
    return out


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("usage: giant-check.py <log> [--seconds N] [--stress] [--tests N]")
    path = args[0]
    seconds = None
    stress = False
    tests = 10
    i = 1
    while i < len(args):
        if args[i] == "--seconds":
            seconds = int(args[i + 1])
            i += 2
        elif args[i] == "--stress":
            stress = True
            i += 1
        elif args[i] == "--tests":
            tests = int(args[i + 1])
            i += 2
        else:
            sys.exit("unknown argument " + args[i])

    text = open(path, "rb").read().decode("latin-1")
    problems = []

    if not re.search(r"\[giant-test\] SUMMARY pass=%d fail=0\b" % tests, text):
        problems.append("no 'SUMMARY pass=%d fail=0'" % tests)

    stats = [int(m.group(1)) for m in re.finditer(r"\[giant\] t=(\d+) \|", text)]
    if seconds is not None:
        missing = [t for t in range(10, seconds + 1, 10) if t not in stats]
        if missing:
            problems.append("missing statistics lines t=%s" % missing[:10])
    disp = {}
    for m in re.finditer(r"\[giant\] t=(\d+) \| h0 disp=(\d+) .*? \| h1 disp=(\d+) ", text):
        disp[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    problems += progress("dispatch", disp)
    bad_console = re.findall(r"contimeout=([1-9]\d*)", text)
    if bad_console:
        problems.append("console lock timeouts %s" % bad_console[:5])

    if stress:
        lines = re.findall(
            r"\[giant-stress\] rounds=(\d+) on_h0=(\d+) on_h1=(\d+) "
            r"forbid=(\d+)/(\d+) sem=(\d+)/(\d+) errors=(\d+)", text)
        need = (seconds // 10 - 2) if seconds else 1
        if len(lines) < need:
            problems.append("%d stress lines, need %d" % (len(lines), need))
        last = -1
        for n, (rounds, h0, h1, ft, fo, st, so, err) in enumerate(lines):
            rounds, h0, h1 = int(rounds), int(h0), int(h1)
            ft, fo, st, so, err = int(ft), int(fo), int(st), int(so), int(err)
            if rounds < last:
                problems.append("stress rounds went down at line %d" % n)
            last = rounds
            if err:
                problems.append("stress errors=%d at line %d" % (err, n))
            if not (0 <= ft - fo <= 4) and not (0 <= fo - ft <= 4):
                problems.append("forbid %d/%d at line %d" % (ft, fo, n))
            if not (0 <= st - so <= 4) and not (0 <= so - st <= 4):
                problems.append("semaphore %d/%d at line %d" % (st, so, n))
        # Stress lines follow the statistics line of the same tick.
        share = {}
        for m in re.finditer(r"\[giant\] t=(\d+) \|[^\n]*\n(?:[^\n]*\n){0,2}?"
                             r"\[giant-stress\] rounds=\d+ on_h0=(\d+) on_h1=(\d+)",
                             text):
            share[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
        # The first minute is the boot; workers start after the self-test.
        problems += progress("stress share", {t: v for t, v in share.items() if t >= 60})
        if lines:
            rounds, h0, h1 = (int(x) for x in lines[-1][:3])
            print("stress: %d lines, %d rounds, hart0 %d, hart1 %d"
                  % (len(lines), rounds, h0, h1))

    for marker in FAILURES:
        count = text.count(marker)
        if count:
            problems.append("%d x %r" % (count, marker))

    print("statistics lines: %d, last t=%s" % (len(stats), stats[-1] if stats else None))
    if problems:
        for p in problems:
            print("FAIL: " + p)
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
