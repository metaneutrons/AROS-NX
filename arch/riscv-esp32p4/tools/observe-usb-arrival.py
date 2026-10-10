#!/usr/bin/env python3
"""Observe device-node presence without opening the serial device.

Polling brackets filesystem appearance/removal, not physical rail-on or USB
enumeration itself. Each transition lies between previous_sample_monotonic and
monotonic. Correlate with late-boot-log --timing on the SAME host and host boot.
The node must be observed absent before an appearance counts as a new arrival.
No serial/control-line access, reset, reconnect or firmware write is performed.
"""

import argparse
import datetime
import json
import math
import os
import time


def positive_duration(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be finite and positive")
    return seconds


def present(path):
    try:
        os.stat(path)
    except FileNotFoundError:
        return False
    return True  # Other errors invalidate observation, rather than mean absent.


def observe(path, seconds, log, *, interval=0.02, monotonic=time.monotonic,
            sleep=time.sleep, probe=present):
    with open(log, "x", encoding="utf-8") as output:
        start = monotonic()

        def emit(event, now, **fields):
            output.write(json.dumps(dict(event=event, monotonic=now,
                                         seconds=now - start, **fields)) + "\n")
            output.flush()

        failure = None
        try:
            previous = monotonic()
            state = probe(path)
            now = monotonic()
            emit("start", now, present=state, port=path,
                 wall_time=datetime.datetime.now().astimezone().isoformat(),
                 interval=interval, clock="host time.monotonic; same host/boot only")
            deadline = start + seconds
            while now < deadline:
                sleep(min(interval, max(0.0, deadline - now)))
                sample_started = monotonic()
                new_state = probe(path)
                now = monotonic()
                if new_state != state:
                    emit("appeared" if new_state else "removed", now,
                         previous_sample_monotonic=previous, present=new_state)
                    state = new_state
                # Lower bound precedes the previous stat, not its completion:
                # a transition may occur between stat and reading the clock.
                previous = sample_started
        except BaseException as error:
            failure = error
        finally:
            emit("end", monotonic(), status="ok" if failure is None else "error",
                 error=None if failure is None else str(failure))
        if failure is not None:
            raise failure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("seconds", type=positive_duration)
    parser.add_argument("log", help="new JSONL file (must not exist)")
    args = parser.parse_args()
    observe(args.port, args.seconds, args.log)


if __name__ == "__main__":
    main()
