#!/usr/bin/env python3
"""Closed, source-immutable adapter for Mesa 20.0.8 generators.

The transpiler supplies only capability-audited modes and arguments. Every
named-file generator works in a private build-tree staging directory; fetched
source trees are never output locations.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"mesa20_generate.py: {message}")


def child_path(root: Path, raw: str, label: str, *, must_exist: bool) -> Path:
    path = Path(raw).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        fail(f"{label} escapes {root}: {path}")
    if path == root:
        fail(f"{label} must be a child of {root}")
    if must_exist and not path.is_file():
        fail(f"{label} is not a regular file: {path}")
    return path


def run(command: list[str], source_root: Path) -> bytes:
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    result = subprocess.run(
        command,
        cwd=source_root,
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        sys.stderr.buffer.write(result.stderr)
        fail(f"generator exited with status {result.returncode}: {command[0]}")
    return result.stdout


def replace_once(arguments: list[str], marker: str, replacement: str) -> list[str]:
    count = sum(argument.count(marker) for argument in arguments)
    if count != 1:
        fail(f"mode requires exactly one {marker} marker, found {count}")
    return [argument.replace(marker, replacement) for argument in arguments]


def safe_basename(raw: str, label: str) -> str:
    path = Path(raw)
    if not raw or path.name != raw or raw in {".", ".."}:
        fail(f"unsafe {label}: {raw!r}")
    return raw


def main() -> None:
    if len(sys.argv) < 6:
        fail(
            "usage: SOURCE_ROOT BUILD_ROOT GENERATOR_SCRIPT OUTPUT MODE "
            "[MODE_ARGUMENTS...]"
        )

    source_root = Path(sys.argv[1]).resolve()
    build_root = Path(sys.argv[2]).resolve()
    if not source_root.is_dir():
        fail(f"source root is not a directory: {source_root}")
    build_root.mkdir(parents=True, exist_ok=True)
    if not build_root.is_dir():
        fail(f"build root is not a directory: {build_root}")

    generator = child_path(
        source_root, sys.argv[3], "generator input", must_exist=True
    )
    output = child_path(build_root, sys.argv[4], "generator output", must_exist=False)
    mode = sys.argv[5]
    arguments = sys.argv[6:]

    work_parent = build_root / ".generator-work"
    work_parent.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(str(output).encode("utf-8")).hexdigest()[:16]
    with tempfile.TemporaryDirectory(prefix=f"{key}-", dir=work_parent) as raw_work:
        work = Path(raw_work)

        if mode == "python-stdout":
            if any(marker in argument for marker in ("@OUTPUT@", "@OUTDIR@") for argument in arguments):
                fail("python-stdout arguments contain an output marker")
            data = run([sys.executable, "-B", str(generator), *arguments], source_root)

        elif mode == "python-output":
            staged = work / output.name
            command_arguments = replace_once(arguments, "@OUTPUT@", str(staged))
            run([sys.executable, "-B", str(generator), *command_arguments], source_root)
            if not staged.is_file():
                fail(f"Python generator did not create {staged.name}")
            data = staged.read_bytes()

        elif mode == "python-outdir":
            staged_dir = work / "out"
            staged_dir.mkdir()
            command_arguments = replace_once(arguments, "@OUTDIR@", str(staged_dir))
            run([sys.executable, "-B", str(generator), *command_arguments], source_root)
            staged = staged_dir / output.name
            if not staged.is_file():
                fail(f"Python generator did not create {staged.name}")
            data = staged.read_bytes()

        elif mode == "flex":
            executable = os.environ.get("AROS_FLEX_EXECUTABLE", "")
            if not executable or not Path(executable).is_file():
                fail("AROS_FLEX_EXECUTABLE is missing or not a regular file")
            if any(argument.startswith("-o") or argument == "--stdout" for argument in arguments):
                fail("flex output options are owned by the adapter")
            data = run([executable, *arguments, "--stdout", str(generator)], source_root)

        elif mode == "bison":
            if len(arguments) < 3:
                fail("bison mode requires SOURCE_NAME HEADER_NAME PREFIX")
            source_name = safe_basename(arguments[0], "Bison source name")
            header_name = safe_basename(arguments[1], "Bison header name")
            prefix = arguments[2]
            if not prefix or any(character.isspace() for character in prefix):
                fail(f"unsafe Bison prefix: {prefix!r}")
            extra = arguments[3:]
            if any(
                argument in {"-o", "--output", "-d", "--defines", "-l", "--no-lines"}
                or argument.startswith(("--output=", "--defines="))
                for argument in extra
            ):
                fail("Bison output and line controls are owned by the adapter")
            staged_source = work / source_name
            staged_header = work / header_name
            executable = os.environ.get("AROS_BISON_EXECUTABLE", "")
            if not executable or not Path(executable).is_file():
                fail("AROS_BISON_EXECUTABLE is missing or not a regular file")
            run(
                [
                    executable,
                    "-d",
                    "-l",
                    "-p",
                    prefix,
                    f"--defines={staged_header}",
                    "-o",
                    str(staged_source),
                    *extra,
                    str(generator),
                ],
                source_root,
            )
            if output.name == source_name:
                staged = staged_source
            elif output.name == header_name:
                staged = staged_header
            else:
                fail(
                    f"Bison output {output.name} is neither {source_name} nor {header_name}"
                )
            if not staged.is_file():
                fail(f"Bison did not create {staged.name}")
            data = staged.read_bytes()

        elif mode == "mesa-git-sha1":
            if arguments:
                fail("mesa-git-sha1 mode accepts no arguments")
            data = b'#define MESA_GIT_SHA1 ""\n'

        elif mode == "v3dx-wrapper":
            if len(arguments) != 1 or arguments[0] not in {"33", "41"}:
                fail("v3dx-wrapper mode requires exactly version 33 or 41")
            if not generator.name.startswith("v3dx_") or generator.suffix != ".c":
                fail(f"v3dx-wrapper input is not a v3dx C source: {generator.name}")
            version = arguments[0]
            data = (
                f'#define V3D_VERSION {version}\n#include "{generator.name}"\n'
            ).encode("ascii")

        else:
            fail(f"unsupported generator mode: {mode!r}")

    if not data:
        fail(f"generator produced an empty result for {output.name}")
    sys.stdout.buffer.write(data)


if __name__ == "__main__":
    main()
