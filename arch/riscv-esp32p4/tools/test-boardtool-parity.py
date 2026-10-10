#!/usr/bin/env python3
"""Source-level guard for the shared ESP32Board CLI/MUI operation routes."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
APP = ROOT / "workbench/tools/ESP32Board"


def inspect(header: str, main: str, worker: str) -> list[str]:
    problems: list[str] = []
    table_match = re.search(
        r"static\s+const\s+struct\s+BoardToolOperation\s+"
        r"boardtool_operations\[\]\s*=\s*\{(.*?)\n\};",
        header,
        re.S,
    )
    if not table_match:
        return ["shared operation registry is missing"]

    rows = re.findall(
        r'\{\s*BOARDTOOL_OP_(\w+)\s*,\s*"([A-Z0-9-]+)"\s*,\s*'
        r'"[^"]*"\s*,\s*(TRUE|FALSE)\s*\}',
        table_match.group(1),
    )
    registry = {name: (identifier, available == "TRUE")
                for identifier, name, available in rows}
    expected = {
        "INFO": ("INFO", True),
        "CHECK-C6": ("CHECK_C6", True),
        "LAYOUT": ("LAYOUT", True),
        "UPDATE-C6": ("UPDATE_C6", False),
    }
    if registry != expected or len(rows) != len(expected):
        problems.append(f"unexpected/duplicate capabilities in registry: {registry}")

    for name in expected:
        token = f'boardtool_find_operation_name("{name}")'
        if token not in main:
            problems.append(f"GUI does not resolve {name} from the shared registry")

    required_main = {
        "CLI action lookup": "boardtool_find_operation_name(action)",
        "CLI shared backend dispatch": "boardtool_dispatch(operation->id, &message)",
        "GUI information dispatch": "boardtool_dispatch(info->id, work_message)",
        "GUI check button ID": "MUIM_Application_ReturnID, check->id",
        "GUI imported-layout button ID": "MUIM_Application_ReturnID, layout->id",
        "GUI check worker request": "send_operation((ULONG)id, path, digest, flash_mb)",
        "GUI worker-message ownership guard": "!busy && !close_pending",
        "GUI close remains deferred during work": "if (close_pending && !busy)",
        "GUI unavailable-update state": "MUIA_Disabled, !update->available",
        "shared dispatch refuses unavailable operation": "if (!descriptor->available)",
        "horizontal partition graph": "layout_group = (Object *)HGroup",
    }
    for label, token in required_main.items():
        if token not in main:
            problems.append(f"missing {label}")

    close_guard = main.find("if (done || (close_pending && !busy))")
    event_wait = main.find("signals = Wait(signals | reply_signal")
    if close_guard < 0 or event_wait < 0 or close_guard > event_wait:
        problems.append("close completion can wait for another input signal")

    if ("boardtool_find_operation_id(message->operation)" not in worker or
            "boardtool_dispatch(operation->id," not in worker):
        problems.append("worker does not dispatch the queued shared operation ID")
    if "Imported preview, not live/active, no write" not in main:
        problems.append("CLI layout output lacks imported/non-live/no-write provenance")
    return problems


def main() -> int:
    header = (APP / "boardtool.h").read_text(encoding="utf-8")
    frontend = (APP / "main.c").read_text(encoding="utf-8")
    worker = (APP / "worker.c").read_text(encoding="utf-8")
    failures = inspect(header, frontend, worker)
    if failures:
        raise SystemExit("ESP32Board parity guard failed:\n  - " + "\n  - ".join(failures))

    # Counter-probes prove that a GUI-only capability drift is caught. They
    # alter only in-memory source strings and create no temporary files.
    gui_drift = frontend.replace(
        'boardtool_find_operation_name("CHECK-C6")',
        'boardtool_find_operation_name("INFO")',
        1,
    )
    if not inspect(header, gui_drift, worker):
        raise SystemExit("ESP32Board parity guard missed a GUI action drift")

    update_enabled = header.replace(
        '{ BOARDTOOL_OP_UPDATE_C6, "UPDATE-C6", "Update C6 (unavailable)",       FALSE }',
        '{ BOARDTOOL_OP_UPDATE_C6, "UPDATE-C6", "Update C6 (unavailable)",       TRUE  }',
        1,
    )
    if not inspect(update_enabled, frontend, worker):
        raise SystemExit("ESP32Board parity guard missed an enabled update")

    close_drift = frontend.replace(
        "if (done || (close_pending && !busy))", "if (FALSE)", 1)
    if not inspect(header, close_drift, worker):
        raise SystemExit("ESP32Board guard missed a blocking close regression")

    print("ESP32Board parity guard passed; three counter-probes rejected.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
