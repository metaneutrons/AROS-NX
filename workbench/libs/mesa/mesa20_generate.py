#!/usr/bin/env python3
"""Legacy entry point for the Mesa 20.0.8 generator lanes.

The adapter itself lives in mesa26_generate.py. This entry point only adds the
two modes that no Mesa 26 lane uses: python-outdir and the Mesa 20 v3dx
wrapper for V3D 3.3 and 4.1. It goes away together with the Mesa 20.0.8
inventories.
"""

from __future__ import annotations

from pathlib import Path
import sys

from mesa26_generate import fail, main, replace_once, run, v3dx_wrapper


def python_outdir(
    source_root: Path, generator: Path, output: Path, arguments: list[str], work: Path
) -> bytes:
    staged_dir = work / "out"
    staged_dir.mkdir()
    command_arguments = replace_once(arguments, "@OUTDIR@", str(staged_dir))
    run([sys.executable, "-B", str(generator), *command_arguments], source_root)
    staged = staged_dir / output.name
    if not staged.is_file():
        fail(f"Python generator did not create {staged.name}")
    return staged.read_bytes()


def mesa20_v3dx_wrapper(
    source_root: Path, generator: Path, output: Path, arguments: list[str], work: Path
) -> bytes:
    return v3dx_wrapper("v3dx-wrapper", {"33", "41"}, generator, arguments)


MESA20_MODES = {
    "python-outdir": python_outdir,
    "v3dx-wrapper": mesa20_v3dx_wrapper,
}


if __name__ == "__main__":
    main(MESA20_MODES)
