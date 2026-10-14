# AROS remote control: USB diagnostic-MVP investigation

Implementation follow-up2026-10-14: Fabian explicitly requested preparing
screenshot/mouse/read-only backends before the later board test. The isolated
echo and separate USBDiag candidates are now host/native-build verified,
not installed or hardware qualified. See
[RemoteDiag implementation/limits](../../workbench/tools/RemoteDiag/README.md)
and ROADMAP RC0. This supersedes the preparation order suggested below, not
the transport/hardware acceptance prerequisites. The remainder records the
original source investigation, not a claim that implementation is still absent.

2026-10-14. Source investigation, not a completed transport or hardware gate.
Fabian authorized this investigation and the diagnostic goal: screenshots,
mouse movement/click and read-only file access independent of WiFi.

Tracking: [RC epic #77](https://github.com/metaneutrons/AROS-NX/issues/77),
[USB RC-M4 #81](https://github.com/metaneutrons/AROS-NX/issues/81),
[file RC-M9 #87](https://github.com/metaneutrons/AROS-NX/issues/87).
The epic's RC-v5 requirements remain the acceptance authority until the first
reviewed design PR snapshots the complete plan. This report does not replace
that plan or lower the original input/capture or full-feature gates.

## Decision and current limits

Proceed to an isolated USB enumeration/echo spike, not yet the remote service.
Use the separate P4 High-Speed OTG controller as the transport candidate;
preserve the existing USB Serial/JTAG console and ROM flashing path. Do not
send captures/files through the intentionally lossy debug queue or let a
second reader consume console input. No console-multiplexer fallback has been
accepted. No AROS gadget driver was found in the audited sources; integration
work is required, not just enabling the existing CDC host class.

No ESP32 target was present in the Mac's IOUSBHostDevice inventory during
the 2026-10-14 investigation. No serial port was opened, reset,
written or flashed. No GUI operation, cable change or network retry occurred.
Fabian has left the display at home; fresh physical/visual readiness is absent.
There is no enumeration, echo, throughput, screenshot or input acceptance.

## Evidence and provenance

Audited AROS tree: `feature/esp32p4-wifi`, starting revision
`d548a8ac9598f1cb71d946a23f085851947d06bf`. Two bounded, read-only Luna audits
covered AROS USB host/device inventories and local P4/IDF reference sources;
the primary checked the decisive source locations and rendered the schematic.

Vendor schematic: `JC1060P470C_I_W_Y-V1.0.pdf`, sheet 1, USB connector blocks,
SHA-256 `ae3f41fe9a66b201358ef8cf1f531fdced68c63f84c8bde36e23cde5132a23ee`.
The exported Altium PDF shows:

- USB2, "Full Speed USB": `USB1P1_N/P` nets.
- USB3, "High speed USB": `ESP_USB_N/P` nets.
- Both are Type-C connectors, with 5.1-kohm CC pull-downs and common
  `USB5V_IN`. USB3 is a schematic reference, not a USB 3.x speed claim.

This is schematic evidence, not installed-board connector identity or
electrical qualification. The module's internal mapping, actual assembly,
cable orientation, VBUS behavior and independent console coexistence must
be confirmed on the identified device before a hardware pass. Do not assume
the previously used socket is the HS connector.

Local reference IDF is 6.0.1, revision
`8c19b156084a0753687347cca1f5355782893533`, under the existing ESP32 build's
`Ports/esp-idf/esp-idf-v6.0.1`. Its bootloader loader/utility and
`esp_hw_support/CMakeLists.txt` have preexisting changes; none was edited.

- `components/esp_hal_usb/esp32p4/usb_dwc_periph.c:31-56`: FS OTG uses
  GPIO27/26; HS uses dedicated UTMI pins and `ETS_USB_OTG_INTR_SOURCE`.
- `components/esp_hal_usb/esp32p4/include/hal/usb_serial_jtag_ll.h:212-238`:
  separate FSLS PHY mapping for USJ and FS OTG. USJ on GPIO24/25 is the
  project's existing console contract; FS OTG defaults are not that pair.
- `components/esp_hal_usb/esp32p4/include/hal/usb_dwc_ll.h:173-184` exposes
  DMA and slave/PIO modes.
- The CDC example manifest requests `espressif/esp_tinyusb` `^2.0.1~1`;
  its application uses FreeRTOS. A dependency range is not an exact resolved
  stack identity. No local TinyUSB checkout was identified in the searched
  source/vendor/cache directories.

The primary therefore inspected upstream TinyUSB at immutable revision
`4c195b7b94f813a37b77b184bcae7b2b6ef2835e` through GitHub, not a locally
compiled dependency. It is a source candidate, not a selected release:

- [DWC2 Espressif adapter](https://github.com/hathach/tinyusb/blob/4c195b7b94f813a37b77b184bcae7b2b6ef2835e/src/portable/synopsys/dwc2/dwc2_esp32.h):
  explicit P4 controller table, FreeRTOS headers/delay, `esp_intr_alloc`,
  IDF-managed PHY hooks and DMA cache callbacks. Its rhport0 is FS and
  rhport1 is HS, the reverse of IDF's controller-array order. Translate
  explicitly; a literal port0 is not a safe HS selection.
- [Device controller](https://github.com/hathach/tinyusb/blob/4c195b7b94f813a37b77b184bcae7b2b6ef2835e/src/portable/synopsys/dwc2/dcd_dwc2.c):
  separate slave/FIFO and DMA paths. PIO can be selected in this candidate;
  native HS enumeration/performance is untested.
- [OSAL](https://github.com/hathach/tinyusb/blob/4c195b7b94f813a37b77b184bcae7b2b6ef2835e/src/osal/osal.h)
  provides `OPT_OS_CUSTOM`; `osal_none.h` explicitly excludes true SMP and
  multithreaded use. Merely setting OS-none is not an AROS SMP port.
- MIT licensing is present in `LICENSE` and these source headers. Retain
  notices and independently review the exact subset/dependency licenses when
  vendoring; this inspection does not authorize copying an IDF application
  wholesale or claim license closure for a future build.

Official reference:
[Espressif P4 device stack](https://docs.espressif.com/projects/esp-usb/en/latest/esp32p4/usb_device.html)
and [fixed-function Serial/JTAG console](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-guides/usb-serial-jtag-console.html).
Host has Homebrew libusb 1.0.30 installed; no host device was claimed.

## AROS reuse and integration gaps

The scoped search covered `rom/usb`, `workbench/devs/USB`, P4 kernel/board/boot
and USB headers/build rules, including TinyUSB, DWC2, OTG, gadget and device-mode
names. The result is limited to this audited tree:

- P4 `kernel/hardware.h:76-94`, `kernel_console.c:213-225` and
  `maygetchar.c:31-36` implement the fixed console FIFO, not a configurable
  USB gadget or private RC endpoint.
- Poseidon CDC-ACM binds attached peripherals in
  `rom/usb/classes/cdcacm/cdcacm.class.c:329-340`; HIDD control/bulk pipes in
  `workbench/devs/USB/include/usb.h:398-429` are host-side. Neither is a P4
  device function. Broadcom's `usb2otg_core.c:83-86` forces host mode.
- P4 `boot/mmakefile.src:127-135` has no USB gadget/controller in the BSP.
- P4 `kernel/kernel_traps.c:289-318` dispatches timer, SMP IPI and DSI DMA
  directly; registering a generic IRQ handler alone does not add USB
  dispatch. Controller masking/acknowledgment and safe teardown need an
  actual adapter; do not unmask an unhandled level IRQ.
- P4 `exec/cpu_init.c:43-64` does not implement a general DMA receive
  invalidate through `CacheClearE`. Start with explicit PIO/FIFO selection;
  DMA requires a separately verified alignment/cache/ownership contract.
- ScreenGrabber `main.c:61,102,155` and
  `workbench/libs/cgfx/readpixelarray.c:29-88` demonstrate generic
  `LockPubScreen`/rectangular `ReadPixelArray`. The selected build already
  has input.device, intuition, dos and cybergraphics artifacts. That is
  build inventory, not capture/remote-input proof. LockPubScreen prevents
  screen disposal; it is not an atomic redraw snapshot.
- `compiler/include/devices/inputevent.h:79,90,159-166` provides screen-bound
  pixel coordinates. `rom/devs/input/processevents.c:397-417` processes
  `IND_WRITEEVENT` and severs the event chain: submit owned individual events
  and wait for completion before reuse. Local/remote button overlap still
  needs the RC-M1 arbitration contract.

## Proposed spike and diagnostic slice

Keep hardware/PHY/clock/interrupt knowledge in a P4 USB adapter and generic
commands in an ordinary after-DOS AROS service. Do not add IDF/FreeRTOS
compatibility shims across the generic service. Select a pinned TinyUSB subset
only after reviewing its integration surface. Supply a real AROS OSAL or a
proved single-owner execution model; ISR/task/core exclusion must be tested.
One USB worker and bounded message queues separate service jobs from endpoint
callbacks; no DOS, graphics capture, allocation-heavy work or blocking output
in an ISR or while holding a scheduling-sensitive lock.

Transport candidate: one vendor-specific bulk IN/OUT interface on HS OTG,
accessed by a libusb host adapter. This avoids turning a text-oriented serial
capture into a binary protocol client. CDC-ACM remains an alternative, not a
fallback already accepted; its host ownership/binary capture must be resolved
with devserial if selected. Allocate a legitimate development VID/PID policy;
do not impersonate the fixed `303a:1001` debug device. Framing, request/session
identity, length limits, integrity checks and terminal results remain RC-M1
design work, not permission to expose arbitrary commands.

Diagnostic service slice after transport qualification:

1. Status/capabilities and full/cropped RGB capture of an explicitly selected
   public screen. Snapshot before transfer, release screen locks before USB
   waits, validate ReadPixelArray's returned pixel count. A 1024x600 RGB888
   frame is 1,843,200 bytes; this is arithmetic, not measured throughput.
   Define bounded allocation and consistency/cursor limits. If a snapshot
   cannot be obtained within the chosen budget, fail rather than hold
   graphics locks across transport backpressure.
2. Screen-bound pointer movement and completed button down/up/click jobs.
   Default-off, explicit input lease, release only remote-owned input on
   cancel/disconnect. No touch-contact, keyboard or full RC-M6 claim from
   this smaller diagnostic slice.
3. Listing/stat and ranged/full file download using normal DOS read APIs,
   configured roots and resolved-path confinement. Include native protection
   mask, comments and DateStamp metadata on reads; no protection override,
   upload, delete, move, patch or metadata mutation. ENV: can contain secrets:
   transfer only explicitly authorized paths and never print content into
   debug/evidence logs. Detect source modification across chunks/resume.

The host CLI owns the same reusable client later used by MCP. MCP/TCP/TLS and
file writes do not belong in this first USB spike. A read grant does not
implicitly grant input control or access outside configured roots.

## Next decision gate (not yet passed)

First compile and test bounded transport queues/descriptor/framing fixtures
without hardware; then qualify native enumeration and deterministic echo on
the identified HS connection. Test partial transfers, queue-full/slow/no
reader, cancel, unplug/reconnect and stale session rejection; compare exact
bytes/digests in both directions and inspect watchdog/boot/debug continuity.
Measure throughput before promising screenshot latency. No MSC exposure of
SYS:, no flash-layout changes, no firmware update or WLAN policy changes.

Physical connection changes and visible input/capture comparisons require
fresh Fabian readiness. If enumeration cannot be achieved without taking over
the debug PHY or changing the board wiring, stop and review that outcome;
do not silently switch to console sharing. Only after this transport gate
should screenshot/mouse/read-only backend integration proceed. Builds and
source inventories cannot close RC-M4 or the diagnostic MVP.
