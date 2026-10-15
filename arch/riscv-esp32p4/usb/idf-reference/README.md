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
