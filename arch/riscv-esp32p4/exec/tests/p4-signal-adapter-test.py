#!/usr/bin/env python3
"""Extract selected production P4 signal-adapter bodies into a host fixture."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[4]
EXEC = ROOT / "arch/riscv-esp32p4/exec"
ADAPTER = EXEC / "p4_signal_adapter.c"
QUEUE = ROOT / "arch/riscv-esp32p4/kernel/p4_scheduler_queue.c"
PROTOCOL = EXEC / "p4_signal_protocol.c"
FIXTURE = EXEC / "tests/p4-signal-adapter-test.c"


def extract(source: str, start: str, end: str) -> str:
    if source.count(start) != 1 or source.count(end) != 1:
        raise RuntimeError(f"expected unique extraction markers: {start!r}, {end!r}")
    begin = source.index(start)
    finish = source.index(end, begin)
    if finish <= begin:
        raise RuntimeError(f"reversed extraction markers: {start!r}, {end!r}")
    return source[begin:finish]


def extract_function(source: str, start: str) -> str:
    """Extract one C function without depending on the following function."""
    if source.count(start) != 1:
        raise RuntimeError(f"expected unique function marker: {start!r}")
    begin = source.index(start)
    opening = source.index("{", begin)
    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "*":
                state = "block_comment"
                index += 2
                continue
            if char == "/" and following == "/":
                state = "line_comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[begin:index + 1]
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "string" or state == "char":
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "char" and char == "'"
            ):
                state = "code"
        index += 1
    raise RuntimeError(f"unterminated function: {start!r}")


def main() -> int:
    adapter = ADAPTER.read_text()
    queue = QUEUE.read_text()
    fixture = FIXTURE.read_text()

    replacements = {
        "/* EXTRACTED_ADAPTER_LOAD */": (
            extract(
                adapter,
                "static struct p4_signal_effect p4_signal_adapter_error(",
                "void p4_signal_adapter_lock_disabled(",
            )
            + extract(
                adapter,
                "static enum p4_signal_owner_state p4_signal_adapter_owner(",
                "int p4_signal_adapter_wait_arm_locked(",
            )
        ),
        "/* EXTRACTED_ELIGIBLE_HART */": extract_function(
            queue, "int krnP4SignalEligibleHartLocked("
        ),
        "/* EXTRACTED_ADAPTER_RAISE */": extract(
            adapter,
            "struct p4_signal_effect p4_signal_adapter_raise_locked(",
            "struct p4_signal_effect p4_signal_adapter_set_locked(",
        ),
    }
    for marker, body in replacements.items():
        if fixture.count(marker) != 1:
            raise RuntimeError(f"expected one fixture marker: {marker}")
        fixture = fixture.replace(marker, body)

    with tempfile.TemporaryDirectory(prefix="p4-signal-adapter-") as temp_dir:
        temp = Path(temp_dir)
        generated = temp / "p4-signal-adapter-extracted.c"
        binary = temp / "p4-signal-adapter-test"
        generated.write_text(fixture)
        command = [
            "clang",
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-I",
            str(EXEC),
            str(PROTOCOL),
            str(generated),
            "-o",
            str(binary),
        ]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
