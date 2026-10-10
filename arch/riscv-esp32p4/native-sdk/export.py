#!/usr/bin/env python3
"""Refresh explicit source-input seals, not configuration or graph policy."""

import argparse
import hashlib
import json
from pathlib import Path
import stat
import sys


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def regular_source(root, name):
    relative = Path(name)
    if relative.is_absolute() or not relative.parts or any(
        part in (".", "..") for part in relative.parts
    ) or str(relative) != name:
        raise ValueError("unsafe source input: " + name)
    cursor = root
    for part in relative.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ValueError("symlink source input: " + name)
    if not stat.S_ISREG(cursor.stat().st_mode) or cursor.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("nonregular or oversized source input: " + name)
    return cursor.read_bytes()


def export(root):
    name = "arch/riscv-esp32p4/native-sdk/consumer-v2.json"
    original = regular_source(root, name)
    contract = json.loads(original, object_pairs_hook=unique_object)
    if contract["schema"] != "aros-native-consumer-contract-v2":
        raise ValueError("unsupported consumer schema")
    paths = [entry["path"] for entry in contract["inputs"]]
    if not 1 <= len(paths) <= 128 or len(set(paths)) != len(paths) or name in paths:
        raise ValueError("ambiguous, recursive or excessive source inputs")
    probes = contract["native_sdk_link_probes"]
    if set(probes) != {"c", "cxx"}:
        raise ValueError("expected exactly C and C++ application probes")
    for language, suffix in (("c", ".c"), ("cxx", ".cpp")):
        probe = probes[language]
        if set(probe) != {"source", "libraries"}:
            raise ValueError("unknown application probe metadata")
        if probe["source"] not in paths or not probe["source"].endswith(suffix):
            raise ValueError("application probe must be a sealed source input")
    contract["inputs"] = [
        {"path": path, "sha256": hashlib.sha256(regular_source(root, path)).hexdigest()}
        for path in sorted(paths)
    ]
    return root / name, original, (json.dumps(contract, indent=2) + "\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--update", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    path, original, measured = export(root)
    if args.check:
        if original != measured:
            raise ValueError("consumer source seals are stale; review inputs before --update")
    else:
        path.write_bytes(measured)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("native SDK export: " + str(error), file=sys.stderr)
        sys.exit(1)
