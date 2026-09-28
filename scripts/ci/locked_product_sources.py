#!/usr/bin/env python3
"""Validate and stage the closed source inputs used by AROS-NX product CI."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import sys
from typing import Any


HERE = Path(__file__).resolve().parent


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def _relative_path(value: Any, label: str) -> PurePosixPath:
    if not isinstance(value, str) or not value or "\\" in value:
        raise ValueError(f"invalid {label}")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.split("/")):
        raise ValueError(f"unsafe {label}: {value}")
    return path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_contract(plan_path: Path, staging_path: Path, source_root: Path) -> list[dict[str, Any]]:
    plan = _read_json(plan_path)
    staging = _read_json(staging_path)
    if set(plan) != {"schema", "entries"} or plan["schema"] != "aros-cache-source-fetch-plan-v1":
        raise ValueError("unexpected source fetch plan schema")
    if set(staging) != {"schema", "entries"} or staging["schema"] != "aros-nx-product-source-staging-v1":
        raise ValueError("unexpected source staging schema")
    if not isinstance(plan["entries"], list) or not plan["entries"]:
        raise ValueError("source fetch plan must contain entries")
    if not isinstance(staging["entries"], list) or len(staging["entries"]) != len(plan["entries"]):
        raise ValueError("source staging must cover every fetch entry exactly once")

    by_role: dict[str, dict[str, Any]] = {}
    filenames: set[str] = set()
    for entry in plan["entries"]:
        if not isinstance(entry, dict):
            raise ValueError("invalid source fetch entry")
        role = entry.get("role")
        filename = entry.get("filename")
        integrity = entry.get("integrity")
        if not isinstance(role, str) or not role or role in by_role:
            raise ValueError("duplicate or invalid source role")
        if not isinstance(filename, str) or _relative_path(filename, "cache filename").name != filename:
            raise ValueError(f"invalid cache filename for {role}")
        if filename in filenames:
            raise ValueError(f"duplicate cache filename: {filename}")
        if not isinstance(integrity, dict) or integrity.get("kind") != "locked":
            raise ValueError(f"source is not locked: {role}")
        digest = integrity.get("sha256")
        size = integrity.get("size")
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ValueError(f"invalid SHA-256 for {role}")
        if type(size) is not int or size <= 0:
            raise ValueError(f"invalid size for {role}")
        by_role[role] = entry
        filenames.add(filename)

    result: list[dict[str, Any]] = []
    destinations: set[str] = set()
    for mapping in staging["entries"]:
        if not isinstance(mapping, dict) or set(mapping) != {"role", "destination", "source_guard"}:
            raise ValueError("invalid source staging entry")
        role = mapping["role"]
        if role not in by_role or any(item["role"] == role for item in result):
            raise ValueError(f"unmatched or duplicate staging role: {role}")
        destination = _relative_path(mapping["destination"], "staging destination")
        if destination.name != by_role[role]["filename"] or str(destination) in destinations:
            raise ValueError(f"invalid or duplicate staging destination for {role}")
        destinations.add(str(destination))
        guard = mapping["source_guard"]
        if not isinstance(guard, dict) or set(guard) != {"path", "line"}:
            raise ValueError(f"invalid source guard for {role}")
        guard_path = source_root.joinpath(*_relative_path(guard["path"], "guard path").parts)
        if guard_path.is_symlink() or not guard_path.is_file():
            raise ValueError(f"missing regular source guard for {role}")
        line = guard["line"]
        if not isinstance(line, str) or not line or "\n" in line or "\r" in line:
            raise ValueError(f"invalid source guard line for {role}")
        if line not in guard_path.read_text(encoding="utf-8").splitlines():
            raise ValueError(f"source revision no longer matches locked input: {role}")
        result.append({**by_role[role], "destination": destination})
    return result


def verify_cache(entries: list[dict[str, Any]], cache: Path, *, closed: bool = True) -> None:
    if cache.is_symlink() or not cache.is_dir():
        raise ValueError("source cache is not a regular directory")
    expected = {entry["filename"] for entry in entries}
    actual = {item.name for item in cache.iterdir()}
    if closed and actual != expected:
        raise ValueError(f"source cache inventory mismatch: expected {sorted(expected)}, found {sorted(actual)}")
    for entry in entries:
        path = cache / entry["filename"]
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"source cache entry is not a regular file: {path.name}")
        integrity = entry["integrity"]
        if path.stat().st_size != integrity["size"] or _sha256(path) != integrity["sha256"]:
            raise ValueError(f"source cache integrity mismatch: {path.name}")


def export(entries: list[dict[str, Any]], cache: Path, output: Path) -> None:
    # The aros-fetch cache may contain its own lifecycle and lock metadata.
    # Export only locked, verified source objects into a closed artifact root.
    verify_cache(entries, cache, closed=False)
    if output.exists() or output.is_symlink():
        raise ValueError("refusing to overwrite source export")
    output.mkdir(parents=True)
    for entry in entries:
        shutil.copyfile(cache / entry["filename"], output / entry["filename"])
    verify_cache(entries, output)


def stage(entries: list[dict[str, Any]], cache: Path, build_root: Path) -> None:
    verify_cache(entries, cache)
    if build_root.is_symlink() or (build_root.exists() and not build_root.is_dir()):
        raise ValueError("unsafe product build root")
    if (build_root / "CMakeCache.txt").exists():
        raise ValueError("source inputs must be staged before CMake configures")
    build_root.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        destination = build_root.joinpath(*entry["destination"].parts)
        current = build_root
        for component in entry["destination"].parts[:-1]:
            current = current / component
            if current.is_symlink() or (current.exists() and not current.is_dir()):
                raise ValueError(f"unsafe staging parent: {current}")
            current.mkdir(exist_ok=True)
        if destination.exists() or destination.is_symlink():
            raise ValueError(f"refusing to overwrite staged source: {destination}")
        temporary = destination.with_name(destination.name + ".staging")
        if temporary.exists() or temporary.is_symlink():
            raise ValueError(f"refusing to overwrite staging temporary: {temporary}")
        try:
            shutil.copyfile(cache / entry["filename"], temporary)
            integrity = entry["integrity"]
            if temporary.stat().st_size != integrity["size"] or _sha256(temporary) != integrity["sha256"]:
                raise ValueError(f"staged source changed while copying: {entry['filename']}")
            os.replace(temporary, destination)
        finally:
            if temporary.exists():
                temporary.unlink()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("validate", "verify", "export", "stage"))
    parser.add_argument("--plan", type=Path, default=HERE / "product-sources.plan.json")
    parser.add_argument("--staging", type=Path, default=HERE / "product-sources.staging.json")
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--build-root", type=Path)
    args = parser.parse_args()
    try:
        entries = load_contract(args.plan, args.staging, args.source_root)
        if args.command in ("verify", "export", "stage"):
            if args.cache is None:
                parser.error("--cache is required")
        if args.command in ("verify", "stage"):
            verify_cache(entries, args.cache)
        if args.command == "export":
            if args.output is None:
                parser.error("--output is required")
            export(entries, args.cache, args.output)
        if args.command == "stage":
            if args.build_root is None:
                parser.error("--build-root is required")
            stage(entries, args.cache, args.build_root)
    except (OSError, ValueError) as error:
        print(f"locked source contract failed: {error}", file=sys.stderr)
        return 1
    print(f"Locked product sources {args.command}: {len(entries)} verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
