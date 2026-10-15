# Permanent ESP32-P4 HS device service

Decision: 2026-10-15, Fabian. This is the USB diagnostic-MVP adapter slice
of [RC-M4](https://github.com/metaneutrons/AROS-NX/issues/81), tracked under
[Epic77](https://github.com/metaneutrons/AROS-NX/issues/77), with native
qualification in [RC-M6](https://github.com/metaneutrons/AROS-NX/issues/83).
It does not replace the remaining transport-neutral RC-v5 requirements.

## Outcome and boundaries

One permanent kernel resource owns the HS PHY, DWC2 controller and TinyUSB.
It starts automatically after DOS initialization and remains present without
the remote application. The separate USB-Serial/JTAG Debug/FS connector and
BOOT/RESET ROM path remain recovery. HS ROM recovery is not an MVP requirement;
normal firmware updates over HS are a separate, still-required capability.

The measured baseline is an opt-in polled PIO prototype. A confirmed HS cable
move boots the visible desktop, but the subsequent explicit echo launch did
not produce a device in IOKit/libusb snapshots. Permanent ownership does not
by itself fix or qualify that physical enumeration failure.

No host/HID mode, automatic Type-C role selection, USB Ethernet, DMA/interrupt
driver conversion, file mutation or always-enabled remote access is included.
The audited JC1060 schematic has fixed CC pull-downs and shared input VBUS;
host power/role capability must not be inferred from the P4 controller's OTG
capability. This service uses device mode only and never sources VBUS.

## Design

`esp32p4usb.resource` is registered by a priority7 COLDSTART resident linked
into the core, not a DOS executable. The minimal P4 device-controller stack
and CDC/bulk classes are linked there; generic remote backends, host/HID and
firmware policy are not. The owner is one Exec task started AFTERDOS;
no other task or ISR calls TinyUSB. Hardware waits are bounded; controller
sources94/95 must be unrouted on both CPUs. An existing diagnostic owner is
not stolen. A retained lease/snapshot quarantines failure until reboot.

CDC interfaces1/2 mirror logs independently of Remote-Bulk interface0.
CDC RX is discarded. DTR, line coding and CDC data never authorize reset,
flash writes, input injection or a remote session. No-reader retention is
finite; overflow warnings remain unconditional. The mirror begins when the
owner claims the tap after DOS, not at the first ROM byte. Debug/FS remains
the early-boot diagnostic path.

The private versioned port ABI contains task-bound bounded-copy operations,
not callbacks into a removable application or borrowed buffers. A separate
`USBRemote` process opens the generic RemoteDiag worker only after explicit
ENABLE/NONEAUTH and configured screen/root/input policy. It then claims the
bulk endpoint. Starting the port alone grants no screen, file or input access.
The fixed RX/TX queues total2048 bytes. Reset/detach immediately shuts new
session admission; owner cleanup discards queued traffic. Tokens never repeat;
counter exhaustion refuses new claims. A two-second unrenewed client lease
retires a crashed or stalled client without dereferencing its task pointer.
Ordinary shutdown revokes before waiting for the native worker. Already
delivered data or running native DOS/input operations cannot be retracted;
native handlers can delay worker shutdown. The client must reset its generic
protocol session before reattaching after USB reset/lease retirement.

`USBRemoteStart` is a touch-launchable consent gate for the separate process.
It grants read-only SYS:Prefs access, Workbench capture and explicitly
unattended mouse input for a nominal600-second loop (50 DOS ticks per second;
scheduling/processing delays and blocked handlers can extend shutdown). It does not start or
stop the permanent port, and it grants no firmware-update authority.

## Firmware boundary

The separate read-only device vendor control request is:
`bmRequestType=0xc0`, `bRequest=0x50`, `wValue=wIndex=0`, `wLength=16`.
Reply: magic `P4UP`, little-endian u16 version1, u16 status1 (`NO_BACKEND`),
followed by eight zero bytes. No mutation request is implemented; all other
vendor control requests stall. `RemoteDiag/client.py firmware-status` queries
this without sending any RDG1 operation or enabling the remote backend.

This is a truthful capability boundary, **not firmware-update support**.
Implementing writes depends on the existing
[P4 paired upgrade contract](esp32-p4-firmware-contract.md) and
[signed release contract](esp32-firmware-release-contract.md): publisher
verification, immutable matched core/BSP, live layout/ABI validation, qualified
inactive-slot writer, complete readback, crash-consistent selection and boot
confirmation/rollback. The current legacy board has no inactive pair. No raw
flash command, silent in-place fallback, bootloader/table/NVS rewrite or use
of remote-input consent as flash authorization is permitted.

## Acceptance criteria

- PHS-A1: complete native resident link and loader audit pass with exact
  identity. Normal boot starts one owner with no SD helper dependency. Startup
  failure leaves boot and Debug/FS usable. No second controller owner is admitted.
- PHS-A2: CDC works before remote launch and after remote exit. Verify DTR,
  no/slow reader, disconnect/reconnect, bounds and overflow reporting. Do not
  count descriptor presence or API counters as received log bytes.
- PHS-A3: with no service, bulk traffic cannot execute a backend. With a
  confirmed service, screenshot/crop, completed click and read-only file
  digest/native metadata pass. Wrong owner/token, reset, lease expiry and
  service exit reject stale traffic. No callbacks survive application unload.
- PHS-A4: firmware capability discovery works without remote grant and
  currently reports NO_BACKEND; mutation counterprobes are refused. Actual
  update acceptance additionally requires every P4-contract P1..P4 gate.
- PHS-A5: a complete candidate fits the detected BSP allocation, preserves
  the live helper volume and passes full package dependency/loader checks.
  Missing capacity must block deployment rather than overlap the volume.
- PHS-A6: identified JC1060 native HS enumeration, CDC/bulk transfer, boot,
  watchdog, desktop/touch/storage/WLAN coexistence and Debug/FS recovery pass.
  Physical tests require fresh user readiness. D1001 remains a separate gate.

## Build and qualification policy

The core integration is selected by `P4_USB_PORT=1`; ordinary packages stay unchanged
until native qualification. Build requires the pinned archive consumed by
`usb/prepare_stack.py` plus explicit authorized VID/PID. 1209:0001 is solely
the previously authorized private pid.codes laboratory identity, not a public
product allocation. Invalidate module objects when any flag changes. Do not
reuse a changed staged stack in place; use a fresh generated stage/build.

The existing SDMMC semaphore serializes radio/storage RCC writers, not every
display/GDMA writer of shared clock registers. AFTERDOS avoids normal initial
display setup, but concurrent display reinitialization/stress remains excluded
until a shared RCC ownership contract is implemented and qualified.

The same code also builds as an independent BSP module for loader auditing,
but do not install both forms. The measured module appended to the live BSP
exceeds its legacy allocation by185832 bytes; this failed packaging alternative
is not a deployable artifact. Linking into the core avoids the BSP ELF
relocation overhead and does not change the table or helper boundary.

Rollback is omission of the opt-in owner and restoration of the retained
known-good core through Debug/FS. No bootloader/table/SD/C6 mutation is needed.
Implementation, measured artifacts, failed builds and next gates are recorded
in `arch/riscv-esp32p4/ROADMAP.md`; GitHub issues own the execution summary.
