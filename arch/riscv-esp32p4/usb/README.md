# Opt-in ESP32-P4 USB echo experiment

Status: host-tested and native-build verified; **not hardware qualified**.
This is a standalone AROS DOS application, not a resident, BSP member or
remote-control service. It is never automatically installed or started.
The default build remains echo-only. The separately selected `diag` build now
prepares screenshot, mouse/click and read-only file backends, ahead of hardware
qualification at Fabian's request; this is not a tested remote service.
See [RemoteDiag](../../../workbench/tools/RemoteDiag/README.md).

## Dependencies and offline verification

The build stages only TinyUSB's MIT `LICENSE` and `src` from commit
`4c195b7b94f813a37b77b184bcae7b2b6ef2835e`. Obtain its immutable archive from
[GitHub](https://codeload.github.com/hathach/tinyusb/tar.gz/4c195b7b94f813a37b77b184bcae7b2b6ef2835e).
Required archive SHA-256:
`f42c184ca988bb1f2e71a9f3f88c544e21752602331de981883d7bf9c0fc6063`.
No floating fetch, IDF/FreeRTOS runtime or upstream agent instructions are used.
The stager verifies the archive, rejects unsafe paths and fails closed when an
integration patch no longer matches. Upstream sources/license remain in the
output directory; retain the license when distributing a binary.

From the repository root, with an already downloaded archive and **absent**
output directories:

```sh
sh arch/riscv-esp32p4/usb/run-tests.sh PINNED_ARCHIVE NEW_HOST_OUTPUT
sh arch/riscv-esp32p4/usb/build-prototype.sh AROS_BUILD PINNED_ARCHIVE NEW_NATIVE_OUTPUT
```

Pass a fourth argument `diag` to the build script for standalone `USBDiag`.
It requires explicit NONEAUTH and a named SCREEN, optional ROOT and separately
ALLOWMOUSE/UNATTENDED. No current image, normal startup or package changes.
Diagnostic worker limits and its RDG1 host CLI are documented in RemoteDiag;
native backend calls are not hard-time-bounded, unlike the controller polls.

The tests open no USB or serial device. Nine Python staging tests, five host
probe tests and five C suites at O0/O2 with ASan/UBSan cover ring bounds,
partial transfers, FS/HS descriptors, owner-only OSAL/FIFO, PHY readback and
restoration, failure budgets and main lifecycle/quarantine. Platform, time,
controller and MMIO in the main fixture are fakes; these tests do not execute
the real DWC2 controller or prove AROS scheduling/USB enumeration.

The native script compiles every object fresh with warnings as errors and
the configured `riscv-aros-gcc`. It explicitly selects this build's generated
headers and Developer SDK, even if the compiler's default sysroot is stale.
The output is an ELF32 AROS RV32 single-float DOS executable. It is **not** a
kernel module: `boot/audit-package.py`'s kernel section-placement policy is
inapplicable. DOS LoadSeg loads nonempty SHF_ALLOC sections, including
`.eh_frame`. A successful link is not an on-target LoadSeg/startup test.

## Transport and ownership limits

- HS OTG rhport1, dedicated UTMI pins; no FS USB_WRAP access, USJ console
  sharing, debug-writer transport, driver detachment or hardware IRQ routing.
- One owning AROS task polls DWC2 PIO and runs TinyUSB. The custom OSAL is
  specifically for that single-owner/polled arrangement, not a general
  multi-task or ISR-safe SMP adapter. DMA is disabled.
- Vendor interface with OUT01/IN81; FS64/HS512-byte packets and a 4096-byte
  bounded echo ring. Backpressure stops reads rather than overwriting data.
  Bus-reset/unplug callbacks discard queued echo; physical disconnect and
  reconnect behavior still require hardware measurement.
- Native MMIO waits have a 100 ms/100000-poll guard. Lifetime is 1..600 s,
  default120; Ctrl-C requests stop. Native timer/scheduler behavior is untested.
- Both harts' HS source94/95 routes must be unused. A named permanent owner
  record prevents a second copy. PHY fields are restored on normal exit.
  Cleanup retries at most16 times/500 ms. If restoration cannot complete,
  the record and snapshot remain quarantined until reboot, without pointers
  into unloaded program code; later copies refuse to start. This is a deliberate
  fault quarantine, not successful cleanup or automatic recovery.
- The current ABI8 SDMMC host semaphore excludes runtime SDMMC/radio RCC
  writers. It is **not** a universal RCC lock: display/GDMA reinitialization
  is outside it. Only a steady ordinary desktop, with no concurrent display,
  stress or reinitialization runner, is eligible for this experiment. A generic
  remote service must resolve the shared-RCC contract first.

## Future hardware run — not performed by the offline scripts

Review the actual connector, board revision, cable and power arrangement
first. JC1060's schematic USB3 means the High-Speed connector, not USB3.x.
USB2/USB3 share USB5V_IN; do not assume two powered host cables are safe.
The descriptor declares100 mA bus power provisionally; real board draw and
VBUS/detach detection are not qualified. That descriptor is not a power audit.

Use an authorized laboratory VID/PID, never a borrowed production identity.
There are no default IDs; debug303a:1001 is explicitly rejected. AROS `/N`
arguments are decimal:

```text
USBEcho ENABLE VID <authorized-decimal-VID> PID <authorized-decimal-PID> SECONDS 120 STEADYCLOCKS
```

The host probe requires PyUSB/libusb and the exact inspected bus/address:

```sh
python3 arch/riscv-esp32p4/usb/echo_probe.py --vid <VID> --pid <PID> --bus <BUS> --address <ADDRESS>
```

It checks product/interface/endpoints before claiming the selected interface,
does not reset, detach a driver or change configuration, and never replays an
ambiguous failed write. Its bounded byte-exact echo is not a throughput or RC
session/authentication test. Actual enumeration/control requests, transfer,
slow/no reader, disconnect/reconnect, watchdog/debug/WLAN coexistence and
power behavior remain required before generic diagnostic-service integration.
