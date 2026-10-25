#!/usr/bin/env python3
"""Reject mismatched generated reference configurations before deployment.

This is an offline config check, not image/layout or hardware acceptance.
"""
import argparse
from pathlib import Path
import re

EXPECTED = {
    "CONFIG_IDF_TARGET": '"esp32p4"',
    "CONFIG_ESP32P4_SELECTS_REV_LESS_V3": "y",
    "CONFIG_ESP32P4_REV_MIN_FULL": "100",
    "CONFIG_ESP32P4_REV_MAX_FULL": "199",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ": "360",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360": "y",
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG": "y",
    "CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT": "y",
    "CONFIG_SPIRAM": "n",
    "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
    "CONFIG_ESPTOOLPY_FLASHMODE": '"dio"',
    "CONFIG_ESP_SDIO_HOST_INTERFACE": "y",
    "CONFIG_ESP_SDIO_GPIO_RESET_SLAVE": "-1",
    "CONFIG_ESP_GPIO_SLAVE_RESET_SLAVE": "-1",
    "CONFIG_ESP_SDIO_BUS_WIDTH": "1",
    "CONFIG_ESP_SDIO_CLOCK_FREQ_KHZ": "400",
    "CONFIG_ESP_SDIO_PIN_CLK": "18",
    "CONFIG_ESP_SDIO_PIN_CMD": "19",
    "CONFIG_ESP_SDIO_PIN_D0": "14",
    "CONFIG_ESP_SDIO_PIN_D1": "15",
    "CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED": "y",
}


def validate(text):
    values = {}
    errors = []
    for line in text.splitlines():
        disabled = re.fullmatch(r"# (CONFIG_\w+) is not set", line)
        setting = re.fullmatch(r"(CONFIG_\w+)=(.*)", line)
        if not disabled and not setting:
            continue
        key, value = (disabled[1], "n") if disabled else setting.groups()
        # Legacy SDK aliases may be repeated outside this safety contract.
        if key not in EXPECTED:
            continue
        if key in values:
            errors.append(f"duplicate setting: {key}")
        values[key] = value
    for key, expected in EXPECTED.items():
        if values.get(key) != expected:
            errors.append(f"{key}: expected {expected}, got {values.get(key)!r}")
    return errors


def selftest():
    fixture = "\n".join(f"{k}={v}" for k, v in EXPECTED.items())
    assert not validate(fixture)
    for key in EXPECTED:
        assert validate(fixture.replace(f"{key}={EXPECTED[key]}", f"{key}=wrong"))
        assert validate(fixture.replace(f"{key}={EXPECTED[key]}", ""))
        assert validate(fixture + f"\n{key}={EXPECTED[key]}")
    assert validate(fixture.replace("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360",
                                    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=400"))
    assert not validate(fixture.replace("CONFIG_SPIRAM=n", "# CONFIG_SPIRAM is not set"))
    print(f"PASS: config selftest, {len(EXPECTED)} required settings")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path, nargs="?")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        selftest()
    if args.config:
        errors = validate(args.config.read_text(encoding="utf-8"))
        if errors:
            print("FAIL:\n" + "\n".join(errors))
            return 1
        print(f"PASS: {len(EXPECTED)} generated config settings")
    elif not args.selftest:
        parser.error("config or --selftest required")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
