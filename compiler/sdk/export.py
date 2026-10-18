#!/usr/bin/env python3
"""Refresh source SDK input hashes without altering graph or ABI policy."""

import argparse
import hashlib
import json
from pathlib import Path
import stat
import sys
import tomllib


ROOTS = ["core-linklibs", "includes", "linklibs"]


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


def regular_source(root, name):
    relative = Path(name)
    if (not isinstance(name, str) or relative.is_absolute()
            or not relative.parts or str(relative) != name
            or any(part in (".", "..") for part in relative.parts)):
        raise ValueError("unsafe source input: " + str(name))
    cursor = root
    for part in relative.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise ValueError("symlink source input: " + name)
    metadata = cursor.stat()
    if (not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1
            or metadata.st_size > 16 * 1024 * 1024):
        raise ValueError("nonregular, aliased or oversized source input: " + name)
    return cursor.read_bytes()


def export(root):
    profiles = tomllib.loads(regular_source(root, "aros-targets.toml").decode())
    names = [target["name"] for target in profiles["targets"]]
    if len(names) != len(set(names)):
        raise ValueError("duplicate source preset")
    outputs = []
    paths = set()
    for profile in profiles["targets"]:
        name = profile.get("sdk_consumer_contract")
        if name is None:
            continue
        if profile.get("native_consumer_contract") is not None or name in paths:
            raise ValueError("ambiguous SDK source contract")
        paths.add(name)
        original = regular_source(root, name)
        contract = json.loads(original, object_pairs_hook=unique_object)
        if (contract["schema"] != "aros-sdk-consumer-contract-v1"
                or contract["profile"] != profile["name"]):
            raise ValueError("SDK consumer schema or profile differs")
        if contract["roots"] != ROOTS:
            raise ValueError("SDK consumer requires the complete sorted SDK roots")
        inputs = [entry["path"] for entry in contract["inputs"]]
        if (not 1 <= len(inputs) <= 128 or len(set(inputs)) != len(inputs)
                or name in inputs or "aros-targets.toml" not in inputs):
            raise ValueError("ambiguous, recursive or incomplete source inputs")
        probes = contract["native_sdk_link_probes"]
        if set(probes) != {"c", "cxx"}:
            raise ValueError("expected exactly C and C++ application probes")
        for language, suffix in (("c", ".c"), ("cxx", ".cpp")):
            probe = probes[language]
            if (set(probe) != {"source", "libraries"}
                    or probe["source"] not in inputs
                    or not probe["source"].endswith(suffix)):
                raise ValueError("application probe must be a sealed source input")
        measured = []
        identities = set()
        for path in sorted(inputs):
            content = regular_source(root, path)
            metadata = (root / path).stat()
            identity = (metadata.st_dev, metadata.st_ino)
            if identity in identities:
                raise ValueError("source inputs alias one regular file")
            identities.add(identity)
            measured.append({"path": path, "sha256": hashlib.sha256(content).hexdigest()})
        contract["inputs"] = measured
        outputs.append((root / name, original, (json.dumps(contract, indent=2) + "\n").encode()))
    if not outputs:
        raise ValueError("no source SDK consumer contracts declared")
    return outputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--check", action="store_true")
    modes.add_argument("--update", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    outputs = export(root)  # Validate all declarations before changing any one.
    if args.check:
        if any(original != measured for _, original, measured in outputs):
            raise ValueError("SDK source seals are stale; review inputs before --update")
    else:
        for path, original, _ in outputs:
            if path.read_bytes() != original:
                raise ValueError("SDK declaration changed during input observation")
        for path, _, measured in outputs:
            path.write_bytes(measured)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("SDK source export: " + str(error), file=sys.stderr)
        sys.exit(1)
