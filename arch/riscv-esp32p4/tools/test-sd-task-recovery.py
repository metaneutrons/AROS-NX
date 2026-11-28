#!/usr/bin/env python3
"""Compile bounded extracted-source tests for SD BusTask recovery dispatch."""

from __future__ import annotations

import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[3]
PORT = Path(__file__).resolve().parents[1]
FIXTURE = PORT / "network" / "test_sd_task_recovery.c"
TOOLS = PORT / "tools"
HOST_HELPERS = TOOLS / "test-sdmmc-host.py"
RECOVERY_HELPERS = TOOLS / "test-sd-recovery.py"
GENERIC_BUS = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.c"
BUS_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_bus.h"
UNIT_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_unit.h"
BASE_HEADER = ROOT / "rom" / "devs" / "sdcard" / "sdcard_base.h"
EXEC_IO_HEADER = ROOT / "compiler" / "include" / "exec" / "io.h"
ERRORS_HEADER = ROOT / "compiler" / "include" / "exec" / "errors.h"
TRACKDISK_HEADER = ROOT / "compiler" / "include" / "devices" / "trackdisk.h"
NEWSTYLE_HEADER = ROOT / "compiler" / "include" / "devices" / "newstyle.h"
SCSIDISK_HEADER = ROOT / "compiler" / "include" / "devices" / "scsidisk.h"

COMMAND_MARKER = "/* INSERT_ACTUAL_SD_COMMANDS */"
MACRO_MARKER = "/* INSERT_ACTUAL_SD_FUNCTION_MACROS */"
TYPE_MARKER = "/* INSERT_ACTUAL_SD_TYPES */"
FUNCTION_MARKER = "/* INSERT_ACTUAL_SD_FUNCTIONS */"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_module(name: str, path: Path):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError(f"could not load test helpers from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def assemble_fixture() -> tuple[str, list[tuple[str, bytes]]]:
    host = load_module("p4_sdmmc_host_test_helpers", HOST_HELPERS)
    recovery = load_module("p4_sd_recovery_test_helpers", RECOVERY_HELPERS)
    fixture = FIXTURE.read_text(encoding="utf-8")
    markers = (COMMAND_MARKER, MACRO_MARKER, TYPE_MARKER, FUNCTION_MARKER)
    for marker in markers:
        if fixture.count(marker) != 1:
            raise ValueError(f"fixture must contain exactly one {marker}")

    constants = "\n".join(
        (
            recovery.extract_constants(
                EXEC_IO_HEADER,
                ("CMD_READ", "CMD_WRITE", "CMD_UPDATE", "CMD_NONSTD"),
            ),
            recovery.extract_constants(
                TRACKDISK_HEADER,
                (
                    "TD_READ64",
                    "TD_WRITE64",
                    "TD_CHANGESTATE",
                    "TD_PROTSTATUS",
                    "TD_ADDCHANGEINT",
                    "TD_REMCHANGEINT",
                    "TDERR_DiskChanged",
                ),
            ),
            recovery.extract_constants(
                NEWSTYLE_HEADER, ("NSCMD_TD_READ64", "NSCMD_TD_WRITE64")
            ),
            recovery.extract_constants(SCSIDISK_HEADER, ("HD_SCSICMD",)),
            recovery.extract_constants(ERRORS_HEADER, ("IOERR_BADADDRESS",)),
        )
    )
    macros = "\n".join(
        (
            recovery.extract_constants(BUS_HEADER, ("FNAME_SDCBUS",)),
            recovery.extract_constants(BASE_HEADER, ("FNAME_SDC",)),
            recovery.extract_constants(UNIT_HEADER, ("Unit",)),
        )
    )
    types = "\n\n".join(
        (
            extract_legacy_struct(SCSIDISK_HEADER, "SCSICmd"),
            recovery.extract_struct(host, UNIT_HEADER, "sdcard_Unit"),
            recovery.extract_struct(host, BUS_HEADER, "sdcard_CardInfo"),
            recovery.extract_struct(host, BUS_HEADER, "sdcard_BusUnits"),
            recovery.extract_struct(host, BUS_HEADER, "sdcard_Bus"),
        )
    )
    functions = "\n\n".join(
        (
            recovery.extract_function(
                host,
                GENERIC_BUS,
                "sdcard_TaskRecoverUnit",
                r"\bstatic\s+BOOL\s+sdcard_TaskRecoverUnit\s*\(",
            ),
            recovery.extract_function(
                host,
                GENERIC_BUS,
                "sdcard_TaskRecoverBus",
                r"\bstatic\s+void\s+sdcard_TaskRecoverBus\s*\(",
            ),
            recovery.extract_function(
                host,
                GENERIC_BUS,
                "sdcard_TaskHandleIO",
                r"\bstatic\s+BOOL\s+sdcard_TaskHandleIO\s*\(",
            ),
        )
    )

    assembled = fixture.replace(COMMAND_MARKER, constants)
    assembled = assembled.replace(MACRO_MARKER, macros)
    assembled = assembled.replace(TYPE_MARKER, types)
    assembled = assembled.replace(FUNCTION_MARKER, functions)
    hashes = [
        ("production command constants", constants.encode("utf-8")),
        ("production function macros", macros.encode("utf-8")),
        ("production structs", types.encode("utf-8")),
        ("production dispatch functions", functions.encode("utf-8")),
        ("test fixture", FIXTURE.read_bytes()),
        ("assembled fixture", assembled.encode("utf-8")),
    ]
    return assembled, hashes


def strip_comments(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)


def extract_legacy_struct(path: Path, name: str) -> str:
    source = path.read_bytes().decode("latin-1")
    match = re.search(rf"\bstruct\s+{re.escape(name)}\s*(?=\{{)", source)
    if match is None:
        raise ValueError(f"could not find struct {name} in {path}")
    opening = source.find("{", match.end())
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[match.start() : position + 1] + ";"
    raise ValueError(f"unterminated struct {name} in {path}")


def audit_bus_task_dispatch(host) -> list[tuple[str, str]]:
    bus_task = host.extract_definition(
        GENERIC_BUS,
        "BusTask",
        r"\bvoid\s+FNAME_SDCBUS\s*\(\s*BusTask\s*\)\s*\(",
    )
    unit_recovery = host.extract_definition(
        GENERIC_BUS,
        "sdcard_TaskRecoverUnit",
        r"\bstatic\s+BOOL\s+sdcard_TaskRecoverUnit\s*\(",
    )
    recover_unit = host.extract_definition(
        GENERIC_BUS,
        "RecoverUnit",
        r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*RecoverUnit\s*\)\s*\(",
    )
    start_unit = host.extract_definition(
        GENERIC_BUS,
        "StartUnit",
        r"\bBOOL\s+FNAME_SDCBUS\s*\(\s*StartUnit\s*\)\s*\(",
    )
    bus_recovery = host.extract_definition(
        GENERIC_BUS,
        "sdcard_TaskRecoverBus",
        r"\bstatic\s+void\s+sdcard_TaskRecoverBus\s*\(",
    )
    io_dispatch = host.extract_definition(
        GENERIC_BUS,
        "sdcard_TaskHandleIO",
        r"\bstatic\s+BOOL\s+sdcard_TaskHandleIO\s*\(",
    )

    source = strip_comments(bus_task)
    wake = re.search(
        r"if\s*\(\s*tasksig\s*&\s*\(\s*1L\s*<<\s*bus->sdcb_TaskSig\s*\)\s*\)\s*"
        r"sdcard_TaskRecoverBus\s*\(\s*bus\s*\)\s*;",
        source,
    )
    queue = source.find("GetMsg(bus->sdcb_MsgPort)")
    wrapped_dispatch = source.find("sdcard_TaskHandleIO(msg)")
    if not wake:
        raise ValueError("BusTask must dispatch explicit TaskSig through sdcard_TaskRecoverBus")
    if queue < 0 or wake.start() >= queue:
        raise ValueError("BusTask recovery wake must run before queued IO is dequeued")
    if wrapped_dispatch < queue or "FNAME_SDC(HandleIO)(msg)" in source:
        raise ValueError("BusTask queue must dispatch through sdcard_TaskHandleIO")

    lock_depth = 0
    lock_tokens = re.finditer(
        r"\b(KrnSpinLock|KrnSpinUnLock)\s*\(|"
        r"\b(sdcard_TaskRecoverBus|sdcard_TaskHandleIO|GetMsg)\s*\(",
        source,
    )
    checked_calls: list[str] = []
    for token in lock_tokens:
        name = token.group(1) or token.group(2)
        if name == "KrnSpinLock":
            lock_depth += 1
        elif name == "KrnSpinUnLock":
            lock_depth -= 1
            if lock_depth < 0:
                raise ValueError("BusTask spin-lock source order unlocks without locking")
        else:
            if lock_depth != 0:
                raise ValueError(f"BusTask calls {name} while a metadata spinlock is held")
            checked_calls.append(name)
    if lock_depth != 0:
        raise ValueError("BusTask spin-lock source order is unbalanced")
    if checked_calls.count("sdcard_TaskRecoverBus") != 1:
        raise ValueError("BusTask must have one explicit recovery wrapper call")
    if checked_calls.count("sdcard_TaskHandleIO") != 1:
        raise ValueError("BusTask must have one wrapped IO dispatch call")
    if checked_calls.count("GetMsg") != 1:
        raise ValueError("BusTask must have one queue dequeue site")

    for label, body, hardware_call in (
        ("TaskRecoverUnit", unit_recovery, r"FNAME_SDCBUS\s*\(\s*RecoverUnit\s*\)"),
        ("TaskRecoverBus", bus_recovery, r"sdcard_TaskRecoverUnit\s*\("),
        ("TaskHandleIO", io_dispatch, r"FNAME_SDC\s*\(\s*HandleIO\s*\)"),
    ):
        clean = strip_comments(body)
        if not re.search(hardware_call, clean):
            raise ValueError(f"{label} no longer reaches its expected production dispatch")
        if label != "TaskRecoverUnit" and re.search(
            r"\b(?:KrnSpinLock|KrnSpinUnLock|SDCBUS_LockUnit|SDCBUS_UnlockUnit)\s*\(",
            clean,
        ):
            raise ValueError(f"{label} contains a hardware dispatch under a metadata lock")

    dispatch_source = strip_comments(io_dispatch)
    gated_commands = re.findall(
        r"io->io_Command\s*==\s*([A-Z0-9_]+)", dispatch_source
    )
    if gated_commands != ["CMD_READ", "TD_READ64", "NSCMD_TD_READ64"]:
        raise ValueError("TaskHandleIO must gate exactly the three sector-read commands")

    unit_recovery_source = strip_comments(unit_recovery)
    unit_guard_tokens = list(
        re.finditer(
            r"\b(SDCBUS_LockUnit|SDCBUS_UnlockUnit)\s*\(|"
            r"\b(FNAME_SDCBUS\s*\(\s*RecoverUnit\s*\))\s*\(",
            unit_recovery_source,
        )
    )
    unit_guard_depth = 0
    unit_guard_counts = {"SDCBUS_LockUnit": 0, "SDCBUS_UnlockUnit": 0}
    recovery_calls = 0
    for token in unit_guard_tokens:
        name = token.group(1) or token.group(2)
        if name == "SDCBUS_LockUnit":
            if unit_guard_depth != 0:
                raise ValueError("TaskRecoverUnit nests its metadata guard")
            unit_guard_depth += 1
            unit_guard_counts[name] += 1
        elif name == "SDCBUS_UnlockUnit":
            if unit_guard_depth != 1:
                raise ValueError("TaskRecoverUnit unlocks an unheld metadata guard")
            unit_guard_depth -= 1
            unit_guard_counts[name] += 1
        else:
            if unit_guard_depth != 0:
                raise ValueError("TaskRecoverUnit calls RecoverUnit while guarded")
            recovery_calls += 1
    if unit_guard_depth != 0 or unit_guard_counts != {
        "SDCBUS_LockUnit": 2,
        "SDCBUS_UnlockUnit": 2,
    } or recovery_calls != 1:
        raise ValueError("TaskRecoverUnit must use two balanced guards around one unguarded recovery")
    reserve = re.search(
        r"if\s*\(\s*!blocked\s*\)\s*unit->sdcu_RecoveryBlocked\s*=\s*TRUE\s*;",
        unit_recovery_source,
    )
    final_latch = re.search(
        r"unit->sdcu_RecoveryBlocked\s*=\s*!result\s*;", unit_recovery_source
    )
    guard_positions = [
        (token.group(1) or token.group(2), token.start())
        for token in unit_guard_tokens
    ]
    if not reserve or not final_latch:
        raise ValueError("TaskRecoverUnit must reserve and publish its failure latch")
    if not (
        guard_positions[0][0] == "SDCBUS_LockUnit"
        and guard_positions[0][1] < reserve.start() < guard_positions[1][1]
        and guard_positions[1][1] < guard_positions[2][1] < guard_positions[3][1]
        and guard_positions[2][0] == "FNAME_SDCBUS(RecoverUnit)"
        and guard_positions[3][0] == "SDCBUS_LockUnit"
        and guard_positions[3][1] < final_latch.start() < unit_guard_tokens[4].start()
        and (unit_guard_tokens[4].group(1) or unit_guard_tokens[4].group(2))
        == "SDCBUS_UnlockUnit"
    ):
        raise ValueError("TaskRecoverUnit latch reservation/publication is outside its short guards")

    recover_source = strip_comments(recover_unit)
    unit_lock_tokens = list(
        re.finditer(r"\bSDCBUS_(?:LockUnit|UnlockUnit)\s*\(", recover_source)
    )
    callback_tokens = list(
        re.finditer(
            r"\b(bus->sdcb_Recovery(?:Needed|Prepare|Finish)|"
            r"FNAME_SDCBUS\s*\(\s*IdentifyCardLocked\s*\)|"
            r"sdcard_StartUnitLocked)\s*\(",
            recover_source,
        )
    )
    hardware_callbacks: list[str] = []
    for callback in callback_tokens:
        locks_before = [token for token in unit_lock_tokens if token.start() < callback.start()]
        last_lock = max(
            (token for token in locks_before if token.group(0).startswith("SDCBUS_LockUnit")),
            key=lambda token: token.start(),
            default=None,
        )
        last_unlock = max(
            (token for token in locks_before if token.group(0).startswith("SDCBUS_UnlockUnit")),
            key=lambda token: token.start(),
            default=None,
        )
        if last_lock is not None and (last_unlock is None or last_unlock.start() < last_lock.start()):
            raise ValueError(
                f"RecoverUnit invokes {callback.group(1)} while metadata lock is held"
            )
        hardware_callbacks.append(callback.group(1))
    expected_callbacks = {
        "bus->sdcb_RecoveryNeeded",
        "bus->sdcb_RecoveryPrepare",
        "bus->sdcb_RecoveryFinish",
        "FNAME_SDCBUS(IdentifyCardLocked)",
        "sdcard_StartUnitLocked",
    }
    if not expected_callbacks.issubset(set(hardware_callbacks)):
        raise ValueError("RecoverUnit recovery callbacks or activation path changed")

    start_source = strip_comments(start_unit)
    acquire = re.search(
        r"SDCBUS_BeginTransaction\s*\(\s*bus\s*,\s*FALSE\s*\)", start_source
    )
    start_lock = re.search(r"SDCBUS_LockUnit\s*\(\s*bus\s*\)", start_source)
    blocked_guard = re.search(
        r"if\s*\(\s*sdcUnit->sdcu_RecoveryBlocked\s*\)\s*\{(?P<body>.*?)\}",
        start_source,
        flags=re.DOTALL,
    )
    start_activation = re.search(
        r"sdcard_StartUnitLocked\s*\(\s*&candidate\s*\)", start_source
    )
    start_unlocks = list(re.finditer(r"SDCBUS_UnlockUnit\s*\(\s*bus\s*\)", start_source))
    if not all((acquire, start_lock, blocked_guard, start_activation, start_unlocks)):
        raise ValueError("StartUnit recovery guard/acquisition boundary is incomplete")
    if not (
        acquire.start() < start_lock.start() < blocked_guard.start() < start_activation.start()
        and len(re.findall(r"SDCBUS_LockUnit\s*\(\s*bus\s*\)", start_source)) == 1
        and start_unlocks[-1].start() < start_activation.start()
    ):
        raise ValueError("StartUnit must acquire nonblocking, inspect the latch under guard, then activate")
    blocked_body = blocked_guard.group("body")
    if not (
        "SDCBUS_UnlockUnit(bus)" in blocked_body
        and "SDCBUS_EndTransaction(bus)" in blocked_body
        and re.search(r"return\s+FALSE\s*;", blocked_body)
    ):
        raise ValueError("StartUnit must refuse a blocked unit before card activation")
    last_start_lock = start_source.rfind("SDCBUS_LockUnit(bus)", 0, start_activation.start())
    last_start_unlock = start_source.rfind("SDCBUS_UnlockUnit(bus)", 0, start_activation.start())
    if last_start_unlock < last_start_lock:
        raise ValueError("StartUnit invokes card activation while metadata guard is held")

    task_wake_hash = sha256(bus_task.encode("utf-8"))
    return [
        ("BusTask wake-before-dequeue/no-spin source audit", task_wake_hash),
        ("TaskRecoverUnit balanced reservation guards/source audit", sha256(unit_recovery.encode("utf-8"))),
        ("StartUnit active-open refusal/source-boundary audit", sha256(start_unit.encode("utf-8"))),
        ("RecoverUnit callbacks-outside-metadata-lock source audit", sha256(recover_unit.encode("utf-8"))),
        ("TaskRecoverBus iteration source audit", sha256(bus_recovery.encode("utf-8"))),
        ("TaskHandleIO single-dispatch source audit", sha256(io_dispatch.encode("utf-8"))),
    ]


def run() -> int:
    compiler = shlex.split(os.environ.get("CC", "cc"))
    if not compiler:
        raise ValueError("CC is empty")
    assembled, hashes = assemble_fixture()
    host = load_module("p4_sdmmc_host_test_helpers_audit", HOST_HELPERS)

    print("Extracted production inputs (SHA-256):")
    for name, content in hashes:
        print(f"  {name}: {sha256(content)}")
    for name, digest in audit_bus_task_dispatch(host):
        print(f"  {name}: PASS ({digest})")
    print(
        "Model: extracted TaskRecoverUnit/TaskRecoverBus/TaskHandleIO execute "
        "with scripted RecoverUnit and HandleIO boundaries. No card discovery, "
        "controller, DMA, media, or hardware recovery is exercised."
    )

    with tempfile.TemporaryDirectory(prefix="p4-sd-task-recovery-") as temporary:
        directory = Path(temporary)
        source_path = directory / "test_sd_task_recovery.c"
        source_path.write_text(assembled, encoding="utf-8")
        environment = os.environ.copy()
        environment["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"

        for optimization in (0, 2):
            binary = directory / f"test_sd_task_recovery_O{optimization}"
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
                str(source_path),
                "-o",
                str(binary),
            ]
            print(f"\nBuilding O{optimization} with ASan/UBSan and strict warnings")
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

            print(f"Running O{optimization} (20-second timeout)")
            try:
                result = subprocess.run(
                    [str(binary)],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    timeout=20,
                    env=environment,
                )
            except subprocess.TimeoutExpired:
                print("FAIL: test binary exceeded 20 seconds", file=sys.stderr)
                return 1
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            if result.returncode != 0:
                return result.returncode
            if not re.search(r"(?m)^PASS: \d+ cases, \d+ checks$", result.stdout):
                print("FAIL: incomplete case/check summary", file=sys.stderr)
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(run())
