#!/usr/bin/env python3
"""Fail closed when an artifact is missing or exceeds a byte limit."""

import argparse
import os
import stat
import sys


def parse_limit(value):
    try:
        limit = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "must be an integer byte limit") from exc
    if limit < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return limit


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", help="file whose actual size is checked")
    parser.add_argument("limit", type=parse_limit,
                        help="maximum allowed size in bytes")
    parser.add_argument("--label", default="Artifact",
                        help="name used in the status message")
    args = parser.parse_args(argv)

    try:
        metadata = os.stat(args.artifact)
    except OSError as exc:
        print("%s: cannot stat %s: %s" % (
            args.label, args.artifact, exc), file=sys.stderr)
        return 1

    if not stat.S_ISREG(metadata.st_mode):
        print("%s: %s is not a regular file" % (
            args.label, args.artifact), file=sys.stderr)
        return 1

    size = metadata.st_size
    if size <= 0:
        print("%s: %s is empty" % (args.label, args.artifact),
              file=sys.stderr)
        return 1
    if size > args.limit:
        print("%s: %d bytes exceeds the %d-byte limit" % (
            args.label, size, args.limit), file=sys.stderr)
        return 1

    print("%s: %d of %d bytes" % (args.label, size, args.limit))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
