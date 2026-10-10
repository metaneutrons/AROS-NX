# Complete Hosted reference: build only

Not approved for flashing. Never execute the generated `idf.py flash` or
`flash_args`: they use a different partition layout and would overwrite AROS
bootloader/partition state. Passing compilation does not verify Wi-Fi.

See `../../network/HOSTED-REFERENCE.md` for archive hashes, external workspace,
dependency revisions, hardware safety gates and evidence.

Build with IDF6.0.1 and the pinned external components. From a fresh verified
Hosted d518 archive (including its exact protobuf-c gitlink), apply the retained
compatibility patch with `git apply --check`, then `git apply`.
Do not apply it to another upstream revision or already patched source.

Set `HOSTED_REFERENCE_ROOT` to the external directory containing
`esp-hosted-d5184217f710d54c01f932e75b4c55e3f4933fc9` and
`remote-0.2.3/esp_wifi_remote`. Set IDF_PATH and the matching GCC/Python paths,
IDF_PYTHON_ENV_PATH, ESP_IDF_VERSION=6.0.1 and IDF_COMPONENT_MANAGER=0.
Use an external build directory and SDKCONFIG path to keep generated files
outside the repository. Run `idf.py -B <build> -D SDKCONFIG=<config> build`.
For a fresh configuration, SDKCONFIG_DEFAULTS must point to this directory's
sdkconfig.defaults. Verify the resulting settings and the final image header.

## Compatibility changes and limitations

- Explicit IDF6 component dependencies for SDMMC, DMA, GPIO and Wi-Fi remote.
- Keep libc's existing endian macros instead of redefining them.
- Replace the removed DMA allocator with an overflow-checked 64-byte-aligned,
  size-rounded heap allocation. Explicit INTERNAL/8BIT/DMA caps deliberately
  restrict placement more than the old helper; the reference refuses PSRAM.
  IDF's heap performs additional hardware/cache alignment handling. This is
  not an allocator for a production PSRAM-enabled port.
- Preserve known old STA/HE wire feature mappings. Outgoing opaque reserved
  bits are zero; incoming reserved bits are ignored, not copied into new native
  feature/reserved positions. New IDF6 features are not represented in the
  legacy wire schema. Schema/generated protobuf files remain byte-identical.
- Recognize the two new HTTP event variants without enabling OTA calls.
- Select P4 revisions <3.0 explicitly. Setting only REV_MIN_100 is insufficient
  with IDF6: it otherwise selects the incompatible >=3.x hardware path.

The allocator host test extracts the actual external source function:
`python3 test-allocator.py <patched-host>/host/port/os_wrapper.h`.
Host stubs cannot verify actual DMA capability, silicon cache behavior or WLAN.

Still required before any deployment: absent-reset GPIO guard, bounded
startup/connection failure handling, safe constructor/task error propagation,
explicit board identity and compatible image/layout verification, scanout
quiesce, fresh exact-span backup, independent readback and baseline restoration.
The current front end only calls Wi-Fi initialization; MAC/scan/association,
frame transfer and recovery gates must be added before reference acceptance.
