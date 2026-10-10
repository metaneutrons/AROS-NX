#!/usr/bin/env python3
"""Compile the extracted production SDMMC DMA engine against host models."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
from runpy import run_path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_dma_engine.c"
DMA_SOURCE = PORT / "sdmmc" / "dma.c"
SDMMC = PORT / "sdmmc"
HOST_HEADER = SDMMC / "host.h"
CACHEOPS_HEADER = ROOT / "compiler" / "include" / "aros" / "cacheops.h"
KERNEL_HEADER = ROOT / "compiler" / "include" / "aros" / "kernel.h"
TAGITEM_HEADER = ROOT / "compiler" / "include" / "utility" / "tagitem.h"
INSERT_SOURCE = "/* INSERT_ACTUAL_DMA_SOURCE */"
INSERT_BUDGET_HELPER = "/* INSERT_ACTUAL_BUDGET_HELPER */"
INSERT_CACHEOPS = "/* INSERT_ACTUAL_CACHEOPS_CONSTANTS */"
CLOCK_EXTRACTION = run_path(str(PORT / "tools" / "test-clock-engine.py"))
extract_constant = CLOCK_EXTRACTION["extract_constant"]
extract_definition = CLOCK_EXTRACTION["extract_definition"]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_declaration(source: str, label: str, signature: str) -> str:
    matches = list(re.finditer(signature, source, flags=re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected one {label} declaration, found {len(matches)}")
    start = matches[0].start()
    end = source.find(";", matches[0].end())
    if end < 0:
        raise ValueError(f"unterminated {label} declaration")
    return source[start : end + 1]


def translate_host_barrier(body: str, label: str) -> str:
    instruction = '__asm__ volatile("fence rw, rw" ::: "memory");'
    if body.count(instruction) != 1:
        raise ValueError(f"expected one RISC-V fence in {label}")
    return body.replace(instruction, "modeled_memory_barrier();")


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    fixture = FIXTURE.read_text(encoding="utf-8")
    if fixture.count(INSERT_SOURCE) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_SOURCE}")
    if fixture.count(INSERT_BUDGET_HELPER) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_BUDGET_HELPER}")
    if fixture.count(INSERT_CACHEOPS) != 1:
        raise ValueError(f"fixture must contain exactly one {INSERT_CACHEOPS}")

    source = DMA_SOURCE.read_text(encoding="utf-8")
    host_header = HOST_HEADER.read_text(encoding="utf-8")
    cacheops_source = CACHEOPS_HEADER.read_text(encoding="utf-8")
    kernel_source = KERNEL_HEADER.read_text(encoding="utf-8")

    constants = [
        extract_constant(TAGITEM_HEADER.read_bytes().decode("latin1"), "TAG_USER"),
        extract_constant(kernel_source, "KATTR_CPULoad"),
        extract_constant(kernel_source, "KATTR_CPULoad_END"),
        extract_constant(kernel_source, "KATTR_CacheOps"),
        extract_constant(cacheops_source, "KRN_CACHE_OPS_VERSION"),
    ]
    data_declarations = [
        (
            "descriptor array",
            extract_declaration(
                source,
                "descriptor array",
                r"^static\s+struct\s+P4HostDMADesc\s+descriptors\s*\[",
            ),
        ),
        (
            "descriptor size assertion",
            extract_declaration(
                source,
                "descriptor size assertion",
                r"^typedef\s+char\s+p4host_dma_descriptor_must_be_64_bytes\s*\[",
            ),
        ),
        (
            "permanent bounce buffer",
            extract_declaration(
                source,
                "permanent bounce buffer",
                r"^static\s+UBYTE\s+bounce\s*\[",
            ),
        ),
        (
            "cache operation pointer",
            extract_declaration(
                source,
                "cache operation pointer",
                r"^static\s+const\s+struct\s+KrnCacheOps\s*\*cache_ops\s*",
            ),
        ),
    ]

    function_specs = (
        ("dma_owned", r"\bstatic\s+BOOL\s+dma_owned\s*\("),
        ("dma_wait", r"\bstatic\s+BOOL\s+dma_wait\s*\("),
        ("dma_settle", r"\bstatic\s+BOOL\s+dma_settle\s*\("),
        ("dma_writeback", r"\bstatic\s+void\s+dma_writeback\s*\("),
        ("dma_invalidate", r"\bstatic\s+void\s+dma_invalidate\s*\("),
        ("dma_cache_available", r"\bstatic\s+BOOL\s+dma_cache_available\s*\("),
        ("p4host_dma_init", r"\bvoid\s+p4host_dma_init\s*\("),
        ("p4host_fifo_reset", r"\bBOOL\s+p4host_fifo_reset\s*\("),
        ("p4host_dma_reset", r"\bBOOL\s+p4host_dma_reset\s*\("),
        ("p4host_dma_prepare", r"\bBOOL\s+p4host_dma_prepare\s*\("),
        ("p4host_dma_ready", r"\bBOOL\s+p4host_dma_ready\s*\("),
        ("p4host_dma_complete", r"\bBOOL\s+p4host_dma_complete\s*\("),
        ("p4host_dma_inspect", r"\bvoid\s+p4host_dma_inspect\s*\("),
    )
    memory_source = (SDMMC / "memory.h").read_text(encoding="utf-8")
    definitions = [
        (name, extract_definition(memory_source, name,
                                 rf"\bstatic\s+inline\s+void\s+{name}\s*\("))
        for name in ("p4host_zero", "p4host_move")
    ] + [
        (name, extract_definition(source, name, signature))
        for name, signature in function_specs
    ]
    budget_helper = extract_definition(
        host_header,
        "p4host_budget_ok",
        r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\(",
    )
    for index, (name, body) in enumerate(definitions):
        if name in ("dma_writeback", "dma_invalidate"):
            definitions[index] = (name, translate_host_barrier(body, name))

    low_level = [
        (
            "dma_read source (replaced by MMIO model)",
            extract_definition(source, "dma_read", r"\bstatic\s+ULONG\s+dma_read\s*\("),
        ),
        (
            "dma_write source (replaced by MMIO model)",
            extract_definition(source, "dma_write", r"\bstatic\s+void\s+dma_write\s*\("),
        ),
    ]
    inserted = "\n\n".join(
        [body for _, body in data_declarations] +
        [body for _, body in definitions]
    )
    assembled = fixture.replace(INSERT_CACHEOPS, "\n".join(constants))
    assembled = assembled.replace(INSERT_BUDGET_HELPER, budget_helper)
    assembled = assembled.replace(INSERT_SOURCE, inserted)

    hashes: list[tuple[str, bytes]] = [
        (name, body.encode("utf-8")) for name, body in data_declarations
    ]
    hashes.extend((name, body.encode("utf-8")) for name, body in definitions)
    hashes.extend((name, body.encode("utf-8")) for name, body in low_level)
    hashes.append(("actual p4host_budget_ok", budget_helper.encode("utf-8")))
    hashes.extend(
        (f"production constant: {name}", value.encode("utf-8"))
        for name, value in zip(
            ("TAG_USER", "KATTR_CPULoad", "KATTR_CPULoad_END",
             "KATTR_CacheOps", "cache ops version"), constants
        )
    )
    hashes.append(("actual dma.h", (SDMMC / "dma.h").read_bytes()))
    hashes.append(("actual host.h", HOST_HEADER.read_bytes()))
    hashes.append(("actual registers.h", (SDMMC / "registers.h").read_bytes()))
    hashes.append(("assembled fixture", assembled.encode("utf-8")))
    return assembled, hashes


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble_fixture()
    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {sha256(content)}")
    print(f"  test fixture: {sha256(FIXTURE.read_bytes())}")
    print(
        "Host-only modeled MMIO and kernel cache lookup; production poll, "
        "timeout and settle limits are retained. This is not hardware evidence."
    )

    with tempfile.TemporaryDirectory(prefix="p4-dma-engine-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_dma_engine.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_dma_engine_O{optimization}"
            command = compiler + [
                "-std=c99",
                f"-O{optimization}",
                "-g",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Wconversion",
                "-Wshadow",
                "-Wstrict-prototypes",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I",
                str(SDMMC),
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict C99 warnings")
            build = subprocess.run(
                command,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                timeout=60,
            )
            if build.returncode != 0:
                sys.stderr.write(build.stdout)
                sys.stderr.write(build.stderr)
                return build.returncode or 1
            if build.stdout:
                sys.stdout.write(build.stdout)
            if build.stderr:
                sys.stderr.write(build.stderr)

            print(f"Running O{optimization} (30-second timeout)")
            try:
                result = subprocess.run(
                    [str(binary)],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=30,
                    env=environment,
                )
            except subprocess.TimeoutExpired:
                print(
                    "FAIL: DMA fixture exceeded 30 seconds; a bounded wait "
                    "may have stalled",
                    file=sys.stderr,
                )
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: fixture did not report its complete case count", file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
