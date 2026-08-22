# ESP32-P4 roadmap to a graphical boot

This is the authoritative forward plan for the `riscv-esp32p4` port.  The
companion [README](README.md) records architecture, board facts and the
narrative of results already observed.  This file records what comes next,
in which order, and what evidence is required before a step changes state.

Last plan review: 2026-08-22.  Current hardware is a Seeed reTerminal D1001
with an ESP32-P4 revision 1.3 and a SanDisk `SN128` SDXC card.

## Documentation is part of the result

The rules in this section are mandatory for work on this port.

1. A change that starts, advances, blocks, invalidates or verifies a roadmap
   step must update this file in the same change.
2. Update the master table, the affected phase and the evidence log.  If the
   externally visible status changed, update the README status table and
   milestone text too.
3. `hardware verified` means that the acceptance gate was observed on the
   D1001.  A successful build, static audit or plausible register dump is not
   enough.
4. Record failures and corrections as evidence.  Do not silently replace an
   old claim, because knowing which approach failed is part of the bring-up.
5. Every hardware record must include the date, board/chip revision, build or
   artifact identity, test configuration, relevant UART evidence, safety
   impact, remaining risk and next safe step.
6. A diagnostic define is not part of the normal boot path.  If a gate uses
   one, record it and repeat the final gate without it before closing a phase.
7. Changes to geometry, rotation, pixel format, timing, transfer size or clock
   require the dependent bandwidth and safety assumptions here to be updated.

Use these states consistently:

| State | Meaning |
| :--- | :--- |
| `not started` | No implementation claim. Research may exist. |
| `in development` | Code or test infrastructure exists, but the phase gate has not passed. |
| `build verified` | The intended artifacts build and pass static checks only. |
| `hardware partial` | Some, but not all, acceptance points passed on the D1001. |
| `hardware verified` | Every required hardware and safety point passed and is recorded below. |
| `blocked` | The exact blocker and the experiment needed to remove it are recorded. |

No phase is `done` merely because code exists.  In this roadmap, completion
means `hardware verified` plus the documentation update above.

## Target and boundaries

The first graphical-boot gate, `GB0`, is reached when a cold reset follows the
normal boot path, mounts a host-prepared MicroSD card read-only as `SYS:`, runs
DOS startup, installs the native ESP32-P4 graphics HIDD and displays a stable,
correctly oriented `1280 x 800` Wanderer desktop.  UART diagnostics must remain
available.  No `P4_*_TEST` path may be required.

Touch is deliberately a separate gate after `GB0`.  M7 is complete when touch
moves and clicks the Wanderer pointer correctly, but lack of touch must never
prevent the graphical boot.

Until `GB0`, the following remain out of scope unless a preceding gate proves
they are necessary:

- SD writes, formatting or repair on the target;
- four-bit/high-speed SD mode beyond a separately measured read-only step;
- display composition, PPA acceleration and triple buffering;
- the second HP core, audio, camera, radio, networking and power management;
- loading a second pre-DOS PKG container from SD.

The boot-critical resident set stays in the internal `arosbsp` flash partition
for `GB0`.  MicroSD supplies `SYS:` and therefore ordinary commands, libraries,
classes, preferences, fonts and Wanderer after DOS is alive.  This avoids a
pre-DOS filesystem/loader cycle and still proves that code outside the core and
flash package is loaded from SD.

The on-target storage path remains read-only through `GB0`.  Images are created
and written on the host, and their hashes are compared before and after every
board test that could have reached the medium.

## Verified baseline

The following is the starting point, not future work:

- the patched ESP-IDF second stage boots the XIP core from `ota_0`;
- 32 MiB PSRAM works at 20 MHz and is owned by Exec after package reservation;
- preemption, the 100 Hz tick, `timer.device`, kernel.resource and Exec work;
- the flash PKG loader relocates four ELF32 modules into PSRAM;
- `sdcard.device` enumerates Unit 0 in one-bit/400 kHz read-only mode;
- CMD17, low LBAs and `NSCMD_TD_READ64` near the end of the card are verified;
- `partition.library` version 3 opens the root device and reads LBA 0 through
  `ReadPartitionDataQ()` with `io_Actual == 512`;
- the adapter rejects CMD18, all larger transfers and every media-mutating
  command before changing controller state.

The detailed measurements and card identity are in the README.  The first
evidence entry below pins the exact flashed artifacts.

## Dependency map

```text
 A1 CMD18 -> A2 tables -> A3 FAT image -> A4 DOS -> A5 SD proof ----\
                                                                     +-> C3 GB0 -> C4 touch
 B0 contract -> B1 PSRAM ---------\                                 /
                                    +-> B5 -> B6 -> C1 -> C2 -------/
 B0 contract -> B2 -> B3 -> B4 ---/
```

Storage and display work may proceed in parallel, but the arrows are hard
gates within each track.  In particular, a filesystem scan does not bypass A1
or A2, and a graphics HIDD does not bypass the raw DSI and cache-coherency
tests.

## Master progress

| ID | Deliverable | State | Required next gate |
| :--- | :--- | :--- | :--- |
| F0 | Core, Exec, PSRAM, flash PKG and one-sector SD reads | `hardware verified` | Evidence entry 2026-08-21 |
| A1 | Bounded CMD18 reads, CMD12 stop and complete recovery | `not started` | This is the next implementation step |
| A2 | Hardened, bounded MBR/GPT/EBR discovery | `not started` | Requires A1 |
| A3 | Reproducible, host-built read-only FAT32 `SYS:` image | `not started` | Requires A2 test layout |
| A4 | Minimal resident DOS/FAT bootstrap from flash PKG | `not started` | Requires A2 and A3 |
| A5 | Command and library loaded from MicroSD | `not started` | Closes M6 |
| B0 | Canonical D1001 display contract and provenance | `not started` | Resolve timing contradictions first |
| B1 | Calibrated 200 MHz PSRAM with measured headroom | `not started` | Requires B0; required before scanout |
| B2 | Safe I2C1/PCA9535 panel-power sequence | `not started` | Requires B0; backlight remains dark |
| B3 | LDO3, DSI PHY/host and JD9365 command path | `not started` | Requires B0 and B2 |
| B4 | Stable internal DSI test pattern | `not started` | Requires B3; isolates panel from DMA/PSRAM |
| B5 | Native `800 x 1280` RGB565 PSRAM scanout | `not started` | Requires B1 and B4 |
| B6 | VSYNC handoff, buffering decision and landscape rotation | `not started` | Requires stable B5 |
| C1 | ESP32-P4 boot framebuffer graphics HIDD | `not started` | Requires B6; follows `fbgfx` pattern |
| C2 | Graphics, input skeleton, Layers and Intuition screen | `not started` | Requires C1 |
| C3 | Read-only SD boot to correctly oriented Wanderer (`GB0`) | `not started` | Requires A5, B6 and C2 |
| C4 | Touch as an absolute mouse HIDD | `not started` | Post-GB0; closes M7 |

## Track A: storage and normal boot

There is a hard diagnostic-ordering boundary between A2 and A4.  Complete A1
and A2 with a package that does **not** contain `dosboot.resource`.  The current
ESP32-P4 device and partition diagnostics run after `krnStartExec()`, whose
`RTF_COLDSTART` pass enters dosboot at resident priority -50.  A successful
bootstrap does not return, while the no-media path retries indefinitely, so
those post-`krnStartExec()` diagnostics become unreachable as soon as dosboot
is present.  Add dosboot only in A4.  If a later test must run before the
bootstrap, make it a bounded, one-shot COLDSTART resident deliberately ordered
after `SDCard boot wait` (-49) and before dosboot; separate the priorities if
needed rather than relying on equal-priority link order.

### A1 - multi-block reads and recovery

The generic SD driver uses CMD18 for requests larger than one block and sends
CMD12 only after a successful transfer.  The ESP32-P4 backend currently resets
status/FIFO on an error and returns, which can leave the card in data state.
That is why CMD18 is rejected today.  FAT cannot work around the limit: its
cache reads 32 sectors (16 KiB) at a time.

Implementation gate:

- keep the existing independent write-command denylist and the absence of a
  FIFO transmit path;
- allow only aligned `N * 512` read buffers within an explicit transfer cap;
- fix both generic and platform layers rather than hiding the whole recovery
  in the ESP32-P4 backend: the generic read path owns CMD18/CMD12 protocol
  cleanup after an accepted multi-block command, while the backend owns
  controller/FIFO/data-state recovery and any ambiguous partial start;
- make the generic cleanup attempt CMD12 after every accepted CMD18, including
  data, CRC and timeout failures.  A CMD12 send or busy-ACK failure must make
  the request fail rather than being logged as a successful full transfer;
- in the backend, reset FIFO/data state with finite waits as required, then
  use CMD13 to prove the card returned to `TRAN` before accepting another
  request;
- do not use the current `SoftReset(mask)` blindly for recovery: it ignores
  the mask, performs `RESET_ALL` and clears host configuration.  Prefer a
  bounded FIFO/data reset; after any full reset, completely reprogram the
  host and reselect or re-enumerate the card before setting `host_ready`;
- keep CMD17 as the fallback and recovery proof;
- start at one-bit/400 kHz.  Raise the clock only in a separately recorded
  read-only experiment after the state machine is correct; four-bit mode is
  not required for this phase.

Acceptance gate:

- byte-compare 1, 2, 32 and 128 sectors against repeated CMD17 baselines at
  low LBA, inside the test partition and near the card end through READ64;
- run at least 1,000 bounded repetitions without a mismatch;
- inject command CRC, data CRC and timeout failures; each must terminate,
  recover with CMD12/CMD13 and permit the next CMD17;
- reject zero, unaligned, over-cap and out-of-range requests without changing
  controller/card state;
- keep timer/IRQ heartbeats alive and prove the host image hash is unchanged;
- update the table, README M6 text and evidence log.

Relevant code: [ESP32-P4 bus backend](sdcard/sdcard_esp32p4_bus.c),
[backend limits](sdcard/sdcard_esp32p4_intern.h) and
[generic SD read/stop path](../../rom/devs/sdcard/sdcard_ioops.c).

### A2 - bounded partition discovery

Do not call the current full table scanner on untrusted media immediately
after A1.  Existing GPT, MBR, EBR and recursive dosboot paths need explicit
resource and range bounds before automatic cold-start use.

Implementation gate:

- checked addition/multiplication for every byte, sector and entry range;
- require every MBR/GPT partition start and length to fit the root geometry;
- validate GPT header size, entry size/count, CRC ranges and backup-header
  location before allocation or reads;
- cap entry bytes, partitions, nested tables, total reads and total bytes;
- give EBR a visited-sector set and depth limit;
- give recursive dosboot table traversal the same global budget;
- ensure every malformed exit closes handles and leaves CMD17 usable.

Start with one known MBR/FAT32 partition beginning at LBA 2048.  Add GPT only
after the bounded MBR path passes.  The card layout, expected partition range
and host-side sector hashes must be recorded before insertion.

Acceptance gate:

- the known card reports exactly the expected partition and DOS type;
- truncated, cyclic, overlapping, overflowing and bad-CRC corpus images fail
  within the documented read/time budget;
- no malformed case allocates an unbounded size or loops;
- a normal CMD17 succeeds after every failure;
- the medium hash is unchanged and documentation is updated.

Relevant code: [partition support](../../rom/partition/partition_support.c),
[GPT](../../rom/partition/partitiongpt.c), [MBR](../../rom/partition/partitionmbr.c),
[EBR](../../rom/partition/partitionebr.c) and
[dosboot scan](../../rom/dosboot/bootscan.c).

### A3 - reproducible read-only MicroSD image

Add a normal build target that creates a disposable test image without root
privileges.  Use a single MBR FAT32 partition at a fixed offset.  The target
must produce the image, decoded layout, file manifest and SHA-256 checksum.

The first image contains only what the read-only bring-up requires:

- `AROS.boot` with the target CPU marker expected by DOS;
- a minimal `S:Startup-Sequence` audited to avoid writes to `SYS:`;
- the external proof command and proof library used by A5;
- the normal directory skeleton needed for assigns;
- later, the ordinary AROS distribution tree needed by C3.

Do not use the standard Startup-Sequence unchanged during read-only bring-up;
it performs file-management operations.  Redirect transient state such as
`ENV:` to RAM and add content incrementally.  Formatting and image writes
happen on the host only.

Acceptance gate:

- independent host tools parse and check the FAT image read-only;
- the manifest exactly matches the mounted image;
- rebuilding from unchanged inputs is byte-reproducible, or every intentional
  nondeterministic field is documented;
- before/after hashes match every board run;
- build and usage instructions are linked from the README.

### A4 - resident DOS/FAT bootstrap

Expand the flash `arosbsp` package in measured steps.  The headless storage
bootstrap candidate is the existing `utility`, `partition`, `expansion` and
`sdcard` set plus:

- library: `dos`;
- resources: `bootloader`, `FileSystem`, `dosboot`, `lddemon`, `shell`,
  `shellcommands`;
- handlers: `fat` and, for controlled recovery, `econsole`.

Add `aros`, `oop`, `dos64` or other modules only when link/resident/runtime
evidence shows a dependency; do not grow the package by guesswork.  Audit every
new ELF member and resident priority with the same checks used for the current
package, and keep the declared package below the 0x7e0000-byte partition.

For the headless checkpoint, add an explicit bring-up option that supplies
`KRN_CmdLine="econsole nomonitors nocomposition"`; the platform currently
supplies no command line.  Both display flags are required to skip
`C:AROSMonDrvs` deterministically before ECON opens.  `econsole` then sets the
no-Startup-Sequence boot flag, so an AFTERDOS probe or an interactive command
must perform the A5 load test.  This option is a recovery/test route, not the
final graphical boot route.

Read-only safety must propagate above the block-device denylist.  Today
`sdcard.device` reports `TD_PROTSTATUS` as protected and rejects writes, but
FAT reports `ID_VALIDATED` rather than `ID_WRITE_PROTECTED`, and a failed I/O
can enter an interactive `Retry|Cancel` loop.  Before normal boot, make FAT
recognize the device protection state, reject every mutating packet before it
dirties cache state, and return a bounded error without opening a requester.
The standard Startup-Sequence contains write probes, which is another reason
it cannot be used unchanged for this phase.

Acceptance gate:

- UART lists every expected resident and its version in the intended order;
- the SD boot-wait resident runs before dosboot and remains bounded;
- `FileSystem.resource` contains the FAT entry;
- dosboot replaces the whole-disk node with the expected partition node;
- FAT starts, locks the volume, accepts `AROS.boot` and assigns `SYS:`;
- `Info()` reports the volume write-protected; representative create, delete,
  rename and metadata mutations fail immediately with no dirty cache and no
  requester/retry loop, and any denied block write reports `io_Actual == 0`;
- missing/malformed media falls back to UART/econsole without a hang;
- normal heartbeats continue and documentation is updated.

Package references: [current ESP32-P4 package](boot/mmakefile.src),
[standard base package](../../rom/mmakefile.src),
[standard FS package](../all-native/mmakefile.src) and
[complete RISC-V package](../riscv64-opensbi/boot/mmakefile.src).

### A5 - prove code loading from MicroSD

The first command must not exist in the kickstart or flash PKG.  Load and run
`SYS:C/sdboot-test`; it prints a unique build ID and its load address.  Then
open a small `LIBS:sdproof.library` through lddemon and record its init marker
and address.  Both addresses must lie outside core/flash-package ranges.

This is a second ELF path, not a repeat of the flash-PKG proof: PKG members use
the platform `kernel_elf` loader, while DOS `LoadSeg` uses
`rom/dos/internalloadseg_elf.c` and its own RISC-V relocator/cache-clear path.
Record `readelf -h -r` output for both proof files in the image manifest and
make the fixtures exercise representative relocations actually emitted by the
target toolchain.

Acceptance gate:

- DOS/LoadSeg executes the command from FAT and lddemon opens the library;
- command, library and UART build IDs match the image manifest;
- missing, malformed, wrong-machine and deliberately unsupported-relocation
  proof files fail cleanly;
- the system remains alive and the card hash is unchanged;
- the test succeeds through the normal boot path as well as the recovery path;
- M6 changes to `hardware verified` in both roadmap and README.

## Track B: panel hardware and scanout

### B0 - freeze the display contract

The native panel is JD9365, `800 x 1280` portrait, RGB565.  The product is
mounted landscape, so the eventual logical surface is `1280 x 800`.  The
reference implementation suggests a 270-degree transform, but the direction
remains a hypothesis until an asymmetric B6 hardware pattern proves it.  One
native framebuffer is 2,048,000 bytes.

The references currently disagree and must not be presented as one verified
timing.  The D1001 header says horizontal `40/140/40`, vertical `4/16/16`,
40 MHz and calls it 60 Hz; those totals yield about 29.8 Hz.  The running
Vellum driver ignores those fields and uses horizontal `20/20/40`, vertical
`4/30/30` at 40 MHz, about 33.82 Hz.  AROS starts with the latter reference
and measures VSYNC; a timing change is a later, isolated experiment.

Before implementation, record in this repo:

- the exact reference register/command trace and measured frame rate;
- lane count/rate, virtual channel, DBI widths, pixel format, pitch and reset
  polarity/times;
- the licence and provenance of every initialization table used.  The external
  Vellum tree is a behavioral reference, not code that may be copied into AROS
  without a compatible licence.

Known inputs to verify are DSI bus/VC 0, two lanes at 1,000 Mbit/s each, LDO
channel 3 at 2.5 V, 8-bit DBI command/parameter widths and RGB565 output.

Acceptance gate: one canonical display-contract table replaces the conflicting
assumptions here and in the README, distinguishes verified facts from the
rotation-direction hypothesis, and has a dated hardware/reference record.

### B1 - calibrated 200 MHz PSRAM

At 20 MHz the theoretical x16 DTR bandwidth is 80 MB/s, the same as a 40 MHz
RGB565 pixel stream and therefore no usable margin.  Port the necessary IDF v6
timing calibration rather than changing only the divider.

The calibration runs from internal SRAM with internal stack/data, interrupts
disabled and no dependency on PSRAM.  The current IDF reference uses MR0 read
latency 4, MR4 write latency 1, read dummy 26, register dummy 12, four DQS
phases and 31 delay candidates per phase.  Sweep and verify; do not hardcode a
candidate observed on one boot.  Select the middle of a sufficiently wide
passing window.  On failure, fall back explicitly to 20 MHz and mark display
scanout blocked.

Acceptance gate:

- complete address/data and large sequential tests over all 32 MiB;
- repeated cold/warm boots choose a valid window without corruption;
- measured relevant read bandwidth is at least 100 MB/s and retains margin
  under concurrent CPU/cache activity;
- the 20 MHz fallback is still functional and never starts DSI scanout;
- results, selected window statistics and artifact identity are documented.

### B2 - safe panel power control

Bring up I2C1 on GPIO20/GPIO21 and PCA9535 address 0x20 without touching the
panel data path.  The required control bits are PWR_HOLD 8, LCD_PWR_EN 0,
LCD_BL_EN 7 and active-low LCD_RST 2.  Backlight PWM is GPIO14.

Never write a blanket value to the PCA9535.  Preload a safe output latch and
preserve unrelated bits before changing direction: keep PWR_HOLD high,
amplifier and backlight off, panel power controlled and reset asserted.  Set
GPIO14 to duty zero before enabling its path.  Then enable panel power, wait
50 ms and perform the reference reset sequence high 5 ms, low 10 ms, high
120 ms.  Keep the backlight dark until B4 has a stable pattern.

Acceptance gate:

- configuration/output readback matches each intended state;
- repeated sequences do not power the board off or enable unrelated hardware;
- rail, reset and backlight transitions are bounded and logged;
- failure leaves reset asserted and the backlight dark.

### B3 - LDO, DSI PHY/host and JD9365 command mode

Enable LDO3 at 2.5 V, clock/reset the DSI bridge, configure two lanes and wait
with finite timeouts for PHY PLL lock and lane stop-state.  Enter command mode
and send the proven JD9365 initialization sequence over DBI while the backlight
remains off.

Attempt DCS ID read `0x04`, but do not invent an expected value: the existing
reference does not define one.  A stable response across reset that is not an
obvious all-zero/all-one bus value becomes a documented board fact.  If the
panel does not support a useful read, command completion plus B4 is the gate.

Acceptance gate: every clock/PHY/command wait is bounded, reset is recoverable,
and a failure returns to UART with panel/backlight in a safe state.

### B4 - internal DSI test pattern

Use the DSI host's internal pattern generator with the B0 timing before any
PSRAM DMA.  This isolates panel, PHY, command sequence, DPI timing and color
coding from memory and cache behavior.  Enable LCD_BL_EN and a low PWM duty
only after the pattern is active.

Acceptance gate:

- full active area has stable geometry and correct RGB order;
- measured VSYNC matches the B0 contract (about 33.82 Hz for the initial
  reference), not an assumed 60 Hz;
- cold/warm failures never cause an uncontrolled bright flash;
- a photo/log reference and measurement are recorded.

### B5 - native PSRAM framebuffer and coherency

Start with exactly one aligned `800 x 1280` RGB565 buffer in PSRAM.  DMA
descriptors, ISR data and controller state stay in internal SRAM.  Configure
DW-GDMA and the DSI bridge using the IDF v6 behavior as the register reference.

CPU writes are not automatically coherent with display DMA.  Clean each dirty
region CPU-to-memory before presentation, rounding to cache lines or complete
physical scanlines.  Do not add rotation or multiple buffers yet.

Acceptance gate:

- full-screen solids, bars, checkerboards, corners and one-pixel lines are
  correct from PSRAM;
- moving/repeated dirty rectangles never show stale cache lines;
- VSYNC and underrun counters remain clean for at least 30 minutes while SD
  reads and PSRAM stress run concurrently;
- ten cold and ten warm boots pass with UART diagnostics intact.

### B6 - VSYNC handoff, buffering and landscape

Add VSYNC-controlled ownership to the working native scanout.  Measure before
choosing one, two or three buffers; triple buffering is not a default
requirement.  Next add the hardware-proven 90- or 270-degree transform from
logical `1280 x 800` to physical `800 x 1280`, with explicit bounds and
cache-clean regions.  Treat the reference driver's 270-degree mapping as the
first candidate, not as a board fact.

Acceptance gate:

- native single-buffer behavior still passes B5;
- the selected buffering strategy has a documented memory/bandwidth reason;
- all four logical corners and asymmetric test labels land in the correct
  physical positions;
- no tearing, underrun or out-of-bounds write occurs under sustained updates.

## Track C: AROS graphics and Workbench

### C1 - ESP32-P4 framebuffer graphics HIDD

Use the Raspberry Pi/EFI `fbgfx` family as the structural model, not a private
graphics API.  Hardware power, DSI, DMA and scanout stay below the HIDD.  The
module subclasses the generic graphics/bitmap/display classes, publishes one
logical `1280 x 800` RGB565 mode over the B6 transform and registers it through
`AddDisplayDriver(..., DDRV_BootMode, TRUE, ...)`.

Give the platform HIDD resident priority 9, matching the Pi/EFI references.
That becomes a normal-boot invariant in C2: Intuition priority 15 installs its
display callback first, then the HIDD registers the driver.  A C1-only package
without Intuition may exercise the queued/forced graphics path, but that is not
evidence for the final resident integration.

Initial resident set for this gate:

- libraries: `aros`, `utility`, `oop`, `graphics`;
- HIDDs: `hiddclass`, `gfx` and the new ESP32-P4 framebuffer HIDD;
- existing core/debug/timer and board modules.

Acceptance gate:

- exactly one logical `1280 x 800` mode with correct physical pitch, masks and
  RGB order;
- driver registration and bitmap allocation succeed repeatedly;
- Show, RectFill, lines, text and transposed dirty-region updates work in the
  proven orientation without memory damage;
- raw B5 diagnostics remain available beneath the HIDD.

References: [Raspberry Pi fbgfx](../aarch64-raspi/hidd/fbgfx) and
[EFI fbgfx](../../rom/hidds/efifbgfx).

### C2 - first Layers/Intuition screen

Add `layers`, `intuition` and `keymap`; also add the generic input skeleton
even before touch: `input.device` opens `keyboard.device` and `gameport.device`
unconditionally.  The gate therefore includes `input`, `keyboard`, `gameport`,
`inputclass`, `keyboard.hidd` and `mouse.hidd` with no fabricated touch events.

Preserve and log this COLDSTART ordering: Intuition at priority 15, the
ESP32-P4 HIDD at 9, a bounded one-shot screen-test resident at 8, and
`dosboot.resource` at -50.  `SetDisplayDriverCallback()` only
stores the callback; it does not itself replay an already queued driver, so the
acceptance boot must show the callback installed before `AddDisplayDriver()`
rather than depending implicitly on a later `BestModeIDA()` replay.

Acceptance gate:

- a small resident/test process opens Intuition, a Screen and a Window;
- UART records the resident order, display callback and successful driver
  insertion before the test screen and dosboot;
- text, refresh, overlap/scroll and pointer rendering are stable;
- absence of physical keyboard/mouse/touch does not block initialization;
- 20 reproducible boots pass and the package/resident order is documented.

### C3 - normal read-only boot to Wanderer (`GB0`)

Merge the proven storage and graphics sets.  A complete candidate flash package
is based on the standard native/RISC-V packages and contains:

- libraries: `aros`, `oop`, `utility`, `graphics`, `layers`, `intuition`,
  `keymap`, `dos`, `gadtools`, `partition` (and `dos64` only if proven needed);
- architecture libraries: `expansion`, `debug` where not already core;
- resources: `bootloader`, `dosboot`, `FileSystem`, `lddemon`, `misc`, `shell`,
  `shellcommands`;
- devices: `console`, `input`, `gameport`, `keyboard`, `sdcard` and existing
  `timer.device`;
- handlers: `ram`, `con`, `fat` plus the separate recovery `econsole` route;
- HIDDs: `hiddclass`, `inputclass`, `gfx`, `keyboard`, `mouse` and the
  ESP32-P4 framebuffer HIDD.

Build and add this set incrementally; the list is a target, not permission to
hide an unexplained dependency.  Wanderer, Zune/MUI classes, icon/workbench
libraries, fonts, themes and ordinary commands stay in the normal SD `SYS:`
tree.  Start with a minimal read-only Startup-Sequence, then audit and approach
the standard [Startup-Sequence](../../workbench/s/Startup-Sequence) without
allowing writes to the test card.

`GB0` acceptance gate:

- cold reset uses the normal, non-diagnostic path and mounts SD as `SYS:`;
- DOS assigns are correct and Startup-Sequence launches Wanderer from SD;
- the desktop, title and icons are visible at logical `1280 x 800`, right-side
  up, with correct colors and no clipped edge;
- missing SD or failed display produces a bounded UART diagnostic/recovery
  path instead of a silent hang;
- UART remains usable, SD reads continue and the card hash is unchanged;
- at least 20/20 cold boots and a 30-minute desktop/SD/graphics soak pass;
- roadmap, README and evidence log are updated before claiming graphical boot.

### C4 - touch and M7 completion

The controller identity and firmware requirements are not yet verified on this
board; `GSL3670` is historical information, not a current hardware result.
First probe I2C0 (GPIO37/38) and GPIO16 interrupt safely and document address,
identity, reset/firmware needs and licence provenance.

Implement the device as a hardware subclass of `mouse.hidd`, not a private
route into `input.device`.  Report absolute motion before press, final motion
before release, handle only contact zero initially and perform I2C work in a
task/worker rather than interrupt context.  Apply the same orientation transform
as B6 and make calibration explicit.

Acceptance gate:

- corners/center, press, release and drag match the visible pointer;
- 1,000 tap/drag cycles leave no stuck button;
- I2C errors and finger lift recover cleanly;
- absent touch hardware does not prevent C3;
- README M7 and this roadmap change to `hardware verified` with evidence.

## Risks and decisions that must remain visible

| Item | Current decision or unresolved question |
| :--- | :--- |
| SD safety | Target remains read-only through GB0; host image hash is the guard. |
| CMD18 failure | Split ownership: generic code must run CMD12 cleanup and propagate STOP errors; the ESP32-P4 backend must recover controller/FIFO/data state and prove `TRAN`. |
| Partition input | Treat all table bytes as untrusted and budget every traversal. |
| Boot package | Boot-critical residents remain in internal flash; ordinary system files load from SD. |
| Package capacity | `arosbsp` is 0x7e0000 bytes; size and every member hash are checked on each expansion. |
| Panel timing | Start from measured Vellum behavior, not the contradictory 60 Hz comment. |
| PSRAM | 20 MHz remains the safe fallback; display scanout requires a calibrated, measured faster path. |
| Rotation | Native portrait first; correct landscape only after stable scanout and VSYNC ownership. |
| Buffering | One buffer first; extra buffers require measured need and explicit memory cost. |
| Cache/DMA | Every presented CPU-written region needs an explicit clean operation. |
| Source provenance | External Vellum/Waveshare/IDF sources are references; licence compatibility is checked before code reuse. |
| Touch | Not a GB0 dependency and not claimed as GSL3670 until probed. |

## Evidence log

Append new entries; do not rewrite old ones except to correct a factual typo,
and record substantive corrections as a new entry.

### 2026-08-21 - F0 and public partition read verified

- State change: the core/Exec/flash-PKG/single-sector baseline became
  `hardware verified`; CMD18, table scanning and FAT remained unattempted.
- Hardware: reTerminal D1001, ESP32-P4 revision 1.3; SanDisk `SN128` SDXC,
  249,737,216 sectors.
- Source identity: commit `a57b02e34a4f5634ea0614f02b30dc474da5bff1`
  plus the uncommitted ESP32-P4 M6 work (`a57b02e34a-dirty`).  The exact dirty
  diff was not archived with the capture; the artifact hashes below are the
  authoritative identity for this historical run.  Future gates must retain
  the exact diff or a commit.
- Configuration: XIP core in `ota_0`, four-member package in `arosbsp`,
  `P4_SDCARD_DEVICE_TEST` and `P4_PARTITION_TEST`, one-bit SD at 400 kHz,
  polling PIO and read-only command denylist; no partition-table scan.
- Core: 148,848 bytes, SHA-256
  `4a6dede548e7d11151448e122aa0778a1e63cbdf0c96cb5c840b7f47c35cf3c9`.
- BSP PKG: 290,904 bytes, SHA-256
  `cc18bc81c701b376294c4a39e024048adcee58a6e50595b831439dcce23885cc`.
- Package result: four modules loaded; 361,308 bytes of PSRAM reserved;
  `partition.library` version 3 opened successfully.
- Storage result: sector size 512, total 249,737,216; READ64 advertised;
  low/high single-sector reads replaced sentinel buffers.  Public
  `ReadPartitionDataQ()` LBA 0 returned 0, `io_Actual` 512 and hash
  `0x4d7705c5`.
- Runtime result: 100 Hz timer/IRQ heartbeat remained stable through the
  bounded capture.
- Procedure: build the XIP core and four-member package with the diagnostic
  defines above, verify both hashes, write only the core at `0x20000` and the
  package at `0x820000`, cold-reset with the card inserted and capture UART
  through both bounded diagnostic routines.
- Acceptance: package validation/relocation, resident discovery, direct low
  and high single-sector reads, public root read, buffer replacement, repeated
  LBA-0 hash and continuing tick all passed.  Multi-block reads, error
  injection, partition discovery and FAT were not attempted and belong to
  A1-A4.
- Safety: no SD write command, partition-table scan or storage-partition write
  occurred.  Internal flash writes were limited to `ota_0` and `arosbsp`.
- Remaining boundary: CMD18 is still rejected; A1 is the next safe step.

## Evidence-entry template

```text
### YYYY-MM-DD - <phase and result>

- State change:
- Hardware / revision:
- Source commit or dirty-worktree identifier:
- Core/package/image sizes and SHA-256:
- Test defines and clock/bus/mode parameters:
- Command or reproducible procedure:
- Expected result:
- Observed UART / measurement / photo reference:
- Acceptance points passed and failed:
- Safety impact and before/after media hash:
- Remaining risk or blocker:
- Next safe step:
```
