# HS CDC reference measurement (not AROS firmware)

Development-only ESP32-P4 reference from ESP-IDF6.0.1 commit
8c19b156084a0753687347cca1f5355782893533 and esp_tinyusb2.2.1. The official
`tusb_serial_device` source is included unchanged through a wrapper. Optional
persistence instruments install and checked outcomes, retaining normal aborts.
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

## Full-speed comparison on the same HS connector (diagnostic, default OFF)

`HS_REFERENCE_FORCE_FS_HSPHY=ON` limits DCFG.DEVSPD to Full-Speed on the HS
PHY (encoding1), not dedicated-FS-PHY encoding3. Keep the high-speed compile
capability, port1/HS DWC, UTMI init, DMA and descriptors unchanged; disabling
TUD_OPT_HIGH_SPEED would select phy_fs_init and is NOT this experiment.
The option requires persistent link trace and forbids injected install errors.
The sample labels its configured maximum as FS_HSPHY or HS; DSTS remains the
actual observed speed, not a successful-enumeration assertion.

An out-of-tree generated copy changes one device-speed assignment in the
hash-pinned TinyUSB DCD, before its initial connect. No detach/reconnect patch,
managed-dependency edit, clock/reset/termination tuning or production AROS
change is intended. The original official example remains included unchanged.
The normal OFF build uses the original DCD. A successful FS result would only
isolate the speed/negotiation path; it would not qualify High-Speed or justify
shipping a permanent12Mbit/s workaround.

Fabian opts to leave Debug/FS and HS connected simultaneously to the same Mac
on2026-10-16 to avoid repeated cable moves. This is a different two-cable
procedure, not a sole-power interruption or cold boot. Shared VBUS topology and
per-port power protection are not electrically qualified by that choice. Record
the actual setup, capture on Debug/FS and compare both speed modes under the
same setup; never merge those results into the previous single-cable cold gate.
Do not run a standard FS app boot before reading any preceding single-cable
journal: it can create the first trace if the preceding HS boot never did.

## Optional single-cable diagnostic journal (default OFF)

`HS_REFERENCE_EP0_TRACE=ON` requires progress mode and adds two compact
read-only snapshots: the first successfully queued reset-end event (R), and
the late sample6 state (L). N means no reset snapshot was published. Counters
on both lines are sampled at report time, not at the first reset. Publication
is once-only and lock-free; the callback performs no flash or console I/O.
Registers are individually observed, not a coherent bus transaction history.
G/D/S/I/M are GUSBCFG/DCFG/DSTS/GINTSTS/GINTMSK; O/Z/E are EP0 OUT
control/size/interrupt; R/F are non-pop GRXSTSR and GRXFSIZ. K low16 is
DOEPMSK, bits16/17 EP0 OUT pending/mask and bits18/19 EP0 IN pending/mask.
GRXSTSP and data FIFOs are never read. Three successful check messages are
omitted, not the checks or errors, keeping the maximum at14 of16 trace slots.
An already drained SETUP cannot be reconstructed from these snapshots.
The default OFF build retains its previous behavior. This is instrumentation,
not an endpoint-arming fix or a native USB acceptance result.

`HS_REFERENCE_RECONNECT=ON` is a separate diagnostic, requiring persistent
link trace, normal HS speed and no injected error. At the first five-second
observer tick it skips permanently if mounted/connected or any queued bus
activity was seen; otherwise it calls public `tud_disconnect()`, records DCTL
and USBCTRL, waits1000ms, calls `tud_connect()` and records again. Exactly one
attempt, no automatic retry or PHY/speed/reset tuning. The default OFF build
adds no detach/attach stimulus. Both builds now record raw USBCTRL before and
after install to distinguish inherited reset state from installer changes.

The activity guard is best effort, not atomic with the DCD ISR; use only for
the idle failing reference. API returns are unconditional true in the pinned
TinyUSB, not host acknowledgements. Observe DCTL.SDIS and host enumeration;
zero UNPLUG counters do not disprove soft detach. This adds two trace slots:
normal bound14 plus2 is16, with no spare slot. The skipped path adds one.
No USB/CDC pass follows from a complete journal. Compare identical reset and
cable setup; cold, button and Debug resets remain distinct observations.

Host helper test `tests/test_link_reconnect.c` covers all four API outcome
pairs, ordering, one-second request, once-only behavior and invalid inputs.
Run strict C11 O0/O2 ASan/UBSan, including NDEBUG; assertions must not erase
test actions. The runtime stimulus is diagnostic-only, not a production fix.

`HS_REFERENCE_PROGRESS=ON` further scopes the experiment to control-flow
diagnosis. It requires reconnectON (therefore link+persistenceON, forcedFS/
injectionOFF) and exactly one CDC interface. It stores observer_enter before
the first delay, then pure pre/post checkpoints around both public USB calls,
with timestamps/API result/stack high-water mark and no diagnostic USB MMIO
ahead of these markers. A missing return bounds the API/reset/return-append
region, not proof of a hung API. Observer-entry and observer-create order can
invert across cores; the trace mutex serializes writes, not task scheduling.

Only a fresh successfully written observer-entry marker permits the stimulus.
If the pre-detach marker fails, no disconnect or connect MMIO is attempted.
Once disconnect was actually called, connect is attempted even if later trace
writes fail, to restore the link. Preserved old prefixes therefore cannot
cause unrecorded repeat detaches on later recovery boots in this mode.

Progress mode stores a compact one-chunk sample6 plus its terminal marker,
not the full PHY reference snapshot. Shared HS_TRACE_SAMPLE_CHUNK bounds it
to180 bytes, tested with maximum-width values. Nine initial records + observer
entry + four call boundaries + one sample + terminal =16; no spare slots.
Stack/USB progress from a preserved prefix does not prove later boot history;
flash writes still disturb timing and RC0-TW remains a diagnostic workaround.

`HS_REFERENCE_LINK_TRACE=ON` adds read-only UTMI FC00..FC07 snapshots before
and after installer execution and queued TinyUSB event counters to sample6.
The option defaults OFF; the original example/PHY/DWC configuration is not
modified. FC reads are skipped unless HP bus and LP reference clocks are on
and both reset bits are clear. No FIFO reads or W1C writes are added. The
weak event hook is replaced only in this opt-in build: it increments lock-free
32-bit atomic counters, without flash/log/OS calls from interrupt context.
Snapshots are independent relaxed loads, not one coherent time point; counts
wrap modulo32 bits. Counts describe successfully queued stack events, not all
physical bus packets or raw IRQs. SOF interrupts are not additionally enabled.
Port-filtered events (including TinyUSB's port0 deferred functions) are reported
as filtered, never as missing bus events. DSTS line/speed/suspend/frame fields
are raw source-defined observations, not electrical-health proof. In particular
Espressif documents that HS cannot monitor VBUS automatically; GOTGCTL BSESVLD0
alone is not evidence of absent VBUS or the cause of failed enumeration.

Normal staged+link capture uses at most14 of16 slots for the bounded formatter
output (13 observed in the FS control). No new storage/layout exception. Native
FS control of197728B2d612c32... saves pre-install clock-off refusal to read FC,
post-install FC values and zero queued bus events with HS unattached, followed
by complete sample6. This verifies instrumentation only. The HS run must still
use fresh scratch and BOOT-held ROM-only FS retrieval; it has not been run with
this enhanced artifact. AROS is restored afterward, not left as the test app.

The JC1060 vendor tree at `~/Downloads/JC1060P470C_I_W_Y` contains generic
`bsp_usb_host_start()` implementations but no invoked HS USB-device startup
in the inspected IDF/Arduino demos. Those host helpers are not a known-good
CDC reference. The unmodified official CDC example remains the comparison.

`HS_REFERENCE_PERSIST=ON` now selects a staged HST2 trace, superseding the late
HSJ1 snapshot. It begins in app_main before queue creation or USB initialization.
It records app entry/reset reason, USB install entry/return/error, observer
creation, checked example outcomes, observer progress and sample6 register
chunks. This is NOT a read-only-flash reference: at most one4096-byte erase
and sixteen256-byte writes at app-relative0x37000, absolute0x57000, are
permitted during a fresh capture. It is restricted to
JC1060 MAC80:f1:b2:d3:3b:a6 and the existing ota_0 at0x20000/0x800000.
The complete original sector must already be retained for exact restoration;
pass its CRC32 as `HS_REFERENCE_PREIMAGE_CRC32=0x85539a44` only for the
documented A6knoc preimage, not as a reusable board constant.

The unsigned image including checksum padding and SHA must end before that
sector. Signed-image configurations fail compilation; hardware secure boot,
encrypted app partitions, unknown/partial sector contents and image overlap
refuse a fresh capture. A valid old HST2 or HSJ1 record is preserved, never
appended to on a later boot. Failed/uncertain operations disable further writes.
CRC is corruption checking, not authentication. Unlike HSJ1, Debug/FS presence
does not deny a capture: link status is stored as data; preserved earlier
records prevent replacement on an FS return boot. The SOF monitor initially
assumes connection, so an early flag is not cable proof. Synchronize physical
runs and never re-label a preserved FS trace as HS data.

Each256-byte slot contains HST2, LE32 sequence/length/reserved0, up to236 ASCII
bytes with FF padding, then CRC32 over its first252 bytes. Only contiguous
verified slots are valid. Decoder reports a torn tail as failure while showing
the valid prefix. A torn first write/erase can leave no usable record. Refusal
before the first safe write is console-only: untrusted/overlapping flash cannot
safely receive a refusal record. Allocation/partition/MAC errors are also
console-only. This is not a general panic handler and cannot record ROM,
bootloader or pre-app_main failures. Last completed stage narrows a stall;
it does not automatically identify its cause. Flash disturbs runtime timing.

Build the staged candidate with `sdkconfig.defaults;sdkconfig.trace` as ordered
SDKCONFIG_DEFAULTS in a fresh configuration. CMake requires IDF's size profile
with assertions retained; the original debug profile produced an oversized
candidate and must not be flashed across the scratch boundary. This optimization
change is an additional difference from the earlier IDF comparison, not an AROS
or USB fix. The normal configuration must have
`HS_REFERENCE_INJECT_INSTALL_ERROR=OFF`. A separate explicit ON build is a
headless self-test: it deliberately returns ESP_ERR_INVALID_STATE instead of
calling the USB installer, records usb_install_injected/return and example_error,
then records injected_error_park and waits with the scheduler running. Only this
explicit injected self-test parks instead of aborting, avoiding a recovery-
hostile reboot loop. Normal builds retain IDF's configured error-check behavior.
This is not a panic-handler test or an actual USB controller failure.

Decoder labels a clean partial trace `valid_prefix_only`; a verified terminal
marker labels `sample6_complete`. Neither means USB enumeration or boot passed.

IDF protects the whole running app partition, not only the linked image. The
first native HST2 control exposed an abort on the scratch-sector erase before
USB. The diagnostic now uses a narrowly scoped linker wrapper for
esp_partition_main_flash_region_safe: only while the preflight-checked adapter
is synchronously erasing exactly0x57000/4096 or writing an aligned256B record
inside0x57000..0x57fff can that operation pass the app-partition veto. All other
requests delegate to the real SDK decision. Its separate read-only-partition
check, cache/lock handling and dangerous-write protection stay enabled. No
CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED and no global protection toggle.
This is a development-only scoped workaround, not a production flash API.
Native Debug/FS tests now verify the normal10-record complete trace, byte-exact
preservation across a reboot and a separate6-record controlled error trace.
Full original AROS image/scratch is restored and readback verified afterward.
This verifies the diagnostic storage workaround only, not HS enumeration.
The exception is additionally bound to the synchronous caller task; interrupt
context returns no owner. A competing task cannot borrow or clear the token.
The first global operation-only draft failed independent review on that point
and was corrected before flashing. Task/ISR isolation tests pass. Current
corrected candidate196688B1bc7eef7... is the native-tested normal artifact,
retained as normal-window-owner.bin. The mutable build directory was returned
to injectionOFF; do not substitute another hash for the retained tested image.
Removal gate RC0-TW stays open: move storage to an explicitly provisioned data
partition under separate layout authorization and repeat native write/readback/
preservation controls without this wrapper. No layout mutation is authorized
by the present tests.

Before deployment identify the
board/table again, verify the full live scratch preimage and preserve app erase
tails. Never write generated metadata. For an approved HS run wait at least35
seconds, then hold BOOT before reconnecting Debug/FS power, enter ROM without
an intervening application boot, and retain/read the sector before any overwrite. An
unrestricted FS return can create the first trace if HS never did; preservation
of existing captures alone does not prove connector provenance. The first
staged HS run recovered a complete trace but remains ambiguous for this reason.
The corrected BOOT-held/no-reset ROM retrieval recovers10 valid records proving
HS app entry, successful install and sample6/mounted0; host enumeration still
fails. It does not establish PHY health or successful CDC transfers.
Then
decode the saved file with `python3 decode-journal.py SECTOR`. Restore the
entire original AROS0x38000-byte span, including the journal sector, and verify
it independently. No valid record is a failed/inconclusive measurement, not a
reason for an automatic retry. Registers are sampled before their append, but
earlier progress writes precede USB setup: not an undisturbed reference.

Offline checks: compile `main/journal.c` and `tests/test_journal.c` with the
`main` include directory, C11, `-Wall -Wextra -Werror`, O0/O2 and ASan/UBSan;
run both executables. Run `python3 decode-journal.py --self-test` separately.
Also compile journal.c, trace.c and tests/test_trace.c with strict O0/O2
sanitizer settings. These checks do not establish HS enumeration or native
flash-write safety.

Historical HSJ1 limit: observer creation follows successful tinyusb_driver_install.
The official example aborts on install errors; none of those paths can reach
sample6. An unchanged scratch sector therefore does not establish that app_main
was never reached; that journal did not persist refusal reasons. HST2 moves
the first record before USB setup and records checked install failures.
The35s cable wait is not a deadline measured from observer creation. The exact
journal binary reaches sample6 at30220ms in the FS control, with result=-1
(expected with an FS host). There is still no HS runtime result. For a commanded
Debug/FS boot, do not rely on the generic DTR-only devserial reset macro:
set DTR=false, pulse RTS=true for200ms, then RTS=false with capture open.
Keep this control-line correction separate from the unresolved HS cold-start.
