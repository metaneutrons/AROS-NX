# Complete Hosted reference: bounded MAC/scan candidate

Baseline headless boot has recovered by a manual parent-clock workaround. The first
400MHz candidate failed before app entry; its exact AROS rollback matches flash
but initially faults in ROM. A single documented240MHz parent-clock enable
restores AROS; N1R requires uncompensated handoff/early-failure return tests.
The corrected360MHz candidate now passes one MAC/scan hardware run (13 APs,
eight records) and an exact AROS rollback/headless return without another
clock write. This does not close N1R early-failure or native WLAN gates.
Never execute the generated `idf.py flash` or
`flash_args`: they use a different partition layout and would overwrite AROS
bootloader/partition state. Passing compilation does not verify Wi-Fi.

See `../../network/HOSTED-REFERENCE.md` for archive hashes, external workspace,
dependency revisions, hardware safety gates and evidence.

Build with IDF6.0.1 and the pinned external components. From a fresh verified
Hosted d518 archive (including its exact protobuf-c gitlink), apply the retained
compatibility patch with `git apply --check`, then `git apply`. Next apply
`bounded-reference.patch`, then `scan-safety.patch`, checking each first.
Do not apply it to another upstream revision or already patched source.

Set `HOSTED_REFERENCE_ROOT` to the external directory containing
`esp-hosted-d5184217f710d54c01f932e75b4c55e3f4933fc9` and
`remote-0.2.3/esp_wifi_remote`. Set IDF_PATH and the matching GCC/Python paths,
IDF_PYTHON_ENV_PATH, ESP_IDF_VERSION=6.0.1 and IDF_COMPONENT_MANAGER=0.
Use an external build directory and SDKCONFIG path to keep generated files
outside the repository. Run `idf.py -B <build> -D SDKCONFIG=<config> build`.
For a fresh configuration, SDKCONFIG_DEFAULTS must point to this directory's
sdkconfig.defaults. Run `python3 check-config.py --selftest`, then
`python3 check-config.py <generated-sdkconfig>`; verify the final image header
separately. Revision1.x requires360MHz, not the generated400MHz default.

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

The bounded patch removes implicit constructor/destructor initialization,
guards absent reset GPIO, replaces repeated reset attempts with a ten-second
connection deadline, propagates connection failure, and fail-stops on SDIO
card-init, OPEN or transport-worker creation failures. It preserves the bus
mutex created before workers instead of replacing that live lock afterwards.
The application starts
its forty-second aggregate timer before explicit host initialization; panic
prints and halts rather than automatically rebooting. These are cooperative
timing/error policies, not proof of a hard bound during a hardware/scheduler
stall. Persistent transport workers still run after application success.

The scan patch defaults malformed RPC responses to decode failure, validates
MAC length, scan counts/repeated records and field pointers/byte lengths before
allocation/copy, and enforces caller capacity while publishing returned count.
The app avoids the pinned slave's empty-list error by omitting get_records when
the successful scan reports zero APs. No slave firmware changes. The application
queries a unicast MAC, runs one scan, obtains at most eight records, stops Wi-Fi
and prints `REFERENCE result=ok scope=MAC_SCAN`. No credentials or SSIDs logged;
no association, OTA or Ethernet-frame test. This marker alone cannot qualify WLAN.

Tests extract actual external source, not replacement policies:
`python3 test-startup.py <patched-host>/host/drivers/transport/transport_drv.c`.
Heap/time/GPIO mocks are not silicon or installed-firmware evidence.

## Optional C6 lifecycle candidate (not deployed)

`slave-lifecycle.patch` is a separate C6-side patch for the exact pinned
`slave/main/slave_control.c`, original SHA256
`d6234a3479376a4464cac4eac1919f9bbcf6554d24896dd1b63fe4f925bc2619`.
Do not include it silently in the P4 reference build or apply it to the installed
unidentified C6 image. C6 deployment remains postponed and requires the
separately qualified recovery/update path.

Apply with `git apply --check` followed by `git apply` in a disposable verified
Hosted tree. It registers the WiFi handler once, before driver INIT, and keeps
that single handler for the persistent default event loop. Registration failure
returns an RPC error before driver init rather than panicking; a failed driver
init can retry without leaking another handler. DEINIT deliberately does not
remove the handler, so already-queued events still have a consumer. This assumes
the pinned single pserial request dispatcher and persistent default event loop;
it is not safe to reuse the helper for concurrent RPC dispatch or a destroyed
and recreated event loop without an explicit synchronization/reset contract.

This corrects duplicate notification production, not UID0 attempt provenance,
producer quiescence, event queue draining, or a negotiated-security contract.
STOP/DEINIT/INIT/START qualification remains separate. The source-extracted host
test is `python3 test-slave-lifecycle.py <patched-slave>/slave/main/slave_control.c`;
mocked IDF boundaries cannot qualify the actual C6 firmware or radio.

Actual-source parser/app tests are `python3 test-scan.py <rpc_wrap.c> <rpc_rsp.c>`
and `python3 test-app.py main/reference.c`. All three models
and independent runtime review pass; this did not detect the generated CPU
configuration error. Still required before further deployment: baseline boot
recovery, generated-configuration gate, explicit board
identity and compatible image/layout verification, scanout quiesce, fresh
exact-span backup, independent readback and baseline restoration. Complete
reference MAC/scan hardware acceptance and native AROS association, frame
transfer, shared-SD operation and recovery remain open.
