#!/usr/bin/env python3
"""Compile focused host tests from the pinned ESP-Hosted allocator source."""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SIGNATURE = "static inline void *hosted_reference_dma_alloc(size_t size)"


def extract_function(header: Path) -> str:
    source = header.read_text(encoding="utf-8")
    start = source.find(SIGNATURE)
    if start < 0:
        raise ValueError(f"allocator signature not found in {header}")

    opening = source.find("{", start + len(SIGNATURE))
    if opening < 0:
        raise ValueError(f"allocator body not found in {header}")

    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(source):
        char = source[i]
        following = source[i + 1] if i + 1 < len(source) else ""

        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                i += 1
        elif state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        elif char == "/" and following == "/":
            state = "line-comment"
            i += 1
        elif char == "/" and following == "*":
            state = "block-comment"
            i += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1

    raise ValueError(f"unterminated allocator body in {header}")


def make_test_source(function_source: str) -> str:
    return f'''#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MALLOC_CAP_DMA      UINT32_C(0x01)
#define MALLOC_CAP_INTERNAL UINT32_C(0x02)
#define MALLOC_CAP_8BIT     UINT32_C(0x04)

static size_t allocator_calls;
static size_t observed_alignment;
static size_t observed_size;
static uint32_t observed_caps;
static void *allocator_result;
static unsigned checks;
static unsigned cases;
static int failures;
static int return_token;

void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{{
    ++allocator_calls;
    observed_alignment = alignment;
    observed_size = size;
    observed_caps = caps;
    return allocator_result;
}}

{function_source}

static void check(int condition, const char *label)
{{
    ++checks;
    if (!condition) {{
        ++failures;
        fprintf(stderr, "FAIL: %s\\n", label);
    }}
}}

static void reset_allocator(void *result)
{{
    allocator_calls = 0;
    observed_alignment = 0;
    observed_size = 0;
    observed_caps = 0;
    allocator_result = result;
}}

static void test_allocation(const char *name, size_t request, size_t rounded)
{{
    void *result;
    ++cases;
    reset_allocator(&return_token);
    result = hosted_reference_dma_alloc(request);
    check(result == &return_token, name);
    check(allocator_calls == 1, "valid request calls heap allocator once");
    check(observed_alignment == 64, "heap allocator receives 64-byte alignment");
    check(observed_size == rounded, "heap allocator receives rounded size");
    check(observed_caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                            MALLOC_CAP_8BIT),
          "heap allocator receives exactly DMA|INTERNAL|8BIT caps");
}}

static void test_rejected_size(const char *name, size_t request)
{{
    void *result;
    ++cases;
    reset_allocator(&return_token);
    result = hosted_reference_dma_alloc(request);
    check(result == NULL, name);
    check(allocator_calls == 0, "rejected request does not call heap allocator");
}}

static void test_allocator_failure(void)
{{
    void *result;
    ++cases;
    reset_allocator(NULL);
    result = hosted_reference_dma_alloc(65);
    check(result == NULL, "heap allocation failure is returned as NULL");
    check(allocator_calls == 1, "allocation failure still calls heap allocator once");
    check(observed_alignment == 64, "failure path keeps 64-byte alignment");
    check(observed_size == 128, "failure path keeps rounded size");
    check(observed_caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL |
                            MALLOC_CAP_8BIT),
          "failure path keeps exact DMA|INTERNAL|8BIT caps");
}}

int main(void)
{{
    test_allocation("size 1 forwards allocator result", 1, 64);
    test_allocation("size 63 forwards allocator result", 63, 64);
    test_allocation("size 64 forwards allocator result", 64, 64);
    test_allocation("size 65 forwards allocator result", 65, 128);
    test_allocation("size 1536 forwards allocator result", 1536, 1536);
    test_allocation("largest roundable size forwards allocator result",
                    SIZE_MAX - (size_t)63, SIZE_MAX - (size_t)63);
    test_rejected_size("zero size is rejected", 0);
    test_rejected_size("SIZE_MAX overflow is rejected", SIZE_MAX);
    test_rejected_size("first unroundable size is rejected",
                       SIZE_MAX - (size_t)62);
    test_allocator_failure();

    if (failures != 0) {{
        fprintf(stderr, "FAIL: %d of %u checks failed across %u cases\\n",
                failures, checks, cases);
        return 1;
    }}
    printf("PASS: %u cases, %u checks\\n", cases, checks);
    return 0;
}}
'''


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract and test hosted_reference_dma_alloc from os_wrapper.h"
    )
    parser.add_argument("header", type=Path, help="path to pinned host/port/os_wrapper.h")
    args = parser.parse_args()

    compiler_words = shlex.split(os.environ.get("CC", "cc"))
    if not compiler_words:
        parser.error("CC must name a C compiler")
    compiler = shutil.which(compiler_words[0])
    if compiler is None:
        parser.error(f"C compiler not found: {compiler_words[0]}")
    compiler_words[0] = compiler

    try:
        function_source = extract_function(args.header)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="hosted-allocator-test-") as tmp:
        source_path = Path(tmp) / "test_allocator.c"
        source_path.write_text(make_test_source(function_source), encoding="utf-8")

        for optimization in ("-O0", "-O2"):
            binary_path = Path(tmp) / f"test_allocator_{optimization[2:]}"
            command = [
                *compiler_words,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                optimization,
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                str(source_path),
                "-o",
                str(binary_path),
            ]
            print(f"{optimization}: compiling with ASan and UBSan")
            built = subprocess.run(command, text=True, capture_output=True)
            if built.stdout:
                print(built.stdout, end="")
            if built.stderr:
                print(built.stderr, end="", file=sys.stderr)
            if built.returncode != 0:
                print(f"{optimization}: compiler exited {built.returncode}", file=sys.stderr)
                return built.returncode

            run = subprocess.run([str(binary_path)], text=True, capture_output=True)
            if run.stdout:
                print(f"{optimization}: {run.stdout}", end="")
            if run.stderr:
                print(run.stderr, end="", file=sys.stderr)
            if run.returncode != 0:
                print(f"{optimization}: test executable exited {run.returncode}",
                      file=sys.stderr)
                return run.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
