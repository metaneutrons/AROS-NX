#!/usr/bin/env python3
"""Capture one seed boot and a bounded series of ESP32-P4 warm resets.

The port remains open throughout the run. Each boot gets exactly one USB EN
pulse, using the same DTR/RTS sequence as reset-and-log.py. Any failed or
incomplete capture ends the run; it is never retried or replaced.

Example:
    uv run --with pyserial python arch/riscv-esp32p4/tools/smp-retained-reset.py \
        /dev/cu.usbmodem1101 evidence/smp-retained-reset --cycles 20 --timeout 10
"""

import argparse
import hashlib
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


BAUD_RATE = 115200
READ_TIMEOUT_SECONDS = 0.1
RESET_ASSERT_SECONDS = 0.1
RESET_SETTLE_SECONDS = 0.15
POST_READY_SECONDS = 0.35
REQUIRED_WARM_RESETS = 20

ROM_ID = b"ESP-ROM:esp32p4-eco2-20240710"
READY_LINE = b"[smp] retained READY; primary parked before Exec"
RETAINED_PASS_LINE = b"[smp] retained PASS; hart1=1 guards=ok reset=clear clock=on"
SUPPRESSED_LINE = (
    b"[smp] suppressed state=0x00000000 hart=0x00000000 "
    b"echo=0x00000000 cause=0x00000000 pc=0x00000000 "
    b"result=0x00000000 reset=held clock=off"
)

ROM_BANNER_RE = re.compile(rb"ESP-ROM:")
RESET_CAUSE_RE = re.compile(
    rb"(?im)^rst:0x17\s+\(CHIP_USB_UART_RESET\),\s*"
    rb"boot:0x20f\s+\(SPI_FAST_FLASH_BOOT\)\s*$"
)
SUPPRESSED_RE = re.compile(rb"(?m)^" + re.escape(SUPPRESSED_LINE) + rb"\r?$")
RELEASE_RE = re.compile(
    rb"(?im)^\[smp\] release state=0x00000001 "
    rb"hart=0x00000001 echo=0xe1000002 cause=0x00000000 "
    rb"pc=0x00000000 "
    rb"\bresult=0x00000001 reset=clear clock=on\r?$"
)
INHERITED_RE = re.compile(
    rb"(?im)^\[smp\] early inherited reset=0x([0-9a-f]{8}) "
    rb"clock=0x([0-9a-f]{8})\r?$"
)
ISOLATION_RE = re.compile(
    rb"(?im)^\[smp\] early isolation reset=0x([0-9a-f]{8}) "
    rb"clock=0x([0-9a-f]{8}) boot=0x([0-9a-f]{8})\r?$"
)
READY_RE = re.compile(rb"(?m)^" + re.escape(READY_LINE) + rb"\r?$")
PSRAM_CHIP_RE = re.compile(
    rb"(?im)^\[psram\]\s*chip\s+32\s+MB\s+at\s+200\s*MHz\b"
    rb"[^\r\n]*?\bafter\s+([1-9][0-9]*)\s+attempts?\b[^\r\n]*\r?$"
)
PSRAM_WINDOW_RE = re.compile(
    rb"(?im)^\[psram\]\s+window\s+0x48000000\s+mapped,\s+"
    rb"one\s+word\s+per\s+megabyte\s+verified\s*$"
)
PSRAM_CALIBRATED_RE = re.compile(
    rb"(?im)^\[psram\](?![^\r\n]*\bNOT\s+calibrated\b)[^\r\n]*"
    rb"(?=.*\bcalibrat\w*\b)"
    rb"(?=.*\brunning\b)(?=.*\b200\s*MHz\b)[^\r\n]*\r?$"
)
PSRAM_COUNTERS_RE = re.compile(
    rb"(?im)^\[psram\][^\r\n]*\bcommand[\s_-]*timeouts?\s+0\s*,\s*"
    rb"FSM\s+recoveries?\s+0\b[^\r\n]*\r?$"
)
BSP_RE = re.compile(
    rb"(?im)^\[flash\]\s+pkg\s+loaded\s+35\s+modules(?:,\s+"
    rb"reserved\s+[0-9]+\s+bytes\s+of\s+PSRAM)?\s*$"
)
RETAINED_PASS_RE = re.compile(
    rb"(?m)^" + re.escape(RETAINED_PASS_LINE) + rb"\r?$"
)
ROM_DOWNLOAD_RE = re.compile(rb"download|waiting\s+for\s+download", re.IGNORECASE)
FATAL_RE = re.compile(
    rb"(?im)(?:\[smp\][^\r\n]*\bfail\b|\[trap\]|panic|guru|unsafe)"
)
EXEC_CONSOLE = b"[console] runtime output"


def _utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def analyze_log(data, first_byte_received, post_ready_seconds):
    """Return machine-readable criteria and observed marker counts for a boot."""
    rom_count = len(ROM_BANNER_RE.findall(data))
    rom_identity_count = data.count(ROM_ID)
    reset_count = len(RESET_CAUSE_RE.findall(data))
    suppressed_matches = list(SUPPRESSED_RE.finditer(data))
    release_matches = list(RELEASE_RE.finditer(data))
    suppressed_count = len(suppressed_matches)
    release_count = len(release_matches)
    inherited = list(INHERITED_RE.finditer(data))
    isolation = list(ISOLATION_RE.finditer(data))
    ready_count = len(READY_RE.findall(data))
    psram_chips = list(PSRAM_CHIP_RE.finditer(data))
    psram_attempts = [int(match.group(1)) for match in psram_chips]
    psram_window_count = len(PSRAM_WINDOW_RE.findall(data))
    psram_calibrated_count = len(PSRAM_CALIBRATED_RE.findall(data))
    psram_counter_count = len(PSRAM_COUNTERS_RE.findall(data))
    bsp_matches = list(BSP_RE.finditer(data))
    bsp_count = len(bsp_matches)
    retained_passes = list(RETAINED_PASS_RE.finditer(data))
    download = ROM_DOWNLOAD_RE.search(data) is not None
    fatal = FATAL_RE.search(data) is not None

    inherited_values = None
    if len(inherited) == 1:
        inherited_values = {
            "reset_register": int(inherited[0].group(1), 16),
            "clock_register": int(inherited[0].group(2), 16),
        }
        inherited_values["reset_clear"] = not bool(
            inherited_values["reset_register"] & 0x100
        )
        inherited_values["clock_on"] = bool(
            inherited_values["clock_register"] & 0x10
        )
        inherited_values["active_at_entry"] = (
            inherited_values["reset_clear"] and inherited_values["clock_on"]
        )

    isolation_values = None
    isolation_valid = False
    if len(isolation) == 1:
        reset_value = int(isolation[0].group(1), 16)
        clock_value = int(isolation[0].group(2), 16)
        boot_value = int(isolation[0].group(3), 16)
        isolation_values = {
            "reset_register": reset_value,
            "clock_register": clock_value,
            "boot_address": boot_value,
        }
        isolation_valid = (
            bool(reset_value & 0x100)
            and not bool(clock_value & 0x10)
            and boot_value == 0
        )

    ready_position = data.find(READY_LINE)
    exec_console_after_ready = (
        ready_position >= 0
        and data.find(EXEC_CONSOLE, ready_position + len(READY_LINE)) >= 0
    )

    rom_match = ROM_BANNER_RE.search(data)
    reset_matches = list(RESET_CAUSE_RE.finditer(data))
    psram_matches = [
        match
        for marker in (PSRAM_CHIP_RE, PSRAM_WINDOW_RE,
                       PSRAM_CALIBRATED_RE, PSRAM_COUNTERS_RE)
        for match in marker.finditer(data)
    ]
    ready_matches = list(READY_RE.finditer(data))
    ordered_positions = []
    if (rom_match and len(reset_matches) == 1 and len(psram_matches) == 4
            and len(bsp_matches) == 1 and len(inherited) == 1
            and len(isolation) == 1 and len(suppressed_matches) == 1
            and len(release_matches) == 1 and len(retained_passes) == 1
            and len(ready_matches) == 1):
        ordered_positions = [
            rom_match.start(), reset_matches[0].start(),
            max(match.start() for match in psram_matches), bsp_matches[0].start(),
            inherited[0].start(), isolation[0].start(), suppressed_matches[0].start(),
            release_matches[0].start(), retained_passes[0].start(),
            ready_matches[0].start(),
        ]
    ordered_unique = bool(ordered_positions) and all(
        before < after for before, after in zip(ordered_positions, ordered_positions[1:])
    ) and min(match.start() for match in psram_matches) > reset_matches[0].start()

    criteria = {
        "first_byte_received": bool(first_byte_received),
        "one_expected_rom_banner": rom_count == 1 and rom_identity_count == 1,
        "usb_reset_and_spi_boot_mode": reset_count == 1,
        "suppressed_launch_held_and_clocked_off": suppressed_count == 1,
        "positive_release_report": release_count == 1,
        "retained_pass_report": len(retained_passes) == 1,
        "inherited_register_snapshot_present": len(inherited) == 1,
        "early_isolation_confirmed_and_boot_address_cleared": (
            len(isolation) == 1 and isolation_valid
        ),
        "retained_ready_marker": ready_count == 1,
        "post_ready_observation_at_least_300ms": (
            ready_count == 1 and post_ready_seconds >= 0.3
        ),
        "psram_32mb_200mhz_with_positive_attempt_count": len(psram_chips) == 1,
        "psram_window_one_word_per_megabyte_verified": psram_window_count == 1,
        "psram_calibrated_running_200mhz": psram_calibrated_count == 1,
        "psram_command_timeouts_and_fsm_recoveries_zero": psram_counter_count == 1,
        "bsp_loaded_35_modules": bsp_count == 1,
        "required_markers_unique_and_ordered": ordered_unique,
        "no_download_mode": not download,
        "no_explicit_fatal_marker": not fatal,
        "no_exec_console_after_retained_ready": not exec_console_after_ready,
    }
    counts = {
        "rom_boot_banners": rom_count,
        "expected_rom_identity": rom_identity_count,
        "usb_reset_spi_boot_lines": reset_count,
        "suppressed_reports": suppressed_count,
        "positive_release_reports": release_count,
        "inherited_snapshots": len(inherited),
        "early_isolation_reports": len(isolation),
        "retained_ready_markers": ready_count,
        "psram_chip_reports": len(psram_chips),
        "psram_attempts": psram_attempts,
        "psram_window_verification_reports": psram_window_count,
        "psram_calibrated_200mhz_reports": psram_calibrated_count,
        "psram_zero_timeout_recovery_reports": psram_counter_count,
        "bsp_35_module_reports": bsp_count,
        "retained_pass_reports": len(retained_passes),
        "download_marker_count": len(ROM_DOWNLOAD_RE.findall(data)),
        "fatal_marker_count": len(FATAL_RE.findall(data)),
        "exec_console_after_ready_count": int(exec_console_after_ready),
    }
    return {
        "criteria": criteria,
        "counts": counts,
        "inherited_registers": inherited_values,
        "early_isolation_registers": isolation_values,
        "active_at_entry": bool(
            inherited_values and inherited_values["active_at_entry"]
        ),
        "passed": all(criteria.values()),
    }


def _set_read_timeout(port, seconds):
    if hasattr(port, "timeout"):
        port.timeout = max(0.001, seconds)


def _read(port, size, deadline, monotonic):
    remaining = deadline - monotonic()
    if remaining <= 0:
        return b""
    _set_read_timeout(port, min(READ_TIMEOUT_SECONDS, remaining))
    return port.read(size)


def capture_boot(port, timeout_seconds, monotonic=time.monotonic):
    """Capture through READY, then drain for at least 300 ms."""
    started = monotonic()
    deadline = started + timeout_seconds
    data = bytearray()
    first_byte_seconds = None
    ready_at = None
    capture_error = None

    try:
        while monotonic() < deadline and first_byte_seconds is None:
            chunk = _read(port, 1, deadline, monotonic)
            if chunk:
                data.extend(chunk)
                first_byte_seconds = monotonic() - started

        if first_byte_seconds is None:
            capture_error = "no first byte before per-boot timeout"
        else:
            while monotonic() < deadline:
                if ROM_DOWNLOAD_RE.search(data):
                    capture_error = "ROM download mode marker seen"
                    break
                if FATAL_RE.search(data):
                    capture_error = "explicit fatal marker seen"
                    break
                if len(ROM_BANNER_RE.findall(data)) > 1:
                    capture_error = "duplicate ROM boot banner seen"
                    break
                if READY_RE.search(data):
                    ready_at = monotonic()
                    break
                chunk = _read(port, 4096, deadline, monotonic)
                if chunk:
                    data.extend(chunk)

            if ready_at is None and capture_error is None:
                capture_error = "retained READY marker not seen before per-boot timeout"

        if ready_at is not None:
            dwell_deadline = ready_at + POST_READY_SECONDS
            while monotonic() < dwell_deadline:
                chunk = _read(port, 4096, dwell_deadline, monotonic)
                if chunk:
                    data.extend(chunk)
            post_ready_seconds = max(0.0, monotonic() - ready_at)
        else:
            post_ready_seconds = 0.0
    except KeyboardInterrupt:
        capture_error = "capture interrupted"
        post_ready_seconds = max(0.0, monotonic() - ready_at) if ready_at else 0.0
    except Exception as exc:  # Preserve partial bytes on serial exceptions.
        capture_error = f"serial capture error: {type(exc).__name__}: {exc}"
        post_ready_seconds = max(0.0, monotonic() - ready_at) if ready_at else 0.0

    analysis = analyze_log(bytes(data), first_byte_seconds is not None, post_ready_seconds)
    if capture_error:
        analysis["passed"] = False
    return {
        "data": bytes(data),
        "first_byte_wait_seconds": first_byte_seconds,
        "post_ready_observed_seconds": post_ready_seconds,
        "capture_error": capture_error,
        **analysis,
    }


def pulse_en_once(port, sleep=time.sleep):
    """Apply the existing reset-and-log.py DTR/RTS pulse exactly once."""
    pulse = {
        "attempted": True,
        "stage": "input_buffer_flush",
        "rts_asserted": False,
        "rts_deasserted": False,
        "completed": False,
        "assert_seconds": RESET_ASSERT_SECONDS,
        "settle_seconds": RESET_SETTLE_SECONDS,
    }
    try:
        port.reset_input_buffer()
        pulse["stage"] = "dtr_low_before_assert"
        port.setDTR(False)
        pulse["stage"] = "rts_assert"
        port.setRTS(True)
        pulse["rts_asserted"] = True
        pulse["stage"] = "asserted_hold"
        sleep(RESET_ASSERT_SECONDS)
        pulse["stage"] = "dtr_low_before_deassert"
        port.setDTR(False)
        pulse["stage"] = "rts_deassert"
        port.setRTS(False)
        pulse["rts_deasserted"] = True
        pulse["stage"] = "settle"
        sleep(RESET_SETTLE_SECONDS)
        pulse["stage"] = "complete"
        pulse["completed"] = True
    except KeyboardInterrupt:
        pulse["error"] = "KeyboardInterrupt"
    except Exception as exc:
        pulse["error"] = f"{type(exc).__name__}: {exc}"
    return pulse


def _write_summary(path, summary):
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_campaign(port_name, evidence_directory, cycles=REQUIRED_WARM_RESETS,
                 timeout_seconds=10.0, serial_factory=None,
                 monotonic=time.monotonic, sleep=time.sleep):
    """Run one seed boot and ``cycles`` one-pulse warm transitions."""
    if cycles < 1:
        raise ValueError("cycles must be positive")
    if timeout_seconds <= 0:
        raise ValueError("timeout must be positive")

    evidence = Path(evidence_directory)
    evidence.mkdir(parents=True, exist_ok=True)
    summary_path = evidence / "summary.json"
    raw_names = ["seed.log"] + [f"warm-{index:02d}.log" for index in range(1, cycles + 1)]
    collisions = [name for name in ["summary.json", *raw_names] if (evidence / name).exists()]
    if collisions:
        raise FileExistsError("evidence files already exist: " + ", ".join(collisions))

    summary = {
        "schema_version": 1,
        "created_at_utc": _utc_now(),
        "port": str(port_name),
        "baud_rate": BAUD_RATE,
        "timeout_seconds": timeout_seconds,
        "post_ready_observation_seconds_minimum": POST_READY_SECONDS,
        "requested_warm_resets": cycles,
        "required_warm_resets_for_gate": REQUIRED_WARM_RESETS,
        "reset_procedure": {
            "serial_connection_count": 1,
            "retry_count": 0,
            "dtr_low": True,
            "rts_assert_seconds": RESET_ASSERT_SECONDS,
            "rts_deassert_after_pulse": True,
            "settle_seconds": RESET_SETTLE_SECONDS,
            "reset_input_buffer_before_each_pulse": True,
        },
        "status": "running",
        "transition_counts": {
            "seed_pulse_attempts": 0,
            "seed_pulses_completed": 0,
            "seed_pulses_issued": 0,
            "seed_boots_passed": 0,
            "warm_pulse_attempts": 0,
            "warm_pulses_completed": 0,
            "warm_pulses_issued": 0,
            "warm_boots_passed": 0,
            "ambiguous_or_incomplete_pulses": 0,
            "active_at_entry_successors": 0,
            "normalized_or_other_inherited_successors": 0,
        },
        "boots": [],
        "failure": None,
    }
    _write_summary(summary_path, summary)

    if serial_factory is None:
        def serial_factory(name, baudrate, timeout):
            import serial  # pyserial is only required when the CLI opens hardware.
            return serial.Serial(name, baudrate=baudrate, timeout=timeout)

    serial_port = None
    try:
        serial_port = serial_factory(
            port_name, baudrate=BAUD_RATE, timeout=READ_TIMEOUT_SECONDS
        )
        plans = [("seed", 1, "seed.log")]
        plans.extend(("warm", index, f"warm-{index:02d}.log")
                     for index in range(1, cycles + 1))

        for phase, phase_index, filename in plans:
            pulse = {
                "issued_at_utc": _utc_now(),
                "dtr_low": True,
                "rts_assert_seconds": RESET_ASSERT_SECONDS,
                "rts_deasserted": True,
            }
            pulse.update(pulse_en_once(serial_port, sleep=sleep))
            if phase == "seed":
                summary["transition_counts"]["seed_pulse_attempts"] += 1
                if pulse["completed"]:
                    summary["transition_counts"]["seed_pulses_completed"] += 1
                    summary["transition_counts"]["seed_pulses_issued"] += 1
            else:
                summary["transition_counts"]["warm_pulse_attempts"] += 1
                if pulse["completed"]:
                    summary["transition_counts"]["warm_pulses_completed"] += 1
                    summary["transition_counts"]["warm_pulses_issued"] += 1
            if not pulse["completed"]:
                summary["transition_counts"]["ambiguous_or_incomplete_pulses"] += 1

            # Read once, boundedly, even after an incomplete pulse. This can
            # preserve bytes caused by an ambiguous edge; it never retries.
            result = capture_boot(
                serial_port, timeout_seconds, monotonic=monotonic
            )
            if not pulse["completed"]:
                result["passed"] = False
                if not result["capture_error"]:
                    result["capture_error"] = "reset pulse incomplete; no retry attempted"

            raw_path = evidence / filename
            raw_path.write_bytes(result["data"])
            boot_passed = bool(result["passed"])
            inherited = result["inherited_registers"]
            active_at_entry = phase == "warm" and bool(
                inherited and inherited["active_at_entry"]
            )

            if phase == "seed" and boot_passed:
                summary["transition_counts"]["seed_boots_passed"] += 1
            elif phase == "warm":
                if boot_passed:
                    summary["transition_counts"]["warm_boots_passed"] += 1
                    if active_at_entry:
                        summary["transition_counts"]["active_at_entry_successors"] += 1
                    else:
                        summary["transition_counts"]["normalized_or_other_inherited_successors"] += 1

            counts = dict(result["counts"])
            counts["seed_transitions_attempted"] = summary["transition_counts"]["seed_pulse_attempts"]
            counts["seed_transitions_completed"] = summary["transition_counts"]["seed_pulses_completed"]
            counts["warm_transitions_attempted"] = summary["transition_counts"]["warm_pulse_attempts"]
            counts["warm_transitions_completed"] = summary["transition_counts"]["warm_pulses_completed"]
            counts["seed_transitions_issued"] = summary["transition_counts"]["seed_pulses_issued"]
            counts["warm_transitions_issued"] = summary["transition_counts"]["warm_pulses_issued"]
            counts["successful_seed_boots"] = summary["transition_counts"]["seed_boots_passed"]
            counts["successful_warm_transitions"] = summary["transition_counts"]["warm_boots_passed"]

            summary["boots"].append({
                "phase": phase,
                "phase_index": phase_index,
                "pulse": pulse,
                "raw_log": {
                    "filename": filename,
                    "size_bytes": len(result["data"]),
                    "sha256": hashlib.sha256(result["data"]).hexdigest(),
                },
                "first_byte_wait_seconds": result["first_byte_wait_seconds"],
                "post_ready_observed_seconds": result["post_ready_observed_seconds"],
                "capture_error": result["capture_error"],
                "criteria": result["criteria"],
                "criterion_counts": result["counts"],
                "transition_counts": counts,
                "inherited_registers": inherited,
                "early_isolation_registers": result["early_isolation_registers"],
                "active_at_entry_successor": active_at_entry,
                "passed": boot_passed,
            })

            if not boot_passed:
                failed = [name for name, passed in result["criteria"].items() if not passed]
                summary["failure"] = {
                    "phase": phase,
                    "phase_index": phase_index,
                    "reason": result["capture_error"] or "failed criteria: " + ", ".join(failed),
                    "raw_log": filename,
                }
                break

            _write_summary(summary_path, summary)

        warm_passed = summary["transition_counts"]["warm_boots_passed"]
        campaign_passed = (
            summary["failure"] is None
            and summary["transition_counts"]["seed_boots_passed"] == 1
            and warm_passed == cycles
        )
        summary["acceptance"] = {
            "seed_boot_passed": summary["transition_counts"]["seed_boots_passed"] == 1,
            "warm_recovery_series_passed": campaign_passed,
            "e1_20_warm_reset_recovery_gate_passed": (
                campaign_passed and cycles >= REQUIRED_WARM_RESETS
            ),
            "active_hart_at_entry_gate_passed": (
                campaign_passed
                and cycles >= REQUIRED_WARM_RESETS
                and summary["transition_counts"]["active_at_entry_successors"]
                >= REQUIRED_WARM_RESETS
            ),
        }
        summary["status"] = (
            "active_at_entry_gate_passed"
            if summary["acceptance"]["active_hart_at_entry_gate_passed"]
            else "warm_recovery_passed" if campaign_passed else "failed"
        )
    except KeyboardInterrupt:
        summary["status"] = "failed"
        summary["failure"] = {
            "phase": "serial setup or campaign",
            "reason": "KeyboardInterrupt; campaign aborted without retry",
        }
    except Exception as exc:
        summary["status"] = "failed"
        summary["failure"] = {
            "phase": "serial setup or campaign",
            "reason": f"{type(exc).__name__}: {exc}",
        }
    finally:
        if serial_port is not None:
            try:
                serial_port.close()
            except Exception as exc:
                summary["close_error"] = f"{type(exc).__name__}: {exc}"
        summary["finished_at_utc"] = _utc_now()
        _write_summary(summary_path, summary)

    summary["summary_file"] = str(summary_path)
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="serial device, for example /dev/cu.usbmodem1101")
    parser.add_argument("evidence_directory", type=Path, help="new directory for raw logs and summary.json")
    parser.add_argument("--cycles", type=int, default=REQUIRED_WARM_RESETS,
                        help="one-pulse warm EN resets after the seed boot (default: 20)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="seconds per boot to receive first byte and READY (default: 10)")
    args = parser.parse_args(argv)
    if args.cycles < 1:
        parser.error("--cycles must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    try:
        result = run_campaign(
            args.port, args.evidence_directory, cycles=args.cycles,
            timeout_seconds=args.timeout,
        )
    except FileExistsError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    acceptance = result.get("acceptance", {})
    print(f"status={result['status']} "
          f"active_at_entry_gate={acceptance.get('active_hart_at_entry_gate_passed', False)} "
          f"summary={result.get('summary_file', args.evidence_directory / 'summary.json')}")
    counts = result.get("transition_counts", {})
    print("seed_pulses={seed_pulses_issued} warm_pulses={warm_pulses_issued} "
          "warm_recoveries={warm_boots_passed} active_at_entry={active_at_entry_successors}".format(**counts))
    if result.get("failure"):
        print(result["failure"].get("reason", "campaign failed"), file=sys.stderr)
    return 0 if result["status"] in {
        "warm_recovery_passed", "active_at_entry_gate_passed"
    } else 1


if __name__ == "__main__":
    raise SystemExit(main())
