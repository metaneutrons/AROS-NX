#!/usr/bin/env python3
"""Adapt the two reviewed Mesa 26 glapi generators to stdout-only builds.

The build engine installs stdout atomically. This adapter never writes to the
fetched Mesa tree and admits only the two exact products of the AROS recipe.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def fail(message: str) -> None:
    raise SystemExit(f"Mesa 26 glapi generator: {message}")


def exact_path(path: str, expected: Path, label: str) -> Path:
    candidate = Path(path).resolve()
    if candidate != expected.resolve():
        fail(f"unexpected {label}: {candidate}")
    return candidate


def main() -> None:
    if len(sys.argv) != 6:
        fail("expected source root, build root, script, output and mode")
    source_root = Path(sys.argv[1]).resolve(strict=True)
    build_root = Path(sys.argv[2]).resolve(strict=True)
    mode = sys.argv[5]
    shared = build_root / "src/mesa/glapi/shared-glapi"
    header = shared / "shared_glapi_mapi_tmp.h"
    wrappers = shared / "public_glapi_wrappers.c"
    local_root = Path(__file__).resolve().parent

    if mode == "header":
        script = exact_path(
            sys.argv[3],
            source_root / "src/mesa/glapi/mapi_abi.py",
            "header generator",
        )
        exact_path(sys.argv[4], header, "header output")
        command = [
            sys.executable,
            "-s",
            "-B",
            str(script),
            "--printer",
            "shared-glapi",
            "--gl_symbols",
            str(source_root / "src/glx/libgl-symbols.txt"),
            str(source_root / "src/mesa/glapi/glapi/gen/gl_and_es_API.xml"),
        ]
        result = subprocess.run(
            command, cwd=source_root, capture_output=True, check=False
        )
        if result.returncode != 0:
            sys.stderr.buffer.write(result.stderr)
            fail(f"upstream mapi_abi.py exited {result.returncode}")
        sys.stdout.buffer.write(result.stdout)
        return

    if mode == "wrappers":
        script = exact_path(
            sys.argv[3], local_root / "gen_public_glapi_wrappers.sh", "wrapper script"
        )
        exact_path(sys.argv[4], wrappers, "wrapper output")
        if not header.is_file():
            fail(f"required earlier output is absent: {header}")
        manifest = local_root / "public_glapi_required_symbols.txt"
        if not manifest.is_file():
            fail(f"required symbol manifest is absent: {manifest}")
        awk = shutil.which("awk")
        if awk is None:
            fail("POSIX awk is unavailable")
        with tempfile.TemporaryDirectory(prefix="mesa26-glapi-", dir=shared) as temp:
            output = Path(temp) / "public_glapi_wrappers.c"
            result = subprocess.run(
                ["/bin/sh", str(script), str(header), str(manifest), str(output)],
                cwd=source_root,
                env={**os.environ, "AWK": awk},
                capture_output=True,
                check=False,
            )
            if result.returncode != 0:
                sys.stderr.buffer.write(result.stderr)
                fail(f"wrapper generator exited {result.returncode}")
            sys.stdout.buffer.write(output.read_bytes())
        return

    fail(f"unsupported mode: {mode}")


if __name__ == "__main__":
    main()
