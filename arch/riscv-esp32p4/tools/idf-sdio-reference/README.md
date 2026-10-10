# Bounded P4 SDIO reference diagnostic

This is an ESP-IDF comparison instrument, not the AROS WLAN driver.
It uses slot1, one-bit mode,400kHz and P4 CLK18/CMD19/D0-3=14-17.
Only CMD0 and up to three CMD5(arg0) requests are sent. Each command has
a500ms timeout and retries are100ms apart. No card initialization,
CMD52/CMD53, SD data writes or C6 firmware programming is performed.
CMD0 **does change protocol state** and can interrupt an existing C6 session.
No successful OCR is inferred from a failed transaction.

## Before deployment

- Obtain fresh user readiness and coordinate complete rail-off. A USB/EN
  reset is not proof that retained display DMA has stopped. IDF startup
  does not invoke its restart-path DMA reset routine. Do not run this
  diagnostic directly over an active AROS scanout session after USB reset.
- Verify JC1060 MAC `80:f1:b2:d3:3b:a6`, P4 revision1.3 and image hash.
- Independently back up and verify the whole rounded core erase span.
  The diagnostic app and restored AROS core may have different lengths.
- Use only an explicit raw app-only write at **0x20000**. Never use
  `idf.py flash`, `idf.py app-flash`, `@flash_args` or the generated
  instructions: their default app offset is0x10000, and full flashing
  overwrites the AROS bootloader and partition table.
- Preserve AROS bootloader, partition table, BSP, development volume,
  SD media and C6 firmware. Capture from the first boot byte; verify
  process completion and actual command results. Restore AROS and verify
  independent full-span readback after measurement.

## Build identity

Use ESP-IDF6.0.1 and a fresh external build/config directory. Set
`ESP_IDF_VERSION=6.0.1`, `IDF_TARGET=esp32p4`, `IDF_PATH` and the installed
IDF Python/toolchain environment explicitly. Pass an external `SDKCONFIG`
to `idf.py` so generated configuration does not enter this source tree.
Check the generated config and `esptool image-info`, not defaults alone:
min revision100/max199, CPU360MHz,16MB DIO80MHz,USB Serial/JTAG console,
PSRAM disabled. `ESP32P4_SELECTS_REV_LESS_V3=y` is required in addition
to `ESP32P4_REV_MIN_100=y`.

Exact artifacts, failed builds and hardware results belong in the port
ROADMAP evidence log. A successful build or CMD5 response is not WLAN,
AROS shared-host, SD integrity or display qualification.
