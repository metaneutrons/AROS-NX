#!/usr/bin/env python3
"""Pin an out-of-tree P4 build to one board/layout; reject stale reuse."""
import argparse
import json
import os
from pathlib import Path
import re
import tempfile


def check(build, board, layout):
    identity = {"board": board, "layout": layout}
    marker = build / "config/esp32p4-build-profile.json"
    if marker.exists():
        if json.loads(marker.read_text()) != identity:
            raise ValueError("board/layout changed: use a separate build directory")
        return
    # Historical trees have no marker. Never admit an existing kernel as AB.
    if layout == "ab" and (list(build.glob("bin/*/gen/rom/boot/core.elf")) or
                           list(build.glob("bin/*/gen/rom/kernel/kernel/arch/kernel_startup.o"))):
        raise ValueError("unmarked existing kernel: use a fresh A/B build directory")
    marker.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=".p4-profile-", dir=marker.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            json.dump(identity, output, sort_keys=True)
            output.write("\n")
        try:
            # Publish complete contents exclusively, even with parallel makes.
            os.link(temporary, marker)
        except FileExistsError:
            if json.loads(marker.read_text()) != identity:
                raise ValueError("concurrent board/layout mismatch")
    finally:
        os.unlink(temporary)


def board_name(value):
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]*", value):
        raise argparse.ArgumentTypeError("invalid board identifier")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    # Supported-board admission belongs to board.mk, not a second registry.
    parser.add_argument("board", type=board_name)
    parser.add_argument("layout", choices=("legacy", "ab"))
    args = parser.parse_args()
    try:
        check(args.build, args.board, args.layout)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"P4 build profile rejected: {exc}\n")
    print("OK")


if __name__ == "__main__":
    main()
