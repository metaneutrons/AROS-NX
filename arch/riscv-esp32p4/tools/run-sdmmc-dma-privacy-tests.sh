#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 - "$SCRIPT_DIR" <<'PY'
from __future__ import annotations

import hashlib
import os
from pathlib import Path
from runpy import run_path
import re
import shlex
import subprocess
import sys
import tempfile


TOOLS = Path(sys.argv[1]).resolve()
PORT = TOOLS.parent
ROOT = PORT.parent.parent
SDMMC = PORT / "sdmmc"
INCLUDE = ROOT / "compiler" / "include"
FIXTURE = TOOLS / "test-sdmmc-dma-privacy.c"
DMA_SOURCE = SDMMC / "dma.c"
COMMAND_SOURCE = SDMMC / "command.c"
CLOCK_SOURCE = SDMMC / "clock.c"
DMA_HEADER = SDMMC / "dma.h"
REGISTERS_HEADER = SDMMC / "registers.h"
HOST_HEADER = SDMMC / "host.h"
MEMORY_HEADER = SDMMC / "memory.h"
INSERT_CONSTANTS = "/* INSERT_ACTUAL_PLATFORM_CONSTANTS */"
INSERT_BUDGET = "/* INSERT_ACTUAL_BUDGET_HELPER */"
INSERT_SOURCE = "/* INSERT_ACTUAL_SOURCE */"
INSERT_HOST_CONSTANTS = "/* INSERT_HOST_ULONG_CONSTANTS */"
EXTRACTION = run_path(str(TOOLS / "test-clock-engine.py"))
extract_constant = EXTRACTION["extract_constant"]
extract_definition = EXTRACTION["extract_definition"]


def digest(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def replace_once(body: str, label: str, old: str, new: str) -> str:
    if body.count(old) != 1:
        raise ValueError(f"expected one source substitution in {label}")
    return body.replace(old, new)


def extract_constant_lines(source: str, label: str,
                           names: tuple[str, ...]) -> str:
    lines: list[str] = []
    for name in names:
        pattern = re.compile(
            rf"(?m)^[ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b[^\n]*"
        )
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise ValueError(
                f"expected one {name} definition in {label}, found {len(matches)}"
            )
        lines.append(matches[0].group(0))
    return "\n".join(lines)


def materialize_ulong_masks(source: str, names: tuple[str, ...]) -> str:
    """Type selected target constants as ULONG in this LP64 host fixture."""
    materialized: list[str] = [
        "/* Host-only target-width typing; IPTR remains native uintptr_t. */"
    ]
    for name in names:
        pattern = re.compile(
            rf"(?m)^([ \t]*#[ \t]*define[ \t]+{re.escape(name)}\b[ \t]+)([^\r\n]+)$"
        )
        matches = list(pattern.finditer(source))
        if len(matches) != 1:
            raise ValueError(
                f"expected one single-line {name} definition to materialize, "
                f"found {len(matches)}"
            )
        expression = matches[0].group(2).strip()
        if not expression or "/*" in expression or "//" in expression:
            raise ValueError(f"cannot safely materialize {name}: {expression!r}")
        materialized.extend((
            f"#undef {name}",
            f"#define {name} ((ULONG)({expression}))",
        ))
    return "\n".join(materialized)


def translate_fence(body: str, label: str, instruction: str) -> str:
    if body.count(instruction) != 1:
        raise ValueError(f"expected one RISC-V fence in {label}")
    return body.replace(instruction, "modeled_memory_barrier();")


def assemble() -> tuple[str, list[tuple[str, bytes]]]:
    fixture = FIXTURE.read_text(encoding="utf-8")
    for marker in (INSERT_CONSTANTS, INSERT_BUDGET, INSERT_SOURCE,
                   INSERT_HOST_CONSTANTS):
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    dma = DMA_SOURCE.read_text(encoding="utf-8")
    command = COMMAND_SOURCE.read_text(encoding="utf-8")
    clock = CLOCK_SOURCE.read_text(encoding="utf-8")
    host = HOST_HEADER.read_text(encoding="utf-8")
    memory = MEMORY_HEADER.read_text(encoding="utf-8")
    dma_header = DMA_HEADER.read_text(encoding="utf-8")
    registers_header = REGISTERS_HEADER.read_text(encoding="utf-8")
    kernel_header = (INCLUDE / "aros" / "kernel.h").read_text(encoding="utf-8")
    cache_header = (INCLUDE / "aros" / "cacheops.h").read_text(encoding="utf-8")
    tag_header = (INCLUDE / "utility" / "tagitem.h").read_bytes().decode("latin1")

    constants = [
        extract_constant(tag_header, "TAG_USER"),
        extract_constant(kernel_header, "KATTR_CPULoad"),
        extract_constant(kernel_header, "KATTR_CPULoad_END"),
        extract_constant(kernel_header, "KATTR_CacheOps"),
        extract_constant(cache_header, "KRN_CACHE_OPS_VERSION"),
        extract_constant(clock, "P4HOST_CLOCK_TIMEOUT_US"),
        extract_constant(clock, "P4HOST_POLL_BOUND"),
    ]
    dma_constant_names = (
        "P4HOST_DMA_MAX", "P4HOST_DMA_SEGMENT", "P4HOST_DMA_COUNT",
        "P4HOST_DESC_LAST", "P4HOST_DESC_FIRST", "P4HOST_DESC_CHAIN",
        "P4HOST_DESC_OWN",
    )
    register_constant_names = (
        "P4_HPPERIPH0_BASE", "P4_HPPERIPH1_BASE", "P4_LPAON_BASE",
        "P4_SDMMC_BASE", "P4_HP_SYS_CLKRST_BASE", "P4_LP_CLKRST_BASE",
        "P4SD_CTRL_RESET_ALL", "P4SD_CTRL_FIFO_RESET",
        "P4SD_CTRL_DMA_RESET", "P4SD_CTRL_DMA_ENABLE",
        "P4SD_CTRL_USE_INTERNAL_DMA", "P4SD_CTYPE_4BIT_SLOT0",
        "P4SD_CTYPE_8BIT_SLOT0", "P4SD_CMD_RESP", "P4SD_CMD_RESP_LONG",
        "P4SD_CMD_RESP_CRC", "P4SD_CMD_DATA", "P4SD_CMD_WRITE",
        "P4SD_CMD_SEND_AUTO_STOP", "P4SD_CMD_STOP_ABORT",
        "P4SD_CMD_WAIT_DATA", "P4SD_CMD_SEND_INIT", "P4SD_CMD_UPDATE_CLK",
        "P4SD_CMD_USE_HOLD", "P4SD_CMD_START", "P4SD_INT_CMD_DATA_MASK",
        "P4SD_INT_RESP_ERR", "P4SD_INT_CMD_DONE", "P4SD_INT_DATA_OVER",
        "P4SD_INT_RXDR", "P4SD_INT_RCRC", "P4SD_INT_DCRC", "P4SD_INT_RTO",
        "P4SD_INT_DTO", "P4SD_INT_HTO", "P4SD_INT_FRUN", "P4SD_INT_HLE",
        "P4SD_INT_SBE", "P4SD_INT_EBE", "P4SD_STATUS_FIFO_EMPTY",
        "P4SD_STATUS_FIFO_FULL", "P4SD_STATUS_DATA_BUSY",
        "P4SD_STATUS_FIFO_COUNT_M", "P4SD_FIFOTH_RX_WMARK",
        "P4SD_FIFOTH_DMA_MSIZE_8", "P4SD_BMOD_SWR", "P4SD_BMOD_FB",
        "P4SD_BMOD_DE", "P4SD_BMOD_PBL_8", "P4SD_IDSTS_TI",
        "P4SD_IDSTS_RI", "P4SD_IDSTS_FBE", "P4SD_IDSTS_DU",
        "P4SD_IDSTS_CES", "P4SD_IDSTS_NI", "P4SD_IDINTEN_TI",
        "P4SD_IDINTEN_RI", "P4SD_IDINTEN_NI", "P4_HP_SDMMC_CLK_EN",
        "P4_HP_REF_160M_CLK_EN", "P4_HP_SDIO_HS_MODE",
        "P4_HP_SDIO_CLK_SRC", "P4_HP_SDIO_CLK_EN", "P4_HP_SDIO_UPDATE",
        "P4_HP_SDIO_SLF_EN", "P4_HP_SDIO_DRV_EN", "P4_HP_SDIO_SAM_EN",
        "P4_HP_SDIO_FIELDS_M", "P4_LP_SDMMC_RST_EN",
    )
    original_dma_constants = extract_constant_lines(
        dma_header, "dma.h", dma_constant_names
    )
    original_register_constants = extract_constant_lines(
        registers_header, "registers.h", register_constant_names
    )
    original_host_constants = "\n".join((
        original_dma_constants, original_register_constants
    ))
    host_constants = "\n".join((
        materialize_ulong_masks(original_dma_constants, dma_constant_names),
        materialize_ulong_masks(original_register_constants,
                                register_constant_names),
    ))
    budget = extract_definition(
        host, "p4host_budget_ok", r"\bstatic\s+inline\s+BOOL\s+p4host_budget_ok\s*\("
    )

    # The DMA private state and descriptor/static buffer are part of the actual
    # source under test; extract their single declarations textually.
    def declaration(source: str, label: str, signature: str) -> str:
        matches = list(re.finditer(signature, source, flags=re.MULTILINE))
        if len(matches) != 1:
            raise ValueError(f"expected one {label} declaration, found {len(matches)}")
        end = source.find(";", matches[0].end())
        if end < 0:
            raise ValueError(f"unterminated {label} declaration")
        return source[matches[0].start():end + 1]

    data_declarations = [
        declaration(dma, "DMA descriptor array",
                    r"^static\s+struct\s+P4HostDMADesc\s+descriptors\s*\["),
        declaration(dma, "DMA descriptor size assertion",
                    r"^typedef\s+char\s+p4host_dma_descriptor_must_be_64_bytes\s*\["),
        declaration(dma, "permanent DMA bounce buffer",
                    r"^static\s+UBYTE\s+bounce\s*\["),
        declaration(dma, "cache operation pointer",
                    r"^static\s+const\s+struct\s+KrnCacheOps\s*\*cache_ops\s*"),
        declaration(dma, "sticky TX residue extent",
                    r"^static\s+ULONG\s+tx_residue\s*"),
    ]

    def body(source: str, name: str, signature: str) -> str:
        return extract_definition(source, name, signature)

    memory_helpers = [
        body(memory, name, rf"\bstatic\s+inline\s+void\s+{name}\s*\(")
        for name in ("p4host_zero", "p4host_move")
    ]

    dma_read = body(dma, "dma_read", r"\bstatic\s+ULONG\s+dma_read\s*\(")
    dma_read = replace_once(
        dma_read, "dma_read",
        "return *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset);",
        "return model_sdmmc_read(offset);",
    )
    dma_write = body(dma, "dma_write", r"\bstatic\s+void\s+dma_write\s*\(")
    dma_write = replace_once(
        dma_write, "dma_write",
        "*(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset) = value;",
        "model_sdmmc_write(offset, value);",
    )
    dma_write = translate_fence(
        dma_write, "dma_write", '__asm__ volatile("fence iorw, iorw" ::: "memory");'
    )

    command_read = body(
        command, "command_read", r"\bstatic\s+ULONG\s+command_read\s*\("
    )
    command_read = replace_once(
        command_read, "command_read",
        "return *(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset);",
        "return model_sdmmc_read(offset);",
    )
    command_write = body(
        command, "command_write", r"\bstatic\s+void\s+command_write\s*\("
    )
    command_write = replace_once(
        command_write, "command_write",
        "*(volatile ULONG *)(IPTR)(P4_SDMMC_BASE + offset) = value;",
        "model_sdmmc_write(offset, value);",
    )
    command_write = translate_fence(
        command_write, "command_write",
        '__asm__ volatile("fence iorw, iorw" ::: "memory");'
    )

    clock_read = body(clock, "clock_read", r"\bstatic\s+ULONG\s+clock_read\s*\(")
    clock_read = replace_once(
        clock_read, "clock_read", "return *(volatile ULONG *)address;",
        "return model_clock_read(address);",
    )
    clock_write = body(clock, "clock_write", r"\bstatic\s+void\s+clock_write\s*\(")
    clock_write = replace_once(
        clock_write, "clock_write", "*(volatile ULONG *)address = value;",
        "model_clock_write(address, value);",
    )
    clock_write = translate_fence(
        clock_write, "clock_write",
        '__asm__ volatile("fence iorw, iorw" ::: "memory");'
    )

    function_specs = (
        (dma, "dma_owned", r"\bstatic\s+BOOL\s+dma_owned\s*\("),
        (dma, "dma_wait", r"\bstatic\s+BOOL\s+dma_wait\s*\("),
        (dma, "dma_settle", r"\bstatic\s+BOOL\s+dma_settle\s*\("),
        (dma, "dma_writeback", r"\bstatic\s+void\s+dma_writeback\s*\("),
        (dma, "dma_invalidate", r"\bstatic\s+void\s+dma_invalidate\s*\("),
        (dma, "dma_cache_available", r"\bstatic\s+BOOL\s+dma_cache_available\s*\("),
        (dma, "p4host_dma_init", r"\bvoid\s+p4host_dma_init\s*\("),
        (dma, "p4host_dma_scrub_idle", r"\bBOOL\s+p4host_dma_scrub_idle\s*\("),
        (dma, "p4host_fifo_reset", r"\bBOOL\s+p4host_fifo_reset\s*\("),
        (dma, "p4host_dma_reset", r"\bBOOL\s+p4host_dma_reset\s*\("),
        (dma, "p4host_dma_prepare", r"\bBOOL\s+p4host_dma_prepare\s*\("),
        (dma, "p4host_dma_ready", r"\bBOOL\s+p4host_dma_ready\s*\("),
        (dma, "p4host_dma_complete", r"\bBOOL\s+p4host_dma_complete\s*\("),
        (command, "command_wait", r"\bstatic\s+BOOL\s+command_wait\s*\("),
        (command, "command_valid", r"\bstatic\s+BOOL\s+command_valid\s*\("),
        (command, "p4host_command", r"\bBOOL\s+p4host_command\s*\("),
        (clock, "clock_owned", r"\bstatic\s+BOOL\s+clock_owned\s*\("),
        (clock, "clock_wait_clear", r"\bstatic\s+BOOL\s+clock_wait_clear\s*\("),
        (clock, "clock_delay", r"\bstatic\s+BOOL\s+clock_delay\s*\("),
        (clock, "clock_cleanup_settle", r"\bstatic\s+BOOL\s+clock_cleanup_settle\s*\("),
        (clock, "clock_source", r"\bstatic\s+BOOL\s+clock_source\s*\("),
        (clock, "clock_fail", r"\bstatic\s+void\s+clock_fail\s*\("),
        (clock, "p4host_reset", r"\bBOOL\s+p4host_reset\s*\("),
    )
    definitions = memory_helpers + [body(source, name, signature)
                                    for source, name, signature in function_specs]
    original_clock_fail = definitions[-2]
    original_p4host_reset = definitions[-1]
    definitions[-2] = replace_once(
        definitions[-2], "clock_fail", "value & ~3UL",
        "value & ~((ULONG)3UL)",
    )
    definitions[-1] = replace_once(
        definitions[-1], "p4host_reset", "value & ~3UL",
        "value & ~((ULONG)3UL)",
    )
    fence = '__asm__ volatile("fence rw, rw" ::: "memory");'
    for index, (source, name, signature) in enumerate(function_specs, start=2):
        if name in ("dma_writeback", "dma_invalidate"):
            definitions[index] = translate_fence(
                definitions[index],
                "DMA cache helper",
                fence,
            )

    source_bodies = data_declarations + [
        clock_read, clock_write, dma_read, dma_write,
        command_read, command_write,
    ] + definitions
    assembled = fixture.replace(INSERT_CONSTANTS, "\n".join(constants))
    assembled = assembled.replace(INSERT_HOST_CONSTANTS, host_constants)
    assembled = assembled.replace(INSERT_BUDGET, budget)
    assembled = assembled.replace(INSERT_SOURCE, "\n\n".join(source_bodies))
    hashes = [
        ("actual dma.c", DMA_SOURCE.read_bytes()),
        ("actual command.c", COMMAND_SOURCE.read_bytes()),
        ("actual clock.c", CLOCK_SOURCE.read_bytes()),
        ("actual host.h", HOST_HEADER.read_bytes()),
        ("actual dma.h", (SDMMC / "dma.h").read_bytes()),
        ("original DMA/register constant definitions",
         original_host_constants.encode("utf-8")),
        ("host-width materialized target constants",
         host_constants.encode("utf-8")),
        ("actual command.h", (SDMMC / "command.h").read_bytes()),
        ("actual clock.h", (SDMMC / "clock.h").read_bytes()),
        ("actual registers.h", (SDMMC / "registers.h").read_bytes()),
        ("actual memory.h", MEMORY_HEADER.read_bytes()),
        ("actual hardware/mmc.h", (INCLUDE / "hardware" / "mmc.h").read_bytes()),
        ("actual p4host_budget_ok", budget.encode("utf-8")),
        ("production clock_fail before LP64 cast",
         original_clock_fail.encode("utf-8")),
        ("production p4host_reset before LP64 cast",
         original_p4host_reset.encode("utf-8")),
        ("host-adapted clock functions",
         (definitions[-2] + "\n" + definitions[-1]).encode("utf-8")),
        ("assembled extracted fixture", assembled.encode("utf-8")),
    ]
    hashes.extend((f"{'host-adapted' if name in ('clock_fail', 'p4host_reset') else 'actual'} {name}", body.encode("utf-8"))
                  for name, body in zip(
                      ("p4host_zero", "p4host_move") + tuple(
                          item[1] for item in function_specs
                      ), definitions))
    return assembled, hashes


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble()
    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {digest(content)}")
    print(f"  fixture: {digest(FIXTURE.read_bytes())}")
    print(
        "Production DMA, command and full-controller-reset bodies are tested; "
        "MMIO, fences, cache backing RAM, clock progression and kernel callbacks "
        "are modeled. Selected constants and two clock ~3UL masks are typed as "
        "fixture ULONG (uint32_t) for RV32 semantics on LP64; original and "
        "adapted inputs are hashed. IPTR remains native. This is not silicon "
        "or a target ABI execution test."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sdmmc-dma-privacy-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sdmmc_dma_privacy.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        for optimization in (0, 2):
            binary = directory / f"test_sdmmc_dma_privacy_O{optimization}"
            command = compiler + [
                "-std=c99", f"-O{optimization}", "-g", "-Wall", "-Wextra",
                "-Wpedantic", "-Wconversion", "-Wshadow", "-Wstrict-prototypes",
                "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(SDMMC), "-I", str(INCLUDE), str(source_path), "-o", str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict C99 warnings")
            build = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, check=False, timeout=90)
            if build.returncode:
                sys.stderr.write(build.stdout)
                sys.stderr.write(build.stderr)
                return build.returncode or 1
            if build.stdout:
                sys.stdout.write(build.stdout)
            if build.stderr:
                sys.stderr.write(build.stderr)
            print(f"Running O{optimization} (30-second timeout)")
            try:
                result = subprocess.run([str(binary)], text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        check=False, timeout=30, env=environment)
            except subprocess.TimeoutExpired:
                print("FAIL: SDMMC DMA privacy fixture exceeded 30 seconds",
                      file=sys.stderr)
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: fixture did not report its complete case count",
                      file=sys.stderr)
                return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(run())
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
PY
