# HS CDC reference measurement (not AROS firmware)

Development-only ESP32-P4 reference from ESP-IDF6.0.1 commit
8c19b156084a0753687347cca1f5355782893533 and esp_tinyusb2.2.1. The official
`tusb_serial_device` example is compiled unchanged through an include wrapper.
CMake checks that example's SHA-256; the dependency lock pins the complete
resolved graph (esp_tinyusb2.2.1 and tinyusb0.19.0~3).
After successful driver installation, a low-priority task samples the same
non-destructive registers as USBPortStatus every five seconds. It reports on
Debug/FS and, with an initialized/DTR-connected CDC channel, over native USB.
No FIFO pop or peripheral register write is added by the observer. A truncated
CDC dump is not evidence of a complete comparison. Echo and periodic dumps
share CDC: qualify them separately, not as an uninterrupted binary echo stream.

The primary comparison uses Espressif's default Buffer-DMA path. A pass would
verify this reference, not AROS DMA, PIO, remote access or firmware updates.
The application intentionally has no desktop, SD/radio or remote backend.

Build offline before any hardware action, with explicit IDF/toolchain paths
and an out-of-tree build and SDKCONFIG path. Retain the resolved dependency
lock, sdkconfig, image size/hash and compiler output. Assert target esp32p4,
minimum revision1.0 and SELECTS_REV_LESS_V3 (JC1060 is1.3), HS port, DMA and16MiB flash in the generated
configuration. Do not use the default rev3.1 build on the rev1.3 board.

For a controlled PIO comparison, use a different build directory/SDKCONFIG
and SDKCONFIG_DEFAULTS containing `sdkconfig.defaults;sdkconfig.pio` in that
order. Verify generated SLAVE=y and DMA unset. This reference is not the AROS
polled port: OS, interrupt scheduling and TinyUSB release differ. A difference
between modes on this reference alone must not be attributed to AROS DMA.

NEVER run `idf.py flash` or write the generated bootloader/partition table.
The CSV is a build-time mirror of the existing legacy layout only. Any later
approved test may write only the application image into verified ota_0 at
0x20000, preserving erase-sector tails and independently reading it back.
Retain the exact AROS core for restoration; BSP, helpers, metadata and SD stay
unchanged. A reference build larger than the prior AROS erase span needs a
larger pre-read, including the tail that must be restored. Scope/record every
write and restore; do not count application recovery as a USB firmware updater.
Fresh physical readiness is required for each cable/recovery interaction.

## Optional single-cable diagnostic journal (default OFF)

The JC1060 vendor tree at `~/Downloads/JC1060P470C_I_W_Y` contains generic
`bsp_usb_host_start()` implementations but no invoked HS USB-device startup
in the inspected IDF/Arduino demos. Those host helpers are not a known-good
CDC reference. The unmodified official CDC example remains the comparison.

When native CDC is absent, an explicit development build can retain one
pre-write register snapshot after sample6 (about30 seconds). This is NOT a
read-only-flash reference: `HS_REFERENCE_PERSIST=ON` permits one4096-byte
erase/write at app-relative0x37000, absolute0x57000. It is restricted to
JC1060 MAC80:f1:b2:d3:3b:a6 and the existing ota_0 at0x20000/0x800000.
The complete original sector must already be retained for exact restoration;
pass its CRC32 as `HS_REFERENCE_PREIMAGE_CRC32=0x85539a44` only for the
documented A6knoc preimage, not as a reusable board constant.

The unsigned image including checksum padding and SHA must end before that
sector. Signed-image configurations fail compilation; hardware secure boot,
encrypted app partitions, an attached Debug/FS host, unknown/partial sector
contents and image overlap refuse new writes. A valid old record is retained,
not replaced. Failed/uncertain operations are not retried. CRC is a corruption
guard, not authentication. Debug/FS absence alone does not prove an HS cable
connection: synchronize the physical run and label the execution explicitly.

The current candidate is build verified only. Before deployment identify the
board/table again, verify the full live scratch preimage and preserve app erase
tails. Never write generated metadata. For an approved HS run wait at least35
seconds, return to Debug/FS, retain/read the sector before any overwrite, then
decode the saved file with `python3 decode-journal.py SECTOR`. Restore the
entire original AROS0x38000-byte span, including the journal sector, and verify
it independently. No valid record is a failed/inconclusive measurement, not a
reason for an automatic retry. The snapshot precedes its flash operation;
later USB behavior is not an undisturbed reference measurement.

Offline checks: compile `main/journal.c` and `tests/test_journal.c` with the
`main` include directory, C11, `-Wall -Wextra -Werror`, O0/O2 and ASan/UBSan;
run both executables. Run `python3 decode-journal.py --self-test` separately.
These checks do not establish HS enumeration or native flash-write safety.

Reachability limit: observer creation follows successful tinyusb_driver_install.
The official example aborts on install errors; none of those paths can reach
sample6. An unchanged scratch sector therefore does not establish that app_main
was never reached, and the current journal does not persist refusal reasons.
The35s cable wait is not a deadline measured from observer creation. The exact
journal binary reaches sample6 at30220ms in the FS control, with result=-1
(expected with an FS host). There is still no HS runtime result. For a commanded
Debug/FS boot, do not rely on the generic DTR-only devserial reset macro:
set DTR=false, pulse RTS=true for200ms, then RTS=false with capture open.
Keep this control-line correction separate from the unresolved HS cold-start.
