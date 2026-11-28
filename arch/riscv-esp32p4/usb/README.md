# Opt-in ESP32-P4 composite USB experiment

Status: host-tested and native-build verified; installed on JC1060 via FS for
explicit tests, **not hardware qualified**. Successful FS installation is not
HS recovery, USB transport or remote-backend acceptance.
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

The `diag` build also produces `USBRemoteTest`, a separately launched desktop
consent gate. For private in-house tests only it uses pid.codes1209:0001,
SCREEN Workbench, confined ROOT SYS:Prefs and explicit unattended mouse access.
This test identity is not globally unique and must never be redistributed or
used as a production default (https://pid.codes/1209/0001/). Deploy USBDiag and
USBRemoteTest together in FLASHDISK0P0:; a normal program icon may launch the
latter. Start requires confirmation, Cancel does not open USB; no autostart is
installed. It runs synchronously for the application's600-second loop and
writes stdout to volatile local RAM:USBDiag.log. A blocked native backend can
delay shutdown beyond that loop deadline. The USB file root does not expose
this RAM log. stderr goes to NIL:. The final FINISHED/FAILED requester reports
the actual DOS return or setup phase/IoErr plus one fixed status classification
from at most the last1024 log bytes. Known failure markers also fail a zero
return; unreadable, unknown or truncated-out markers are not success evidence.
No arbitrary file text is displayed and failed log creation never reads an old
run's log. There is no live progress display during the synchronous run.
Do not use local touch/mouse during a confirmed unattended run.

It additionally builds `USBEchoTest` from the same launcher with
P4_USB_LAUNCH_ECHO. Deploy the separately built USBEcho executable with it.
This mode exposes echo test bytes only, logs to RAM:USBEcho.log and grants no
screen, file or input access. Qualify enumeration/byte-exact echo first; do not
start USBRemoteTest as a substitute for that gate. Both launchers require Start
after a requester and use no boot/autostart changes. USBDiag emits one known
raw-console marker after claiming its tap so CDC logging can be distinguished
from descriptor-only enumeration; a reader must assert DTR. Overflow warnings
remain unconditional and early/pre-claim logs are still unavailable.

The tests open no USB or serial device. Nine Python staging tests, six host
probe tests, fourteen recovery tests and nine C suites at O0/O2 with ASan/UBSan cover ring bounds,
partial transfers, FS/HS descriptors, owner-only OSAL/FIFO, PHY readback and
restoration, failure budgets and main lifecycle/quarantine. Platform, time,
controller and MMIO in the main fixture are fakes; these tests do not execute
the real DWC2 controller or prove AROS scheduling/USB enumeration.
The composition suite links actual portable echo/CDC/RemoteDiag sources to
bounded fake endpoints and a single-slot worker model: blocked/no CDC reader,
partial writes, overflow, reset/reconnect, discarded CDC RX and stale replies.
It does not execute TinyUSB, DWC2 or the AROS scheduler. Launcher fixtures cover
fourteen diagnostic and thirteen echo cases at each optimization.

## Explicit legacy-core ROM recovery tool

`hs_recovery.py plan` is a pure-file operation. It validates a P4 image with
appended digest against an explicit operator-supplied SHA-256, checks a saved
partition table and constructs sector-preserving bytes in a new directory:

```sh
python3 arch/riscv-esp32p4/usb/hs_recovery.py plan --image CORE_BIN --sha256 EXPECTED_SHA256 --table TABLE_BIN --before EXACT_AFFECTED_SECTORS --flash-mib 16 --output NEW_PLAN_DIRECTORY
```

Only a checksum-valid legacy table with one unencrypted8MiB `ota_0` at0x20000
is supported (16/32MiB flash); A/B layouts and migration are refused. The hash
is operator authorization/identity, not a signed-release trust proof. This tool
does not update helpers/BSP, metadata, bootloader, C6 or eFuses.

The separate `flash --execute` mode is **unqualified hardware code**; do not
run it until fresh physical readiness and an identified actual HS ROM endpoint
exist. Its required arguments are `--image`, `--sha256`, a new `--output`, an
explicit `--port`, lowercase expected `--mac`, and absolute `--esptool` console
script path in a trusted esptool5.5.0 environment. The wrapper selects that
script's absolute Python interpreter and pins both installed/module versions.
It sets the reviewed `ESPLoader.WRITE_FLASH_ATTEMPTS` to1 before esptool starts,
preventing whole-write reconnect/reset retries; normal protocol block retries
are not disabled. Before/after reset modes remain `no-reset`.

Every command refuses a devserial-managed port; it never closes a port or
stops/clears the daemon. Explicit scoped release and exclusive operator use
must precede a future run. This is a point-in-time ownership check, not an
atomic lease excluding another flasher. Identity/security/USB-OTG/flash size,
live table, exact preimage and sector tail are checked. The table and affected
sectors are reread before the sole write; changes refuse it. Any post-write
ambiguity is WRITE_UNCERTAIN, never automatic retry/reset/rollback. Full
independent readback is mandatory for VERIFIED_READBACK; boot remains unproven.
USB-OTG identity alone does not prove physical HS connector or negotiated speed:
record those independently. All current recovery tests use files/mocks; no
new HS flash, device claim or reset was performed for this implementation.

The native script compiles every object fresh with warnings as errors and
the configured `riscv-aros-gcc`. It explicitly selects this build's generated
headers and Developer SDK, even if the compiler's default sysroot is stale.
The output is an ELF32 AROS RV32 single-float DOS executable. It is **not** a
kernel module: `boot/audit-package.py`'s kernel section-placement policy is
inapplicable. DOS LoadSeg loads nonempty SHF_ALLOC sections, including
`.eh_frame`. A successful link is not an on-target LoadSeg/startup test.

## Transport and ownership limits

The configuration is now composite: vendor interface0 retains OUT01/IN81;
CDC control1/data2 use notification82 and OUT03/IN83. CDC FIFOs are1024 bytes.
The host remote client validates the complete tree and claims only interface0;
the CDC interfaces remain available to the operating system and devserial.
This layout has not yet enumerated on hardware.

USBDiag normally requires the matching kernel's private ABI1
`esp32p4console.resource`. Its exclusive lease mirrors future console records
into an independent64-record ring, without consuming the debug queue or calling
application code from kernel producers. Producers never wait for a USB reader.
CDC sends original payload bytes, limited to256 bytes per polling iteration;
partial writes retain their suffix. Overflow warnings remain enabled regardless
of the normal console-prefix setting; records counts lifetime mirror losses,
including earlier leases, modulo32 bits, not losses attributable to one session.
Bus resets insert a continuity warning;
this is not a lossless boot-history replay. CDC RX is discarded, and CDC control
lines do not reset or flash the board. `NOLOGS` is an explicit lab-only fallback
for an older kernel. Echo mode exposes the same descriptors but does not lease
the console tap. CDC logging is available only while USBDiag runs after DOS,
not during ROM or early boot. Native-ROM flashing is a separate recovery path,
not an implemented firmware-update command in this application.

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
