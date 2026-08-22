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
| A1 | Bounded CMD18 reads, CMD12 stop and complete recovery | `hardware verified` | Evidence entry 2026-08-22: 59 card-referenced cells, 1,000 repetitions, three injected fault modes with CMD12/CMD13 recovery, invalid-request rejection, heartbeat.  Two gate points met differently and documented: card-end comparison via the 32-bit boundary addresses, over-cap rejection unreachable through the device |
| A2 | Hardened, bounded MBR/GPT/EBR discovery | `hardware verified` | Evidence entry 2026-08-23: the card reports exactly its one partition, and eleven malformed tables served from `ramtest.device` are all refused within 4 to 36 sector reads with a working read after each |
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

The generic SD driver uses CMD18 for requests larger than one block and had
sent CMD12 only after a successful transfer.  The ESP32-P4 backend previously
reset status/FIFO on an error and returned, which could leave the card in data
state; CMD18 was therefore rejected.  The A1 implementation now performs the
generic stop attempt and bounded backend recovery below, and has a build-only
comparison/fault diagnostic.  The first D1001 run proves the two-sector path
but fails at 32 sectors, so it is `hardware partial`, not verified.  FAT
cannot work around the limit: its cache reads 32 sectors
(16 KiB) at a time.

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

Current implementation approach (2026-08-22): the complete ESP-IDF v5.4.2
four-by-4-KiB ring, RI/NI refill, poll-demand, auto-stop and
DMA-to-`DATA_OVER` transaction model was implemented and rejected on the
D1001: it did not repair the LBA-2048 duplicate and it stalled at 16 KiB with
`RXDR` pending.  The board is restored to the bounded 512-byte receive chain
and physical generic CMD12/CMD13 fallback, which preserves working zero/high
LBA 1/2/32/128 reads.

Correction of 2026-08-22, after the test card was read in a host reader.  The
sentence above, and every earlier claim that low and high LBAs pass, rest on
comparisons that could not fail.  The card holds a valid MBR at LBA 0 with
hash `0xdebe99c1`, while the driver returned 512 zero bytes there, and the
matrix compared that wrong result against an equally wrong CMD17 baseline and
reported a match.  4086 of the first 4096 sectors are zero-filled, so a
misdirected read looks like zeroes rather than an error.  The defect is
therefore not LBA-dependent, and it is not confined to CMD18: the
single-block path is wrong at LBA 0 as well.  The passive capture is deferred
until the matrix compares against the card.  This is not permission for CMD17
substitution or filesystem work.

Acceptance gate:

- byte-compare 1, 2, 32 and 128 sectors against the card's own content, as
  recorded in `sdcard/test-card-reference.md`, at low LBA, across the
  partition start in both directions and inside the partition.  A CMD17
  baseline alone is not an acceptance reference: it shares the controller,
  FIFO and cache path with CMD18, and at LBA 0 both were wrong together.
  Where no card reference exists, in particular near the card end through
  READ64, the cell must declare itself a self-comparison and must not count
  as a pass;
- prove that the reference blocks of a cell differ from each other.  A cell
  whose expected blocks are identical cannot detect a repeated block, and
  most of this card is zero-filled;
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

### 2026-08-22 - A1 CMD18 implementation and diagnostic build verified

- State change: A1 advanced from `not started` to `build verified`; no A1
  hardware-acceptance claim is made.
- Hardware: no new board run.  The next run is bounded to the existing
  reTerminal D1001 rev. 1.3 and SanDisk `SN128` SDXC card.
- Source identity: baseline commit `7bbfd7b6d1` plus the uncommitted A1 diff.
  The diff adds generic CMD12 cleanup, bounded P4 FIFO/CMD12/CMD13 recovery,
  transfer limits and diagnostic-only CMD18 comparison/fault hooks.
- Configuration: diagnostic core built with `P4_SDCARD_DEVICE_TEST`,
  `P4_SDCARD_MULTIBLOCK_TEST`, `P4_SDCARD_REPEAT=1` and
  `P4_PARTITION_TEST`; XIP linker script.  The package has no fault-injection
  define and retains one-bit/400-kHz polling PIO and the write denylist.
- Core: 150,480 bytes, SHA-256
  `75d7483240e237fafc02b119bb72cad61435a3b264fb8f9bcc47c893602d0d25`.
- BSP PKG: 295,540 bytes, SHA-256
  `fa2389fa5bbb894993f3a4ff3a910b1cd70af4929a4504c1a281339503ef77ee`;
  its `sdcard.device` member is 119,776 bytes.
- Procedure: build the package and diagnostic XIP core, including the selected
  test defines; `sdcard.device` and `kernel_startup.c` compiled, the core
  linked and converted with esptool v5.3.0.  `git diff --check` passed.
- Expected hardware result: the matrix compares 1, 2, 32 and 128 sectors at
  LBA 0, LBA 2048 and through READ64 at the card end, then reports a bounded
  two-sector repeat count.  A fault build injects exactly one post-start
  command-CRC, data-CRC or timeout branch and accepts it only when the next
  CMD17 succeeds.
- Safety: no flash or SD-media write occurred.  The added fault hook can only
  synthesize one read-side failure; it never adds FIFO transmit or a write
  command.
- Remaining risk: all A1 acceptance points, including the 1,000-repeat run,
  each injected-failure mode, invalid-request rejection, heartbeat continuity
  and before/after card hash, remain to be observed on the D1001.
- Next safe step: with explicit approval for only `ota_0` at `0x20000` and
  `arosbsp` at `0x820000`, capture the normal matrix first, then the three
  one-shot fault builds; do not touch bootloader, partition table or `storage`.

### 2026-08-22 - A1 first D1001 CMD18 matrix: two sectors pass, 32 sectors fail

- State change: A1 became `hardware partial`.  The two-sector CMD18 path is
  observed; the required 32/128-sector and recovery gate did not pass.
- Hardware: reTerminal D1001, ESP32-P4 revision 1.3, MAC
  `e8:f6:0a:e0:46:4c`; Winbond `ef:4019`, 32 MiB; SanDisk `SN128` SDXC,
  249,737,216 sectors.
- Source identity: baseline `7bbfd7b6d1` plus the current uncommitted A1 diff.
- Artifacts/configuration: diagnostic XIP core (150,480 bytes, SHA-256
  `75d7483240e237fafc02b119bb72cad61435a3b264fb8f9bcc47c893602d0d25`)
  with `P4_SDCARD_DEVICE_TEST`, `P4_SDCARD_MULTIBLOCK_TEST`,
  `P4_SDCARD_REPEAT=1` and `P4_PARTITION_TEST`; BSP PKG (295,540 bytes,
  SHA-256 `fa2389fa5bbb894993f3a4ff3a910b1cd70af4929a4504c1a281339503ef77ee`).
  One-bit/400-kHz polling PIO, no fault-injection define.
- Procedure: esptool v5.3.0 verified both artifacts while writing only the
  core at `0x20000` and package at `0x820000`, then hard-reset the board and
  captured USB-Serial/JTAG output.  No bootloader, table, NVS or `storage`
  range was written.
- Observed result: CMD17 baseline reads and CMD18 two-sector LBA-0 comparison
  passed (`0x1f116dc5`); the backend logged `CMD12/CMD13 confirmed TRAN (1)`.
  The first 32-sector CMD18 then failed with raw status `0x00000420` and
  controller status `0x04009509`.  Its recovery CMD12 timed out, logged
  `multi-block recovery failed; disabling host`, and all following SD reads
  failed with error `0xfe`, as designed after an unconfirmed stop.
- Safety/runtime result: no SD write was attempted.  The CLIC/timer continued
  through and after the failure (100 Hz heartbeat remained live for over 50
  seconds), so the failure is bounded to the storage path rather than a system
  hang.  A before/after host card hash was not captured in this failed run.
- Remaining risk: determine why the DesignWare data path faults at 32 sectors
  and make bounded recovery issue a usable CMD12/CMD13 or re-enumerate before
  the next request.  The 128-sector, 1,000-repeat, injected-failure and
  invalid-request gates remain unrun.
- Next safe step: audit the local ESP-IDF/Vellum DesignWare recovery sequence,
  add only the required bounded data/FIFO reset ordering, then rebuild and
  repeat the same normal matrix before attempting fault builds.

### 2026-08-22 - A1 non-zero second-block failure reproduced

- State: A1 remains `hardware partial`; this is diagnostic evidence, not an
  acceptance result.
- Artifact/procedure: the existing diagnostic core (SHA-256
  `f463edc55920d1b105f44f0ca204bc9863d3f70886d365633b7ba5ebd1fc163a`)
  was retained.  The updated BSP PKG (296,096 bytes, SHA-256
  `8898e9422dca46c1c2eaf2cf3ed977d56f31c7785e6ec638a474a95c95f0bdae`)
  selected `BYTCNT=0` for CMD18 and explicit generic CMD12; esptool v5.3.0
  verified the write at `arosbsp` offset `0x820000` before hard reset.
- Observed result: all-zero low and high-LBA comparisons pass through 128
  sectors, and every CMD12/CMD13 confirmation returns the card to TRAN.  At
  non-zero LBA 2048, 2-, 32- and 128-sector CMD18 comparisons all first differ
  at byte 512: expected first word of LBA 2049 is `0x41615252`, while the PIO
  read repeats the first word of LBA 2048, `0x429058eb`.  The request reports
  no controller error, then CMD12/CMD13 succeeds and the 100-Hz heartbeat
  continues.
- Interpretation: this rules out the predefined-versus-undefined BYTCNT mode
  as the direct cause.  The current polling loop samples the FIFO on every
  poll, even without an RXDR ownership handoff; the next experiment restricts
  FIFO reads to acknowledged RXDR/DATA_OVER events, matching DesignWare PIO
  ordering.  The final card hash and all remaining A1 gates are still pending.
- Safety: this BSP-only write was within the standing device authorization;
  neither the core/boot partitions nor any SD-media command that can write
  data was touched.

### 2026-08-22 - A1 RXDR acknowledgement ordering did not change data

- State: A1 remains `hardware partial`.
- Artifact/procedure: BSP PKG SHA-256
  `f4962593a2f8c481b01ad11df8a570a05ef4badaea53790af33f92679a8a3aae`
  was built without errors or whitespace faults, verified by esptool at
  `0x820000`, and run with the unchanged diagnostic core.
- Observed result: acknowledging RXDR before draining it reproduces the same
  first mismatch at LBA 2048 byte 512 for 2, 32 and 128 sectors.  Low/high
  zero data, CMD12/CMD13 and continuous heartbeat still pass.
- Next safe step: test a sector-sized RX watermark (127 FIFO locations) so
  PIO receives each full 512-byte block in one handoff, then rerun the same
  no-write matrix.

### 2026-08-22 - A1 PIO boundary confirmed; switch to bounded IDMAC receive

- State: A1 remains `hardware partial`.  The polling PIO approach is not a
  candidate for acceptance, even at a sector-sized RX watermark.
- Artifact/procedure: BSP PKG SHA-256
  `861ec647ec47f8b238d5b6cccd8dddb181d75b3961b6c4b28f883cffbc29de13`
  was verified and flashed only at `0x820000`.  Temporary counters for the
  LBA-2048 test reported the first handoff as 128 FIFO locations/512 copied,
  then 512 locations/1024 copied; CIU byte count advanced while the second
  block contents still repeated the first word.  The probe is removed from
  source after capturing this evidence.
- Interpretation: the controller accepts an ongoing CMD18 stream, CMD12 then
  restores TRAN, and the timer remains live, but CPU PIO cannot provide a
  correct block-boundary receive path on this D1001/P4 configuration.  Changes
  to BYTCNT, RXDR acknowledgement ordering and RX watermark all preserve the
  same byte-512 failure.  This is a transport limitation, not permission to
  downgrade a multi-sector request into CMD17 calls.
- Next implementation gate: add a receive-only, bounded P4 IDMAC descriptor
  path for up to 64 KiB, with aligned buffers/descriptors, finite IDMAC/data
  completion waits, cache maintenance where required, existing generic CMD12
  ownership and the same read-only command denylist.  Rerun the full matrix
  before any stress or fault-injection build.

### 2026-08-22 - A1 IDMAC gate found; incomplete DMA enable sequence

- State: A1 remains `hardware partial`; this entry records a failed,
  recoverable diagnostic run rather than acceptance.
- Artifact/procedure: receive-only bounded IDMAC descriptors were added and
  BSP PKG SHA-256
  `1193f6f231dc25acb69cc75b80f1c5314d95f766e0af8d65a602775ecd2ba08e`
  was verified and flashed only at `arosbsp` offset `0x820000`.  The unchanged
  diagnostic core SHA-256 was
  `f463edc55920d1b105f44f0ca204bc9863d3f70886d365633b7ba5ebd1fc163a`.
- Observed result: CMD18 reached DTO but descriptor 0 still appeared owned;
  IDSTS was `0x00002000`, which is the IDMAC FSM's suspended state, not a
  media error.  Generic CMD12/CMD13 consistently restored TRAN and the
  heartbeat stayed live; single-block reads continued to pass.
- Root cause / correction: comparison with the local ESP-IDF P4 HAL showed
  that the experimental path enabled only BMOD.  The P4 additionally requires
  CTRL.DMA_ENABLE (bit 5), CTRL.USE_INTERNAL_DMA (bit 26), and BMOD.FB.  The
  completion path also must invalidate descriptor cache lines before reading
  IDMAC-cleared OWN bits.  These corrections are queued for the next BSP
  build and device run.
- Safety: no SD write command was issued.  Only the authorized BSP partition
  was changed; bootloader, partition table, otadata, NVS and core image were
  untouched.

### 2026-08-22 - A1 IDMAC cache-coherency correction pending retest

- State: A1 remains `hardware partial`; the corrected DMA gates alone did not
  complete a receive.  DTO was followed by IDMAC FSM suspend (`0x00002000`)
  and controller host timeout, while CMD12/CMD13 and the 100-Hz heartbeat
  remained live.
- Root cause: the generic `CacheClearE()` implementation for this RISC-V P4
  port is intentionally only a memory-ordering fence.  It neither writes
  descriptor ownership to PSRAM nor invalidates bytes changed by IDMAC.
  Therefore the DMA engine can receive stale descriptor contents even though
  DBADDR and all three P4 DMA enable gates are programmed.
- Correction queued: use the documented P4 ROM range writeback operation for
  descriptor and destination handoff, and its range invalidation operation
  before inspecting IDMAC ownership and returning received bytes.  This keeps
  the bounded receive-only design and does not add any SD-media write path.

### 2026-08-22 - A1 IDMAC transport restored; P4 multi-block progression remains blocked

- State: A1 remains `hardware partial`; the result establishes a stable,
  read-only diagnostic baseline but is not an acceptance run.
- Artifact/procedure: P4 ROM range writeback/invalidate operations replaced
  the RISC-V fence-only cache hook for descriptor and destination handoff.
  With the manual generic CMD12 path and 512-byte IDMAC descriptors, BSP PKG
  SHA-256 `e53efc6c77740571660ff40ef6b23f414382ef39ed4ab242efe777805fce0e24`
  (301,968 bytes) was verified and flashed only at `arosbsp` offset
  `0x820000`; esptool confirmed the device hash.
- Observed result: the controller consumes the complete bounded chain and
  generic CMD12/CMD13 returns TRAN.  The 1/2/32/128 matrix transports through
  both low-LBA and READ64 high-LBA paths without controller faults; the 100-Hz
  heartbeat remains live.  The non-zero LBA 2048 comparison still first
  differs at byte 512 for every multi-block length: LBA 2049 should begin
  `0x41615252`, while IDMAC returns LBA 2048's `0x429058eb` again.  Reducing
  descriptors to exactly 512 bytes did not change that boundary, excluding a
  descriptor-size crossing as the cause.
- Follow-up experiment: the vendor driver marks every CMD18 with hardware
  auto-stop.  BSP PKG SHA-256
  `36eaaa7c9aaa2019f28671eb6e98111d2e4816150c7dd04e9ec4dd221c383c1e`
  (302,540 bytes) enabled it while preserving generic StopRead completion via
  CMD13.  Two-block requests reached auto-CMD12/CMD13 confirmation but still
  repeated LBA 2048's first block.  At 32/128 blocks the P4 consumed every
  programmed descriptor then raised `HTO|RXDR` (`0x00000420`) before
  `DATA_OVER`; no ACD was observed.  This experimental image is not a stable
  baseline and has been reverted in source to the manual generic CMD12 path.
- Interpretation: cache coherence and descriptor ownership were genuine
  blockers and are fixed; the remaining fault is P4 data/block progression,
  independent of PIO versus IDMAC, descriptor size, BYTCNT mode, RXDR order,
  watermark and auto-stop.  All-zero low/high regions alone cannot prove
  progression and are now treated as transport-only evidence.
- Safety: both package writes were verified and limited to `0x820000`; no
  SD-media write was issued and the kernel stayed live.  Stress, fault and
  invalid-request gates remain intentionally unrun until a non-zero
  multi-block comparison passes.
- Restored device state: after the auto-stop experiment, the manual generic
  CMD12 baseline was rebuilt as BSP PKG SHA-256
  `4be7e8b8bca718453b4e1ca4791646cd7487b34a1d62756b89931ddff3abe16b`
  (301,964 bytes), verified by esptool at `0x820000` and booted.  It reproduces
  the bounded LBA-2048 byte-512 mismatch while CMD12/CMD13 and the 100-Hz
  heartbeat continue, so the device is left in the safer diagnostic state.
- Next safe step: retain the manual generic CMD12 baseline on device, then
  obtain a P4-specific controller trace or a vendor-supported transaction
  path that proves distinct second-block data before attempting A1 acceptance.

### 2026-08-22 - A1 next implementation gate: reproduce the Espressif transaction model

- Research result: no public issue identified the exact P4 symptom (CMD18
  returning the first non-zero block again at byte 512).  The local ESP-IDF
  v6.0.1 driver nevertheless exposes a material architectural difference:
  its receive IDMAC is a refillable ring, treats RI/NI only as descriptor
  handoff, and completes a multi-block request solely at `DATA_OVER` after
  controller auto-stop.  It does not reset BMOD/DMA/FIFO or return to the
  caller when the last initially submitted descriptor loses OWN.
- Next bounded change: replace the current one-shot descriptor chain with a
  small, receive-only 4-KiB descriptor ring copied exactly from that model;
  on every polled RI/NI, refill and poll-demand it.  CMD18 will use the P4
  auto-stop only in this test implementation and will wait for `DATA_OVER`
  plus ACD.  The generic `sdcard_StopRead()` call remains mandatory; the P4
  backend will satisfy that call by CMD13 TRAN confirmation when hardware has
  already emitted the physical CMD12.  No write command or transmit
  descriptor is introduced.
- Decision gate: first run only LBA 2048 at 2/32/128 sectors and compare each
  block with CMD17.  If those are distinct and correct, run the complete A1
  matrix, 1,000 repetitions, negative request checks and the three one-shot
  read-fault builds.  If the exact ring model still repeats block one, stop
  implementation churn and capture a wire/controller trace for an upstream
  P4 report; do not substitute CMD17 for CMD18.
- Local-IDF verification: ESP-IDF v6.0.1 (`8c19b156`) defines the same
  cache-line-sized 64-byte P4 descriptor layout, but uses
  `SDMMC_DMA_MAX_BUF_LEN=4096` and four descriptors by default.  Its ISR
  clears RI/NI, refills free descriptors and issues poll-demand; its transfer
  state machine does not declare success until the DMA completion transition
  and `DATA_OVER`.  Its SD read API relies on hardware auto-stop and follows
  it with CMD13 status/idle confirmation, rather than sending a second
  physical CMD12.  The local IDF worktree has unrelated bootloader edits and
  is read-only for this investigation.

### 2026-08-22 - A1 refill-ring trial unassessed; stable diagnostic state restored

- State: A1 remains `hardware partial`.  This was an engineering probe, not
  a functional change: its source was reverted and must not be committed.
- Procedure: a receive-only test implementation reproduced the local-IDF
  four-by-4-KiB descriptor ring, RI/NI refill plus poll-demand, hardware
  CMD18 auto-stop and CMD13 TRAN confirmation in place of a second physical
  CMD12.  It compiled cleanly and produced BSP package SHA-256
  `5a7a54752065e2d2f86e6dff299ea577e259027fee2ab549200f9eb15538981c`
  (306,088 bytes).  The package was flashed only at `0x820000` and verified
  with both esptool write verification and `verify-flash`.
- Result limitation: the UART collector used for that first boot did not
  enable screen output logging, so it captured no evidence.  This is not
  evidence of a kernel failure or of a ring result.  The correct collector is
  `screen -L`, which subsequently captured the restored image's kernel
  heartbeat and boot output.
- Restoration: the experimental source was returned to the prior manual
  CMD12/512-byte-descriptor diagnostic path.  BSP package SHA-256
  `4be7e8b8bca718453b4e1ca4791646cd7487b34a1d62756b89931ddff3abe16b`
  (301,964 bytes) was rebuilt, flashed only at `0x820000`, and independently
  verified with esptool.  The serial log confirms the restored kernel's
  continuing 100-Hz heartbeats.  No SD write command was issued.
- Prerequisite for a repeat: the diagnostic core must be built with both
  `-DP4_SDCARD_DEVICE_TEST` and `-DP4_SDCARD_MULTIBLOCK_TEST`; the
  reproducible spelling is `P4_A1_DIAGNOSTIC=1`.  These switches are off by
  default, so a normal package cannot produce CMD18 matrix evidence.  Preserve
  the `screen -L` log from before reset through the result.  Do not repeat the
  ring experiment until that observation path is confirmed on the manual
  baseline.

### 2026-08-22 - A1 matrix instrumented; ESP-IDF-style ring does not clear the P4 gate

- State: A1 remains `hardware partial`.  The diagnostic core is now built
  reproducibly with `P4_A1_DIAGNOSTIC=1`; it is a development-only core and
  not a normal-boot artifact.
- Manual baseline evidence: Core SHA-256
  `d929d5bbc56d20b72579228981483a9616ae99fc047e7e679a6c89f7798cda8d`
  (151,504 bytes) was verified at `ota_0` offset `0x20000`.  With manual
  CMD12 and BSP SHA-256 `4be7e8b8bca718453b4e1ca4791646cd7487b34a1d62756b89931ddff3abe16b`,
  the actual UART matrix confirms correct 1/2/32/128-sector transfers at
  LBA 0 and through READ64 at the card end, plus CMD12/CMD13 TRAN recovery.
  The LBA-2048 two-, 32- and 128-sector reads all repeat LBA 2048 at byte
  512; the independent CMD17 expected word is `0x41615252`, while CMD18
  returns `0x429058eb`.  The 100-Hz heartbeat stays live.
- Ring experiment: BSP SHA-256
  `5a7a54752065e2d2f86e6dff299ea577e259027fee2ab549200f9eb15538981c`
  (306,088 bytes), using four reusable 4-KiB receive descriptors, polled
  RI/NI refill, poll-demand and hardware auto-stop, was verified at
  `0x820000`.  Two sectors at LBA 2048 still repeat the first block.  Every
  32- and 128-sector transfer instead ends with `RINTSTS=0x00000420`
  (`HTO|RXDR`) before `DATA_OVER`, although all submitted descriptors have
  released OWN.  Generic CMD12/CMD13 recovery returns the card to TRAN and
  the heartbeat remains live.
- Interpretation: the local vendor transaction structure is necessary
  context but is not sufficient to resolve the D1001/P4 data progression
  fault.  Do not run stress, injected faults or filesystem work.  Restore the
  manual baseline after this documented failed experiment; the next evidence
  must distinguish CIU/card wire behaviour from a controller state-machine
  issue (logic-analyser or P4 controller trace).

### 2026-08-22 - A1 standalone ESP-IDF host control is blocked before SDMMC

- State: A1 remains `hardware partial`.  This is not SDMMC evidence: the
  official-host control cannot enter `app_main()` on this D1001's P4 revision
  1.3, so it never initializes the card or issues CMD17/CMD18.
- Control source: `tools/idf-sdmmc-control` is a minimal ESP-IDF v6.0.1
  application.  It uses the unmodified Espressif SDMMC host API, board power
  only (LDO4 and GPIO46), one-bit/400-kHz mode, independent CMD17 baselines
  and CMD18 comparisons at LBA 2048 for 2/32/128 sectors.  It contains no
  filesystem, write, erase or format API.  Its explicit target configuration
  is P4 revisions 1.0--1.99 at 360 MHz; IDF's default is revision 3.1 and is
  ABI-incompatible with this chip generation.
- Artifact/procedure: ESP-IDF v6.0.1 (`8c19b156`, its working tree is locally
  dirty only in bootloader sources) produced a 205,152-byte control app,
  SHA-256 `21a83afebcb9099cc316810c867ea754d3fcc12afc9e22c47491179d85a7531a`.
  The independent, clean local ESP-IDF v6.0 (`662a3be3`) produced a
  203,680-byte app, SHA-256
  `d5941bbca97f1a17ad6b2f653b539af25166b20109a59e57fe6f424acb0519ef`.
  Esptool v5.3.0 wrote and independently verified each only at `ota_0` offset
  `0x20000`.  Each image reports min chip rev 1.0, max 1.99 and runs at
  360 MHz.  Neither generated IDF bootloader/partition image nor `arosbsp`,
  the board's bootloader, partition table, NVS or SD media was written.
- Observed result: both IDF v6.0.1 and the clean v6.0 app identically ended
  their UART log at `sleep_gpio: Enable automatic switching of GPIO sleep
  configuration`; no `idf-sdctrl` message appeared.  With the serial reset
  line released, v6.0.1 then remained silent for 35 seconds, proving a
  pre-`app_main()` early-start hang rather than a missed log or an SDMMC
  failure.  ESP-IDF v6 explicitly says P4 revisions below 3.0 differ
  substantially from revision 3.x; neither locally available v6 host runtime
  is therefore a valid live control for this pre-3.0 D1001.
- Restoration: the AROS diagnostic core SHA-256
  `d929d5bbc56d20b72579228981483a9616ae99fc047e7e679a6c89f7798cda8d`
  (151,504 bytes) was immediately written and independently verified at
  `ota_0` `0x20000`.  Its complete boot confirms the existing manual-CMD12
  baseline, including the known byte-512 LBA-2048 repeat, CMD12/CMD13
  recovery and continuing 100-Hz heartbeat.  The stable BSP remains
  `4be7e8b8bca718453b4e1ca4791646cd7487b34a1d62756b89931ddff3abe16b` at
  `arosbsp` `0x820000`.
- Next evidence: do not interpret the failed runtime as an AROS or SD-card
  result.  A controller/CMD-D0 wire trace or an Espressif-supported pre-3.0
  P4 host runtime is needed before further A1 implementation churn.

### 2026-08-22 - A1 buffered controller trace prepared

- State: A1 remains `hardware partial`; this change adds observation only and
  is not a new transfer implementation.
- Scope: `P4_A1_DIAGNOSTIC=1` now enables `P4_SDCARD_TRACE=1`.  The backend
  buffers (rather than prints in the polling loop) up to twelve snapshots for
  precisely CMD18, LBA 2048, two sectors.  Each contains raw/masked interrupt
  state, IDMAC state, controller/FIFO status, CIU (`TCBCNT`) and memory/FIFO
  (`TBBCNT`) byte counters, current descriptor/buffer addresses, descriptor
  ownership and the first word of each target block.  It emits the snapshots
  only after success, failure or timeout, so it does not alter the observed
  transfer timing.
- Decision rule: distinct CIU/BIU counts of 1024 with a repeated second target
  word points below the card wire at IDMAC/cache/memory handoff; CIU progress
  stopping at 512 or an error before the second block points at the CIU/card
  path.  A trace that shows neither distinction requires an external CMD/D0
  wire trace before any further A1 transport change.
- Safety: the trace neither writes a controller data FIFO nor changes the
  command denylist, clock, descriptor layout or stop/recovery sequence.  The
  pending D1001 run is limited to the existing read-only diagnostic core in
  `ota_0` and BSP package in `arosbsp`.

### 2026-08-22 - A1 controller trace exposes premature CMD18 completion

- State: A1 remains `hardware partial`; the trace has produced a concrete
  controller ordering correction, still subject to a D1001 retest.
- Hardware evidence: with Core SHA-256
  `d929d5bbc56d20b72579228981483a9616ae99fc047e7e679a6c89f7798cda8d` at
  `ota_0` and trace BSP SHA-256
  `e8824086e084c0b580fcac587143c23aabc4b1a8a716a474cd475e08e5009b33`
  (305,684 bytes) at `arosbsp`, both independently verified by esptool, the
  LBA-2048 two-block CMD18 trace reached `TBBCNT=1024` and released both
  descriptors while `TCBCNT` was only 941 and `RINTSTS.DATA_OVER` was clear.
  After cache invalidation both destination blocks contained `0x429058eb`.
  CMD12/CMD13 later recovered TRAN and the heartbeat remained live.  No SD
  write command or unauthorized flash range was used.
- Interpretation: the old backend treated descriptor release as transfer
  completion, reset BMOD/FIFO and returned to the generic CMD12 owner before
  the CIU had completed the card-side transfer.  The trace therefore does not
  yet identify a card-wire duplicate; it identifies a premature local
  completion boundary that can itself truncate or replay the tail.
- Correction under test: descriptor completion is now only remembered.
  IDMAC/FIFO state remains armed until `DATA_OVER`; only then is the receive
  range invalidated and the successful generic CMD12 path entered.  This
  preserves the existing read-only descriptor format, command denylist,
  manual stop ownership and finite timeout/recovery paths.
- Next acceptance evidence: rebuild, flash only the BSP package, and rerun
  the same matrix.  The two-sector LBA-2048 trace must show CIU completion and
  `DATA_OVER` before CMD12; only a distinct second block permits any wider A1
  testing.  Otherwise retain the evidence and escalate to CMD/D0 wire trace.

### 2026-08-22 - A1 DATA_OVER completion trial rejected; manual baseline restored

- State: A1 remains `hardware partial`.  The controller telemetry is now
  exhausted for the present manual-CMD12 transaction model; no A1 acceptance
  point has been gained.
- Artifact/procedure: the diagnostic core SHA-256
  `d929d5bbc56d20b72579228981483a9616ae99fc047e7e679a6c89f7798cda8d`
  remained at `ota_0`.  BSP PKG SHA-256
  `082866b0d964fc0d7775aa24fed34a52f43e56a2bd34e5827a4c808012c4461d`
  (305,148 bytes) deferred IDMAC completion until `DATA_OVER`; it was flashed
  and independently verified only at `arosbsp` offset `0x820000`.
- Observed result: for CMD18 LBA 2048, two sectors, `TBBCNT` reached 1024 and
  both 512-byte descriptors released OWN, but the captured CIU count advanced
  only to 760 across the 12 buffered samples and `DATA_OVER` never latched.
  The request timed out rather than invalidating/comparing data.  At 32 and
  128 sectors, including otherwise-working zero and high-LBA cases, the same
  wait reached `HTO|RXDR` (`0x00000420`) before `DATA_OVER`.  Bounded
  CMD12/CMD13 recovery returned TRAN each time and the 100-Hz heartbeat stayed
  live.
- Decision: waiting for `DATA_OVER` is incompatible with the current manual
  CMD12 flow; it turns previously bounded reads into timeouts.  The driver is
  restored to its prior descriptor-complete handoff, which immediately lets
  the generic layer issue CMD12 and retains the safe, reproducible byte-512
  duplicate at non-zero LBA 2048.  The buffered trace code remains
  diagnostic-only and has no runtime effect outside `P4_A1_DIAGNOSTIC=1`.
- Restored device state: the rebuilt diagnostic BSP PKG is 304,876 bytes,
  SHA-256 `221394c729c0f7ac8390f2a0679cc38ddc0dc4d97d7e446c21787383c52127fd`.
  It was flashed and independently `verify-flash`-verified at `0x820000`,
  then booted on the D1001.  LBA 0 and READ64 high-LBA 1/2/32/128-sector
  comparisons pass, CMD12/CMD13 confirms TRAN, and LBA 2048 consistently
  repeats block one at byte 512 for 2/32/128 sectors.  The resulting matrix
  fails only at that known A1 progression gate; no SD write was attempted.
- Safety: no SD-media write command was issued.  The sole flash change was
  the authorized BSP range; bootloader, partition table, otadata, NVS and
  `storage` were untouched.
- Next safe step: capture the physical CMD and DAT0/D0 transaction (including
  the CMD18/CMD12 interval) or establish a working Espressif-supported P4 host
  runtime on this P4 v1.3 before changing A1 transport code again.  Do not
  replace CMD18 with CMD17, run filesystem work, or claim a clean boot.

### 2026-08-22 - A1 D1001 wire-capture points established from schematic

- State: A1 remains `hardware partial`; this is a capture plan, not hardware
  evidence.
- Provenance: Seeed's published D1001 schematic, sheet 7 (`USB&MicroSD`),
  maps the socket nets to the P4 as `SD_CMD`/GPIO44, `SD_CLK`/GPIO43 and
  `SD_D0`/GPIO39.  The socket itself is J1: CMD is T3, clock T5 and DAT0 T7.
  The same nets have less intrusive board-side probe locations: CMD at R51 or
  D7, CLK at R52 or D6, and DAT0 at R53 or D10.  The remaining one-bit-test
  data lanes are unused; D1/D2/D3 must not be mistaken for the active DAT0.
- Required capture: use a high-impedance logic-analyser probe and a short
  ground lead at the socket shield/nearby ground.  Sample CMD, CLK and DAT0
  at at least 8 MHz (20 samples per 400-kHz clock), at 3.3-V logic threshold,
  while the existing read-only diagnostic boot runs its LBA-2048 CMD18,
  two-sector comparison.  Retain the interval from CMD18 (`0x52`) through
  CMD12 (`0x4c`), including both DAT0 data blocks and their CRC/end bits.
- Decision rule: a distinct second DAT0 block matching the card's known CMD17
  LBA-2049 reference localises the defect to the P4 receive/controller path;
  an on-wire repeat localises it to the card transaction or host command path.
  Either outcome is actionable and avoids speculative driver changes.
- Safety: this is passive observation only.  Do not probe socket contacts
  directly, short adjacent pads, or alter card power.  The current firmware
  remains read-only and sends no SD-media write command.

### 2026-08-22 - A1 exact D1001 host-driver port started

- State: A1 remains `hardware partial`; the port is `in development` and has
  no hardware-acceptance claim.
- Research/provenance: Seeed's public `reTerminal-D1001` BSP revision
  `5074d3b2f45626b261298e305aaf792036febc5a` states ESP-IDF v5.4.2 and mounts
  the D1001 MicroSD through slot 0.  Its `bsp_sdcard_mount()` selects the
  native slot, on-chip LDO channel 4 and the standard SDMMC host; its FAT
  formatting option is explicitly out of scope and will not be copied.
  ESP-IDF v5.4.2 source revision
  `f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0` supplies the exact controller
  model in `components/esp_driver_sdmmc/src/sdmmc_host.c` and
  `sdmmc_transaction.c`.
- Gap identified: the current AROS backend has bounded receive descriptors
  and a polled approximation of RI/NI, but not the host driver's complete
  SDMMC interrupt/event handoff and DMA-done-to-`DATA_OVER` state machine.
  The previous four-by-4-KiB trial therefore did not constitute this full
  host-model port.
- Planned bounded change: add only a read-side event latch and P4 SDMMC IRQ
  route, mirror the v5.4.2 four-by-4-KiB descriptor refill/state transitions,
  use controller auto-stop for CMD18, and make the existing generic stop path
  confirm TRAN without issuing a duplicate physical CMD12.  The first AROS
  pass performs the ISR's acknowledge/refill ordering synchronously inside
  the serialized backend; a real CLIC registration is explicitly deferred
  until the dynamic `sdcard.device` has a safe platform IRQ binding.  Keep the test at
  one-bit/400 kHz; do not copy the Seeed BSP's four-bit/high-speed, mount or
  formatting behaviour.
- Safety: no hardware or SD-media change has occurred for this research.  The
  eventual test remains limited to the existing diagnostic core in `ota_0`
  and a verified BSP package at `arosbsp`; it introduces no media write path.
- Next safe step: compile the isolated AROS host-model port, audit that the
  IRQ only latches/acknowledges events, then run its LBA-2048 2/32/128-sector
  matrix on the D1001 before any stress, fault or filesystem test.

### 2026-08-22 - A1 v5.4.2 transaction model builds; D1001 test pending

- State: A1 remains `hardware partial`.  This is `build verified` only for
  the new implementation; it makes no hardware-acceptance claim.
- Implementation: the AROS receive path now uses the ESP-IDF v5.4.2 model's
  four reusable 4-KiB IDMAC descriptors, Normal-Interrupt descriptor refill
  and poll-demand, CMD18 `SEND_AUTO_STOP`, and the explicit
  `SENDING_CMD -> SENDING_DATA -> BUSY -> DATA_OVER` transition.  It does not
  reset DMA/FIFO merely because a descriptor releases.  Since the resident
  device has no safe dynamic CLIC binding yet, the backend synchronously
  latches and acknowledges the same controller status in task context; no
  IRQ route is claimed or enabled.
- Artifact: `gmake kernel-package-esp32p4-riscv P4_A1_DIAGNOSTIC=1` completed
  with BSP package SHA-256
  `c669842b878775b1f931f31e7ee04ce6443118e3978d9ba4902451f2c18753d1`
  (304,332 bytes).  `git diff --check` passed.  No D1001 flash or SD-media
  operation has yet occurred for this artifact.
- Safety: the command denylist, one-bit/400-kHz configuration and absence of
  all transmit/FIFO-write paths remain.  The following test is limited to the
  standing-authorized `arosbsp` BSP range at `0x820000`; it will be verified
  before and after the write and will not touch the card's media.
- Remaining risk: the previous polling ring trial failed, so only a fresh
  D1001 matrix can determine whether the full event ordering fixes CIU/data
  progression.  CLIC delivery remains a separate, unstarted integration task.
- Next safe step: flash and independently verify this BSP package, collect
  the complete diagnostic UART log, then compare LBA-2048 CMD18 2/32/128
  sector results before any repetition, fault injection or filesystem work.

### 2026-08-22 - A1 first v5.4.2-model smoke test caught host-register regression

- State: A1 remains `hardware partial`; no CMD18 acceptance point was reached.
- Artifact/procedure: BSP SHA-256
  `c669842b878775b1f931f31e7ee04ce6443118e3978d9ba4902451f2c18753d1`
  (304,332 bytes) was written only to `arosbsp` at `0x820000` on the D1001
  (ESP32-P4 v1.3, MAC `e8:f6:0a:e0:46:4c`) and passed both the write-time
  hash check and a separate `esptool verify-flash`.
- Observed UART: after `slot 0 polling IDMAC receive`, CMD51 failed with
  `raw=00000000`, then CMD55/clock change failed and the subsequent CMD17
  smoke reads returned zero actual bytes.  The 100-Hz CLIC heartbeat remained
  live for fifteen intervals, so this is a storage-backend regression rather
  than a board boot failure.
- Cause/correction: source audit found that the host-model edit accidentally
  dropped `BYTCNT = data_len` for all data commands, including the 8-byte
  CMD51 SCR read.  The assignment has been restored before rebuilding.  No
  SD write command was issued and no storage partition was touched.
- Next safe step: rebuild the corrected BSP, verify its new hash, replace
  only `0x820000`, and require the CMD17 baseline to pass before interpreting
  any CMD18 result.

### 2026-08-22 - A1 complete v5.4.2 host-model trial rejected; manual baseline restored

- State: A1 remains `hardware partial`; none of its acceptance points has
  advanced.  The full-model source trial is recorded as rejected, not hidden.
- Tested artifacts: after the first `BYTCNT` correction, BSP SHA-256
  `8120724ce7175fa28035cd8ed2bae0b22b34f2cd16007956933dc990186a989a`
  (304,352 bytes) and its PIO `DATA_OVER` correction
  `cc18c245519cba2ca379054f9ec96b9f2a8d960194d56bc3a1f04e164e4f72d7`
  (304,408 bytes) each built cleanly, were written only at `0x820000`, and
  passed both the esptool write-time and independent `verify-flash` checks.
  The IDF-DMA-watermark variant
  `519336469db00d60eae46614fd64e1354147ef10eb16498a6d67d52ac8f31af6`
  (304,420 bytes) was verified by the same procedure.
- Observed UART: two-sector CMD18 completes and auto-stop/CMD13 reaches
  `TRAN` at LBA 0 and through READ64, but LBA 2048 still repeats byte 512.
  At every 32- or 128-sector test, at low, LBA-2048 and high READ64 addresses,
  all four 4-KiB descriptors lose OWN while `RINTSTS=00000020` (`RXDR`),
  `IDSTS=0` and `STATUS=04009509` (a full receive FIFO) remain; `DATA_OVER`
  never occurs.  Bounded CMD12/CMD13 recovery succeeds and the 100-Hz
  heartbeat remains live.  Changing the IDMAC receive watermark from the old
  PIO value to the ESP-IDF reset value did not change that result.
- Restoration: the manual 512-byte descriptor/CMD12 path was rebuilt as BSP
  SHA-256 `5630889d043de01254a93e7a780f9b228cd502ea9e7d74cf480f7235c2f20077`
  (303,932 bytes), written only to `arosbsp` `0x820000`, and independently
  `verify-flash`-verified.  Its D1001 matrix again passes 1/2/32/128 sectors
  at LBA 0 and near card end through READ64, while only non-zero LBA-2048
  CMD18 repeats block one.  Heartbeats remained live throughout.
- Safety: every experiment used one-bit/400-kHz read-only transfers, retained
  the command denylist and no FIFO transmit path, and touched neither SD
  media nor bootloader, partition table, otadata, NVS or `storage`.
- Remaining blocker / next safe step: this exact vendor-host model is not the
  missing abstraction.  Capture CMD, CLK and DAT0 through the LBA-2048 CMD18
  to CMD12 interval as specified above, or run the control on an
  Espressif-supported pre-3.0 P4 runtime, before altering A1 transport again.

### 2026-08-22 - A1 test-card ground truth invalidates the LBA-0 baseline

- State change: A1 stays `hardware partial`, but the reason changes.  Several
  earlier conclusions in this log are hereby revoked, not deleted.  They are
  left in place above with this entry as their correction.
- Hardware: the D1001 test card, read in a host card reader with the board
  powered down.  No AROS artifact was built, flashed or run for this entry.
- Card identity: 127,865,454,592 bytes, exactly 249,737,216 512-byte units,
  which is the same unit count the AROS boot log reports and is how the card
  was identified.  MBR scheme, one FAT32 partition at partition offset 2048
  sectors.  The MBR's single entry is type `0x0b`, start 2048, 249,735,168
  sectors, and LBA 0 carries `0x55 0xaa` at offsets 510/511.
- Procedure: `diskutil unmountDisk`, then a read-only
  `dd if=/dev/rdisk21 bs=512 count=4096`.  Nothing was written to the card.
  The first 2 MiB have SHA-256
  `3aeaff747c68f8c087cda1e3a9fcd877e323bb7ad238f9462efd5d9322fa6e45`.  Full
  hash tables are in `sdcard/test-card-reference.md`.
- Observed result, and the correction it forces:
  1. LBA 0 holds a valid MBR whose FNV-1a-32 is `0xdebe99c1`.  This log
     records `0x4d7705c5` for that sector.  `0x4d7705c5` is the hash of 512
     zero bytes, and `0x1f116dc5`, recorded for the two-sector LBA-0 CMD18
     cell, is the hash of 1024 zero bytes.  The driver returned zeroes where
     the card holds a partition table, the CMD17 baseline returned the same
     zeroes, and the matrix reported a match.
  2. The defect is therefore not confined to CMD18.  The single-block path is
     wrong at LBA 0 as well, and the CMD17 baseline is not an independent
     reference for anything.
  3. There is no LBA dependence left to explain.  No cell of the matrix has
     ever shown CMD18 placing a second block correctly at any address.  The
     LBA-0 cells proved coverage only, that the 0xa5 sentinel was replaced,
     because the harness pre-fills its receive buffer.
  4. 4086 of the first 4096 sectors are zero-filled.  A misdirected read
     lands on zeroes with 99.8 per cent probability, which is why no amount
     of transport rework made the fault move.
  5. LBA 2054 is byte-identical to LBA 2048; it is the FAT32 backup boot
     sector.  A repeated first block and a misdirected read of 2054 cannot be
     distinguished by content, so the planned 2053 straddle case loses its
     discriminating power and 2047/2049 must carry it instead.
  6. CMD17 at LBA 2048 and 2049 is correct.  The recorded first words
     `0x429058eb` and `0x41615252` are the card's real VBR and FSInfo
     signatures, so addressing works there and the byte-512 mismatch at LBA
     2048 is a genuine fault, not an artifact of the reference.
- Superseded conclusions: every statement that low or high LBAs pass; the
  inference that the fault is specific to non-zero LBAs; and the resulting
  next step, a passive CMD/CLK/DAT0 capture, which was chosen to explain a
  selectivity that does not exist.
- Safety: read-only host access to the card, no write, no flash operation, no
  board power.  The card was left unmounted, so the volatile FAT32 structures
  it carries are in the state the next board run will see.
- Remaining risk: LBA 2049, 2080 and 2081 are FSInfo and FAT sectors and were
  observed changing between a mounted and an unmounted read.  Mounting this
  card on a host again invalidates those three reference hashes.  There is
  also no reference for the card's last sectors, where the READ64 cells read;
  the card was removed before that range could be captured.
- Next safe step: make the matrix compare against the card rather than
  against itself, and probe whether the LBA-0 fault is address dependent or
  an artifact of being the run's first data transfer.

### 2026-08-22 - A1 CTRL.use_internal_dma is defined on the wrong bit

- State: A1 stays `hardware partial`.  This is a source finding with a
  verified reference, deliberately left unfixed for now.
- Finding: `sdcard/sdcard_esp32p4_intern.h` defines
  `P4SD_CTRL_USE_INTERNAL_DMA` as bit 26.  The controller places it on bit
  25.
- Reference: ESP-IDF's own register description for this peripheral,
  `components/soc/esp32p4/register/hw_ver1/soc/sdmmc_struct.h`, lays the CTRL
  register out as `card_voltage_a:4` on bits 16 to 19, `card_voltage_b:4` on
  20 to 23, `enable_od_pullup:1` on 24, `use_internal_dma:1` on 25 and
  `reserved3:6` from 26 up.  Bit 26 is reserved.
- Consequence: the backend has never selected the internal DMAC through
  CTRL.  It ran the IDMAC through `BMOD.DE` alone while setting a reserved
  CTRL bit, a combination ESP-IDF never produces.  Every IDMAC number in this
  log was measured in that state.
- What this does not explain: the byte-512 mismatch was first reproduced on
  the pure CPU PIO path, before any IDMAC code existed
  (evidence of 2026-08-22, `BYTCNT=0` PIO run).  A CTRL bit that only gates
  DMA cannot be the cause of a fault that predates the DMA path.  It remains
  a strong candidate for the 16-KiB stall with a full receive FIFO, and for
  the `TBBCNT` over `TCBCNT` inversion in the buffered trace.
- Why it is not being changed in the same step: the rejected v5.4.2 model
  trial bundled several changes and lost the ability to attribute any result.
  The harness correction below must be measured on the driver as it stands,
  then this one character changes alone.
- Safety: no code, flash or media change for this entry.
- Next safe step: after the card-referenced matrix has produced a baseline,
  change bit 26 to bit 25 as an isolated BSP build and rerun the same matrix.

### 2026-08-22 - A1 matrix rebuilt against the card, build verified

- State change: A1 stays `hardware partial`.  This entry is `build verified`
  only; no D1001 run has taken place for this artifact and it makes no
  hardware-acceptance claim.
- Source identity: baseline commit `7bbfd7b6d1` plus the uncommitted A1 diff,
  which now also touches `kernel/kernel_startup.c` and adds
  `sdcard/test-card-reference.md`.  No backend, generic-layer or clock change
  is included, on purpose: the harness has to be measured on the driver as it
  currently stands.
- Artifact: `gmake kernel-esp32p4-riscv P4_A1_DIAGNOSTIC=1
  P4_LDSCRIPT=ldscript-xip.lds` produced a diagnostic XIP core of 152,064
  bytes, SHA-256
  `ece66371fa922f3f4b62222a405a31679041bdc4c37db4c3d3f85db467c11716`.  The
  `.sramtext` residency check passed on the linked ELF.  `git diff --check`
  passed.  The BSP package is unchanged, so the existing `arosbsp` content
  SHA-256 `5630889d043de01254a93e7a780f9b228cd502ea9e7d74cf480f7235c2f20077`
  stays in place and only `ota_0` needs writing.
- Harness changes:
  1. Card-referenced acceptance.  `krnP4SDCardCompareBlocks()` now carries
     the card's own FNV-1a-32 values as compiled-in tables and accepts a cell
     only when both CMD17 and CMD18 match the card.  Where no reference
     exists, in particular the READ64 cells near the card end, the cell falls
     back to self-comparison and prints that it did so.
  2. Block identification.  Each 512-byte block is hashed and named against
     the card's sector table, so a report reads "block 1 holds LBA 2048
     again" rather than "block 1 differs".  Because LBA 2054 has the same
     content as 2048, both are printed when a hash matches them.  An
     untouched sentinel block and a zero sector are named as such.
  3. Blindness warning.  A cell whose own reference blocks are all identical
     cannot detect a repeated block; the harness now computes that and says
     so instead of reporting a pass.
  4. Separate, aligned buffers.  The reference and receive buffers are two
     allocations, each rounded up to a 64-byte cache line, and both addresses
     are printed.  Previously they were halves of one 16-byte-aligned block
     and could share a cache line.
  5. The broken CRC print is gone.  Its loop started at `i - (i & 511)`,
     which for the observed first-difference index 512 equals 512, so the
     loop never ran and every printed CRC was zero.
  6. The whole-buffer CMD18 hash is now printed unconditionally.  It used to
     be discarded on failure, which is exactly when it is needed.
  7. Address probe.  A new `krnP4SDCardAddressProbe()` runs before the
     single-sector sweep and reads LBA 2048, 0, 0, 1, 2048, 2083 in that
     order, so LBA 0 is no longer the run's first data transfer.  This
     separates an address-dependent fault from a first-transfer artifact.
  8. Address list.  The matrix now runs 2048, 0, 2047, 2049 and 2083.  2047
     and 2049 straddle the partition start in both directions and invert the
     expected signature, so a repeated first block shows up as the opposite
     mismatch rather than the same one.  2083 is a FAT sector whose
     neighbours genuinely differ.
  9. `krnP4SDCardReadSector()` and the SRAM probe buffer are aligned to 64
     bytes and the single-sector path now also states the card's value.
- Table verification: all 32 compiled-in constants, ten sector hashes, twenty
  range hashes and the two payload constants, were checked against the
  captured card image; no mismatch.
- Environment note for reproduction: eight tool symlinks under
  `bin/darwin-aarch64/tools` in the build tree pointed at a build directory
  that no longer exists, which made the post-link residency check fail with a
  usage error rather than a check result.  They were repointed at the local
  `crosstools` copies of the same binaries.  `esptool` is not on the default
  path either; the build needs
  `ESPTOOL=/Users/fabian/.espressif/tools/python/v6.0/venv/bin/esptool`
  (v5.3.dev3).  Neither is a source change.
- Safety: read-only diagnostics only.  The command denylist, the absence of
  any FIFO transmit path and the one-bit/400-kHz configuration are untouched,
  and no SD-media write exists in this build.  Nothing has been flashed for
  this entry.
- Remaining risk: the test card must not be mounted on a host again before
  the run, because FSInfo and FAT sectors 2049, 2080 and 2081 change when it
  is, and three of the compiled-in hashes would then be stale.  The card was
  removed from the reader unmounted, so the current state matches the tables.
- Next safe step: reinsert the card in the D1001, write only this core to
  `ota_0` at `0x20000`, verify it independently with esptool, and capture the
  full diagnostic log.  Read the address probe first: if LBA 0 is correct once
  it is not the first transfer, the fault is a first-transfer artifact; if it
  is still zeroes, the fault is address dependent.  Only then interpret the
  CMD18 cells, and change nothing in the driver until that log exists.

### 2026-08-22 - A1 root cause identified: the FIFO read side repeats one word

- State: A1 stays `hardware partial`, but the fault is now identified and
  arithmetically proven rather than described.  No fix has been applied.
- Artifact/procedure: the card-referenced diagnostic XIP core, 152,064 bytes,
  SHA-256
  `ece66371fa922f3f4b62222a405a31679041bdc4c37db4c3d3f85db467c11716`, was
  written only to `ota_0` at `0x20000` on the D1001 (ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`, USB-Serial/JTAG, dual core at 400 MHz) and passed both
  the write-time hash check and an independent `esptool verify-flash`.  The
  BSP package at `arosbsp` was not touched and still holds
  `5630889d043de01254a93e7a780f9b228cd502ea9e7d74cf480f7235c2f20077`.  The
  card was reinserted unmounted, so the compiled-in reference tables match
  its state.  21,645 bytes of diagnostic output were captured.
- Observed result: every sector the driver returns consists of 128 copies of
  that sector's own first 32-bit word.  Exactly one word per block is real.
- Proof: eleven independent predictions, each computed from the captured card
  image and compared against the log, all matched to the bit.
  * Single sectors.  LBA 2048 returned `0x263f72c5`, which is FNV-1a-32 over
    128 copies of `0x429058eb`, the card's real first word there; the card's
    true sector hash is `0x730d1cbd`.  LBA 2049 returned `0x40d152c5`, 128
    copies of `0x41615252`.  LBA 0, 2050 and 2083 returned the zero-payload
    hash `0x4d7705c5` because their first word is zero, even though LBA 0
    holds an MBR and LBA 2083 a populated FAT sector.
  * Multi-sector CMD17 baselines.  Predicted `0x9cdc27c5`, `0x60df11c5` and
    `0x91616bc5` for 2, 32 and 128 sectors from LBA 2048; the log reports
    exactly those.
  * CMD18.  Predicted `0x824a47c5`, `0x86f73dc5` and `0x58871dc5` on the
    assumption that every block carries block zero's pattern; the log reports
    exactly those, and names every block from 1 upwards as a repeat of block
    zero.
- Why this explains the whole history:
  1. `nonzero 512` at LBA 2048 against 133 non-zero bytes on the card is the
     word-replication signature, not corruption.
  2. Every earlier "correct" observation was a first word or a zero sector.
     The recorded `0x429058eb` at LBA 2048 and `0x41615252` at LBA 2049 are
     the one real word of each block; the harness printed four bytes and
     stopped.
  3. `STATUS=0x04009509` decodes to `FIFO_COUNT = 512`, the full documented
     depth, while the card had delivered almost nothing.  A read side that
     believes a nearly empty FIFO is full will pop the last value repeatedly.
  4. That also produces `TBBCNT=1024` against `TCBCNT=941`: the BIU counts
     word reads that never corresponded to received bytes.
  5. It explains why PIO and IDMAC fail identically.  Both depend on the same
     fill-level comparator, the CPU through `STATUS.FIFO_COUNT` and the DMAC
     through the `FIFOTH.rx_wmark` request.
  6. It explains the 16-KiB stall without `DATA_OVER`.  The register
     description states that in DMA mode the end-of-packet flush is a
     precondition for Data Transfer Done, and a read side stuck on a stale
     pointer never completes that flush.
- Divergence from Espressif that matches the symptom: `SendCmd()` issues a
  FIFO reset before every data command (`sdcard/sdcard_esp32p4_bus.c:891`).
  ESP-IDF sets `ctrl.fifo_reset` in exactly two places, once in
  `s_module_reset()` during controller init
  (`components/esp_driver_sdmmc/src/sd_host_sdmmc.c:922`) and once in
  `process_data_status()` on a data error
  (`.../sd_trans_sdmmc.c:319`).  It never resets the FIFO on a healthy
  transfer.  `p4sd_reset_fifo()` then waits only for the self-clearing AHB
  bit, while the register description requires two system clocks plus a
  two-card-clock synchronisation before the FIFO pointers are valid again,
  which is 5 us at 400 kHz.  BLKSIZ, BYTCNT, CTRL, BMOD and the command start
  all follow well inside that window.
- Acceptance points: none passed.  The matrix now fails honestly, including
  the four READ64 cells, which report themselves as self-comparisons and
  print the warning that all their reference blocks are identical.  The
  address probe answered its question: LBA 0 is wrong both as the first data
  transfer and after other reads, so it is not a first-transfer artifact.
  CMD12/CMD13 recovery and the 100-Hz heartbeat stayed live throughout.
- Safety: read-only throughout.  No SD-media write command exists in this
  build, the command denylist and the absence of a FIFO transmit path are
  intact, and the only flash range written was the authorized `ota_0`.
- Remaining risk: the mechanism above is inferred from the register
  description plus the symptom, not yet from a measurement of `FIFO_COUNT`
  immediately after a reset.  The correction must be tested in two variants,
  because dropping the per-command reset and waiting correctly after a reset
  are different changes with different failure modes.
- Next safe step: one isolated BSP build that removes the per-command FIFO
  reset and keeps it only in the error path, matching ESP-IDF.  Log
  `STATUS.FIFO_COUNT` and `FIFO_EMPTY` immediately after every reset and
  before every drain, so the mechanism is measured rather than assumed.  Do
  not combine this with the `CTRL.use_internal_dma` bit correction; that is
  the step after it.

### 2026-08-22 - A1 FIFO settle correction rejected; the CPU FIFO read does not pop

- State: A1 stays `hardware partial`.  The correction under test failed, and
  its instrumentation replaced the inferred mechanism with a measured one.
- Artifact/procedure: BSP package 305,192 bytes, SHA-256
  `ff584242efbfd1714b558868cc6b049cb4e670e0cec596f6fb26488ea79d54e9`, built
  with `gmake kernel-package-esp32p4-riscv P4_A1_DIAGNOSTIC=1`, written only
  to `arosbsp` at `0x820000` and independently `verify-flash`-verified.  The
  core in `ota_0` was unchanged at
  `ece66371fa922f3f4b62222a405a31679041bdc4c37db4c3d3f85db467c11716`.  23,460
  bytes captured.
- Change under test: `p4sd_reset_fifo()` now waits `P4SD_FIFO_RESET_SETTLE_US`
  (20 us, eight card clocks at 400 kHz) after the self-clearing CTRL bit,
  because the register description requires a two-card-clock synchronisation
  the old code did not wait for.  Bounded observations of the FIFO state were
  added after every reset and before every drain.
- Result: rejected.  Every hash in the matrix is bit-identical to the previous
  run, `0x263f72c5` at LBA 2048 and the same four range hashes.  The delay
  changes nothing.
- What the instrumentation proved instead:
  * After a reset the FIFO is genuinely clean: `count=0 empty=1 full=0`.
    The reset was not the fault.
  * At the first drain of a 512-byte read the controller reports
    `claims 128 words, ciu=512, biu=0`.  The fill level is correct and
    `TCBCNT=512` proves the card delivered a complete, correctly addressed
    block.  The card path, addressing, timing and CRC are all sound.
  * After the loop has performed its 128 reads and copied 512 bytes, the very
    next observation still reports `claims 128 words` and `biu=0`.  The fill
    level did not drop and the BIU counter did not move.
  * Therefore the 128 reads of `P4SD_BUFFIFO` never reached the FIFO logic.
    The first read returns the block's real first word; the remaining 127
    return the same value again, which is exactly the 128-copy pattern.
- Compiler excluded: `riscv-esp32p4-aros-objdump -d` on
  `sdcard_esp32p4_bus.o` shows `lw a2,0(a6)` with `a6 = 0x50083200` inside the
  outer loop body at `.L6`, one load per iteration.  The volatile access is
  not hoisted, so the reads are issued.
- Espressif comparison: ESP-IDF has no CPU FIFO read path for SDMMC at all.
  Its driver moves every data byte with the IDMAC; `SDHOST_BUFFIFO_REG` is
  never read by software.  The AROS backend is therefore built on a transfer
  mode the vendor never exercises on this controller.
- Retained: the settle delay is spec-conforming and harmless, costs 20 us per
  data command, and is kept.  A caveat on its own evidence: the FIFO state was
  only sampled after the delay, never before it, so whether the pointers were
  invalid beforehand remains unmeasured.  That question is now moot for the
  fault but the measurement gap should be recorded.
- Safety: read-only throughout, denylist and absent transmit path intact,
  only the authorized `arosbsp` range written, no SD-media write.
- Next safe step: correct `CTRL.use_internal_dma` from bit 26 to bit 25 as a
  single isolated change and retest.  The hypothesis it tests is specific: if
  the internal DMAC was never selected on the controller side, then the IDMAC
  advanced descriptors through `BMOD.DE` while the FIFO-to-memory data path
  was never coupled, which would produce the same word-repeat pattern on the
  CMD18 path that the broken CPU path produces on CMD17.  If that is right,
  the DMA path becomes correct and the remaining work is to route CMD17 and
  CMD51 through the IDMAC as well, as ESP-IDF does.

### 2026-08-22 - A1 CMD18 reads correct data for the first time

- State: A1 stays `hardware partial`, but for the first time a CMD18 transfer
  matches the card byte for byte.  The remaining fault is isolated to the CPU
  FIFO path, which ESP-IDF does not use at all.
- Artifact/procedure: BSP package 305,192 bytes, SHA-256
  `0da2757c5692a1a408a6afb1a5df865cd5278fee0ca374aa0ca671268a3f8a06`, written
  only to `arosbsp` at `0x820000` and independently `verify-flash`-verified.
  Core unchanged in `ota_0`.  10,654 bytes captured.
- Change under test, isolated to one character:
  `P4SD_CTRL_USE_INTERNAL_DMA` moved from bit 26 to bit 25 in
  `sdcard/sdcard_esp32p4_intern.h`.
- Result, the acceptance-relevant line of the run:
  `verdict cmd17 DIFFERS from card, cmd18 matches card`.  For LBA 2048 with
  two sectors, CMD18 returned `0x3180955e`, which is exactly the card's own
  FNV-1a-32 over those 1024 bytes.  The CMD17 baseline for the same range
  returned `0x9cdc27c5`, the word-repeat pattern.  The predicted mechanism is
  therefore confirmed: with the wrong bit the descriptor engine ran through
  `BMOD.DE` while the controller never selected the internal DMAC data path,
  so the DMA transfer carried the same repeated word the broken CPU path
  produces.
- Consequence the same run exposes: after the first CMD18, every later CMD17
  fails hard with `raw=00000020` (RXDR only, no DATA_OVER),
  `status=01008901` and `bytes=512/512`, and the request times out.
  `p4sd_dma_prepare_read()` sets `CTRL.dma_enable` and
  `CTRL.use_internal_dma` and nothing ever clears them, so a subsequent
  single-block command runs the CPU FIFO path while the controller is in
  internal-DMA mode.  Before this correction that combination was
  ineffective, because the bit written was reserved; now it takes effect and
  turns silently wrong data into a clean failure.
- Unchanged and still wrong: the first CMD17 of a boot, issued before any
  CMD18, still returns the 128-copy word pattern.  The CPU FIFO read does not
  pop, as measured in the previous entry.
- Acceptance points: none of A1's gates is passed.  The matrix still fails,
  now because CMD17 fails.  Heartbeat and card enumeration stayed live.
- Safety: read-only throughout, denylist and absent transmit path intact,
  only the authorized `arosbsp` range written, no SD-media write.
- Next safe step: route every data read through the IDMAC, including CMD17
  and CMD51, which is what ESP-IDF does and removes the unsupported CPU FIFO
  path entirely.  Check two things while doing it, that a single 8-byte CMD51
  descriptor is accepted, and that the generic layer's small response buffers
  satisfy the descriptor's alignment requirement.  Clearing the CTRL DMA bits
  after a transfer is the fallback if a CPU path has to be kept for anything.

### 2026-08-22 - A1 every card-referenced cell of the matrix passes

- State: A1 stays `hardware partial`, and this is deliberate.  The data path
  is now correct against the card, which is the first acceptance point of the
  A1 gate.  The repetition, fault-injection and rejection points have not run,
  so the phase is not verified.
- Artifacts: diagnostic XIP core 152,064 bytes, SHA-256
  `2594298365c205979a7aced5ec68f1fec17740a8e6a464afea6f0b9f33bb4928` at
  `ota_0` `0x20000`; BSP package 307,012 bytes, SHA-256
  `4262e01b148cadbc8e5a994927fa5ec33e2d643f2ccf2609ad92f13a928d3d1d` at
  `arosbsp` `0x820000`.  Both written only to those ranges and both
  independently `verify-flash`-verified.  17,271 bytes captured.
- Result: `CMD18 compare matrix passed, two-sector repetitions 1`.  All 24
  cells report match and the log contains no failure, mismatch or WRONG line.
  The twenty card-referenced cells cover 1, 2, 32 and 128 sectors at LBA 0,
  2047, 2048, 2049 and 2083, so both straddle directions across the partition
  start are included.  The four READ64 cells at the card end still declare
  themselves self-comparisons, because no reference was captured for that
  range.
- Single-sector evidence: LBA 0 now returns `0xdebe99c1` with `nonzero 13` and
  `signature 55aa`, the real MBR.  LBA 2048 returns `0x730d1cbd` with
  `nonzero 133`.  All six steps of the address probe match the card, which
  also settles the question that probe was built to answer.
- Changes in this step, in order of effect:
  1. Every data read goes through the IDMAC, including CMD17 and CMD51.  The
     CPU FIFO path is no longer used, matching ESP-IDF, which has no such
     path at all.
  2. Destinations that are not cache-line aligned or not a whole number of
     lines go through an aligned 512-byte bounce buffer if they fit, and are
     rejected with a diagnostic if they do not.  This is what makes the
     eight-byte CMD51 of card identification work through the DMAC.
  3. Cache maintenance rounds its range up to whole 64-byte lines, which the
     ROM range operations require.
  4. `CTRL.dma_enable` and `CTRL.use_internal_dma` are cleared after each
     transfer and in the DMA reset path, so no later command inherits
     internal-DMA mode.
  5. A dangling `else` in `krnP4SDCardMultiblockTest()` bound to the inner
     `if` rather than the `total_sectors >= 128` test, so every successful
     READ64 cell executed `passed = 0`.  The matrix could never report success
     regardless of the hardware.  This bug predates the current work and
     masked the result of the previous run, in which all 24 cells already
     matched.
- Safety: read-only throughout.  No SD-media write command exists in this
  build, the independent write denylist and the absence of a FIFO transmit
  path are intact, and only the two authorized flash ranges were written.  The
  card was not written and its content is unchanged.
- Remaining risk: the READ64 cells have no external reference, so a repeated
  block there is still undetectable; the harness says so rather than claiming
  a pass.  The bounce path is exercised only by CMD51 so far.  The settle
  delay added earlier is retained but was never shown to be necessary.
- Next safe step: run the rest of the A1 gate.  `P4_SDCARD_REPEAT=1000` for
  the repetition point, then `P4_SDCARD_EXPECT_FAULT` for the injected
  command-CRC, data-CRC and timeout cases, each of which must terminate,
  recover through CMD12/CMD13 and permit the next read.  Capture a reference
  for the card's last sectors before trusting the READ64 cells.  Do not start
  A2 until those points hold.

### 2026-08-22 - A1 repetition point passes, 1,000 bounded reads without a mismatch

- State: A1 stays `hardware partial`.  Two of the gate's acceptance points now
  hold, the card-referenced byte comparison and the repetition run.  Fault
  injection and invalid-request rejection have not run.
- Artifacts: diagnostic XIP core 152,304 bytes, SHA-256
  `53f8e4c0ebf8371a39cd845148516239f32564302c601e23d8b44fdccda53e30` at
  `ota_0` `0x20000`, built with `P4_A1_DIAGNOSTIC=1 P4_SDCARD_REPEAT=1000
  P4_LDSCRIPT=ldscript-xip.lds`.  The BSP package was unchanged at
  `4262e01b148cadbc8e5a994927fa5ec33e2d643f2ccf2609ad92f13a928d3d1d`.  Both
  ranges independently `verify-flash`-verified.  17,625 bytes captured.
- Result: `CMD18 compare matrix passed, two-sector repetitions 1000`.  All 24
  matrix cells matched again, ten progress lines from 100 to 1,000 were
  emitted, and the log contains no mismatch, failure, error or WRONG line.
- Coverage of the repetition run: the address rotates over LBA 2048, 0, 2047,
  2049 and 2083, two sectors each, so all five addresses including both
  straddle cases were exercised 200 times each rather than one address 1,000
  times.  Each iteration issues two CMD17 baselines and one CMD18, compares
  1,024 bytes against the card and allocates and frees its buffers, so the
  run also exercises repeated allocation.
- Harness changes for this step: the repetition loop rotates the address and
  reports progress every hundredth iteration, and `P4_SDCARD_REPEAT` is now
  plumbed through `kernel/mmakefile.src` so it can be set on the command line
  instead of only in the source.  The in-source default stays 1.
- Heartbeat, captured separately past the test-end marker rather than
  assumed: `clic raised line reached the trap handler, irqs seen 690, last
  line 20` and `timer 100 Hz on clic line 20, ticks 689`.  The timer and its
  interrupt path are alive after the full matrix and all 1,000 repetitions,
  and the same capture reports PSRAM mapped and verified.  That closes the
  gate's heartbeat point for this run.
- Safety: read-only throughout, denylist and absent transmit path intact, only
  the authorized `ota_0` range written, card content unchanged.
- Next safe step: `P4_SDCARD_EXPECT_FAULT` for the injected command-CRC,
  data-CRC and timeout cases, each of which must terminate, recover through
  CMD12/CMD13 and permit the next read.  Then the rejection cases for zero,
  unaligned, over-cap and out-of-range requests.  A card reference for the
  last sectors is still missing, so the four READ64 cells remain
  self-comparisons.

### 2026-08-22 - A1 fault-injection point passes for all three modes

- State: A1 stays `hardware partial`.  Four acceptance points now hold: the
  card-referenced byte comparison, the 1,000 repetitions, the heartbeat, and
  the injected-fault recovery.  The rejection cases have not run.
- Artifacts: diagnostic XIP core 152,304 bytes, SHA-256
  `39541c934cced9bd3a8b522973279e10767b4760e55333804aa73193f5b1c668`, built
  with `P4_SDCARD_EXPECT_FAULT=1`, at `ota_0` `0x20000`.  Three BSP packages
  built with `P4_SDCARD_FAULT_INJECT=1|2|3`, SHA-256 prefixes
  `76c633c854f7e39a`, `16f2f96c58b00928` and `6c9ec7fe39516951`, each written
  only to `arosbsp` `0x820000` and each independently
  `verify-flash`-verified before its run.
- Plumbing added for this step: `P4_SDCARD_FAULT_INJECT` through
  `sdcard/mmakefile.src` and `P4_SDCARD_EXPECT_FAULT` through
  `kernel/mmakefile.src`.  Both were documented switches that no makefile
  passed to the compiler, so neither had ever been exercised.  Note for
  reproduction: a change to a mmakefile does not invalidate an existing
  object, so the affected `.o` has to be removed or the define silently does
  not reach the build.  That happened once here and produced a core with an
  unchanged hash.
- Result, one line per mode, all three with recovery and a passing matrix:
  * mode 1, command CRC: `failed in state 0: decided on raw=00000040`.
    Bit 6 is RCRC and state 0 is SENDING_CMD, so the abort went through the
    command-error branch.
  * mode 2, data CRC: `failed in state 1: decided on raw=00000084`.
    Bit 7 is DCRC, bit 2 is CMD_DONE and state 1 is SENDING_DATA, so
    completion of the command and the injected data error landed in the same
    iteration and the abort went through the data-error branch.
  * mode 3, timeout: `failed in state 0: decided on raw=00000004`, no
    injected bit.  The abort left the wait loop directly, which is the same
    path a real timeout takes.  It simulates the timeout branch rather than
    letting the loop run out, which is worth stating precisely.
  Each run then reported `CMD18 recovery completed with CMD12/CMD13`,
  `expected injected failure; CMD17 recovery passed` and
  `CMD18 compare matrix passed`.  No run ever reached
  `disabling host`.
- Diagnostic correction this step required: the first two attempts produced
  identical failure lines for all three modes, because a software-injected
  bit never appears in RINTSTS and the failure line printed only the
  register.  It now also prints the request state and the raw value the
  decision was actually made on.  Without that the evidence could not say
  which branch fired, and the first two runs are therefore superseded by
  these three rather than counted.
- Safety: read-only throughout.  The injection sets bits in a local variable
  and never writes a controller register, no media-write path exists in any
  of these builds, and only the two authorized flash ranges were written.
- Remaining risk: the recovery path was exercised once per mode, not
  repeatedly, and always on the same first CMD18 of a run.  Recovery under
  load, for example a fault during a 128-sector transfer, is untested.
- Restored device state: no build with fault injection was left on the board.
  Both artifacts were rebuilt without it, written to their authorized ranges
  and independently `verify-flash`-verified: BSP package 307,680 bytes,
  SHA-256
  `70234352ced167427935fe6f4bbc1e78ea640827df9a8b1cb2422e17eef16d46`, and
  diagnostic XIP core 152,304 bytes, SHA-256
  `bbc3c9be54b7d8acbacc1cf2ab1c7b7b5645d352de6ba3c03e5d682e3d469c24`.  The
  confirmation run reports `CMD18 compare matrix passed` with all 24 cells
  matching and no injection line, so this is the resting state.
- Next safe step: the rejection cases, zero, unaligned, over-cap and
  out-of-range requests, each of which must fail without changing controller
  or card state.  Then capture a card reference for the last sectors so the
  four READ64 cells stop being self-comparisons.

### 2026-08-22 - A1 rejection point passes; reference rebased on stable sectors

- State: A1 stays `hardware partial`.  Five of the gate's six acceptance
  points now hold in one run.  The one that does not is the byte comparison
  near the card end, which has no external reference because the card was
  already back in the board when that range was needed.
- Final artifact: diagnostic XIP core 153,888 bytes, SHA-256
  `f87f5e2042c7eb036dd555d3a48a8513e4453e87c9d98efdcea081249df0c33e`, built
  with `P4_A1_DIAGNOSTIC=1 P4_SDCARD_REPEAT=1000
  P4_LDSCRIPT=ldscript-xip.lds`, written only to `ota_0` `0x20000` and
  independently `verify-flash`-verified.  BSP package unchanged at
  `70234352ced167427935fe6f4bbc1e78ea640827df9a8b1cb2422e17eef16d46`.
- Result of that single run: 27 cells match and none fails,
  `repetitions completed 1000`, `CMD18 compare matrix passed`,
  `rejection test passed`, and no trap.
- Rejection cases, six required and six rejected, each followed by a
  verified read of LBA 2048 to show controller and card state intact:
  unaligned offset, unaligned length, both unaligned, and unaligned offset
  through READ64 all return error 253; the first sector past the end
  (`block ee2b000` against capacity `ee2b000`) and an address far past the
  end both return error 251.  Zero length returns success with zero bytes
  and is recorded as a no-op rather than a rejection.
- Two limits of the rejection test, worth recording rather than hiding:
  1. On a 128 GB card an out-of-range request cannot be expressed through
     32-bit `CMD_READ` at all.  The largest byte offset a ULONG holds is
     block 8,388,607 while the card has 249,737,216, so every 32-bit offset
     is inside the media and those cases must use READ64.
  2. The backend's `P4SD_MAX_DATA_LEN` cap sits behind the generic layer's
     chunking at 128 blocks, so no device request reaches it.  An attempt to
     provoke it asked for 256 sectors into the 512-byte SRAM probe buffer and
     trapped with `pc=0`: that was a fault in the test, not in the driver,
     because `io_Length` is the only statement of a buffer's size and no
     layer can catch a caller that lies about it.  The case was removed.
- Reference correction forced by hardware: a run found LBA 2049 holding
  `0x758fc4ce` where the compiled-in table said `0x1647bc76`, and every cell
  whose range contained 2049 failed while every other cell matched.  CMD17
  and CMD18 agreed on the new value at every address, so this was not a
  driver regression but the volatility warning in
  `sdcard/test-card-reference.md` coming true: the card had been out of the
  board and a host had updated FSInfo.  The tables are now rebased on ranges
  that contain no volatile sector.  LBA 0 and 2083 are the only addresses
  whose 1, 2, 32 and 128-sector ranges are all stable on this card; 2047,
  2048, 2053 and 2054 contribute straddle and single-sector cases below the
  FSInfo and FAT sectors.  LBA 2049, 2080 and 2081 are removed from the
  naming table as well, so a block is reported as unknown rather than
  mislabelled.
- Also in this step: the READ64 path is now exercised at the referenced
  addresses, not only near the card end, which is what actually proves the
  64-bit code path against the card.
- Interruption worth recording: between two runs the board reported
  `GPIO45 high: no card present` twice in a row and rebooted repeatedly.  It
  was mechanical, not a software fault, and it resolved without any change
  from this side.  It is also the most likely occasion on which the card
  passed through a host and had its FSInfo rewritten.
- Safety: read-only throughout.  No SD-media write command exists in any of
  these builds, the command denylist and the absence of a FIFO transmit path
  are intact, and only the two authorized flash ranges were ever written.
- Remaining risk and open point: the four READ64 cells near the card end are
  still self-comparisons and their reference blocks are all identical, which
  the harness reports.  Closing that needs the card in a host reader once, to
  capture the last sectors, and the capture must be the last thing that
  touches the card before the run.  Recovery under load, a fault during a
  128-sector transfer rather than on the first CMD18, also remains untested.
- Next safe step: capture the card-end reference, then decide whether A1 is
  complete.  Do not start A2 before that decision.

### 2026-08-22 - A1 acceptance gate complete

- State change: A1 moves from `hardware partial` to `hardware verified`.  Two
  points of the gate are met differently than originally written and both
  deviations are stated below rather than glossed over.
- Resting artifacts, both written only to their authorized ranges and both
  independently `verify-flash`-verified: BSP package 307,680 bytes, SHA-256
  `70234352ced167427935fe6f4bbc1e78ea640827df9a8b1cb2422e17eef16d46` at
  `arosbsp` `0x820000`; diagnostic XIP core 154,768 bytes, SHA-256
  `164c026b3e940c60e527d2e06bf8f479a7632c84034dcc7c0f77cc3a6adcad0e` at
  `ota_0` `0x20000`, built with `P4_A1_DIAGNOSTIC=1 P4_SDCARD_REPEAT=1000
  P4_LDSCRIPT=ldscript-xip.lds`.  Both hashes reproduced bit-identically from
  a clean object rebuild.
- Confirming run: `CMD18 compare matrix passed, two-sector repetitions 1000,
  cells unverified against a stale reference 0` and `rejection test passed`,
  with 59 cells matching, none failing and no injection line.
- The card-end problem, solved without writing to the card.  The last 128
  sectors are entirely zero-filled, so those cells cannot detect a repeated
  block whatever the reference says.  What they were meant to prove is a byte
  offset beyond what a 32-bit ULONG expresses, and LBA 8388608 is exactly
  that boundary.  Both LBA 8388608 and 10000000 hold file data with 128
  distinct sectors each and are now referenced for 1, 2, 32 and 128 sectors
  through READ64; all eight cells match.  The card-end cells are retained,
  now checked against the card, and still print their blindness warning.
  For the record, writing was also assessed: only four sectors at the very
  end lie past the last addressable cluster, too few for a 128-sector cell,
  and the clusters behind the rest (3901159 to 3901161) are free according to
  the FAT, so a raw write would have been lossless.  It was not necessary.
- Fault injection repeated at the current state, and it caught a defect in
  the injection itself.  With the new core the data-CRC mode stopped working:
  `expected fault was not injected`, because a data error is only acted on
  once the request reaches SENDING_DATA and the injection fired in the first
  iteration, where the state is still SENDING_CMD.  It had appeared to work
  only while CMD_DONE happened to be set in that same iteration, which is
  timing, not a test.  The data case now waits for the state; the other two
  are state-independent and their packages rebuilt bit-identically, which is
  itself the evidence that they were unaffected.  All three then pass:
  mode 1 `failed in state 0: decided on raw=00000040`, mode 2
  `injecting fault mode 2 in state 1` and `failed in state 1: decided on
  raw=00000084`, mode 3 `failed in state 0` with no injected bit.  Each
  reports CMD12/CMD13 recovery, a passing CMD17 afterwards and a passing
  matrix.
- Heartbeat, captured past the test-end marker: `irqs seen 888`,
  `timer 100 Hz on clic line 20, ticks 887`, then `alive 1`.
- The two deviations from the gate as written:
  1. "near the card end through READ64" is met by the boundary addresses
     above instead.  The card end itself carries no distinguishable data, and
     a cell that cannot fail is not evidence.
  2. "reject over-cap requests" is not reachable through the device.  The
     backend's `P4SD_MAX_DATA_LEN` sits behind the generic layer's chunking
     at 128 blocks, so it is defence in depth.  A request larger than its
     own destination buffer is a caller bug that no layer can catch, since
     `io_Length` is the only statement of the buffer's size; the attempt to
     provoke it trapped with `pc=0` and was removed.  Zero length is a no-op
     returning success with zero bytes, recorded rather than counted.
- Safety across the whole phase: read-only throughout.  No SD-media write
  command exists in any build, the independent write denylist and the absence
  of a FIFO transmit path are intact, the card was never written, and only
  `ota_0` and `arosbsp` were ever flashed.
- Remaining risk carried into A2: recovery was exercised once per mode and
  always on the first CMD18 of a run, so a fault during a 128-sector transfer
  is untested.  The reference depends on this card's file contents; if they
  change, recapture per `sdcard/test-card-reference.md`.  Four-bit mode and
  any clock above 400 kHz remain out of scope and untested.
- Next safe step: A2, bounded partition discovery.  The MBR at LBA 0 now
  reads correctly, which is what A2 needs and what was silently broken until
  today.

### 2026-08-23 - A2 source audit: what the partition scanner does on hostile input

- State: A2 moves from `not started` to `in development`.  This entry is a
  source audit with no code change and no hardware run.  It exists because
  the first item of the A2 gate, checked arithmetic for every byte, sector
  and entry range, presupposes knowing where the unchecked ones are.
- Scope audited: `rom/partition/partitionmbr.c` (488 lines),
  `partitiongpt.c` (735), `partitionebr.c` (475) and
  `partition_support.c` (198).  `rom/dosboot/bootscan.c` is named by the gate
  but is out of the current path, because AGENTS.md keeps `dosboot.resource`
  out of the package until A1 and A2 pass.
- GPT, the largest attack surface.  Five defects, each reachable from a
  crafted header:
  1. `HeaderSize` is checked only downwards, `hdrSize >= GPT_MIN_HEADER_SIZE`
     (`partitiongpt.c:264`), and then handed straight to
     `Crc32_ComputeBuf(0, hdr, hdrSize)` at `:273`.  The buffer is one block,
     512 bytes.  A header claiming 0xFFFFFFFF reads four gigabytes past it.
  2. `entrysize * cnt` at `:344` is an unchecked 32-bit multiplication of two
     attacker-controlled values, used for the allocation size.  It can wrap,
     and the same product is then used as a length for
     `Crc32_ComputeBuf(0, table, entrysize * cnt)` at `:361`, reading past a
     buffer that was allocated from the wrapped value.
  3. That allocation, `AllocMem(tablesize, MEMF_ANY)` at `:349`, has no cap.
     `entrysize` 128 with `cnt` 0x1000000 asks for two gigabytes.
  4. The entry loop runs `for (i = 0; i < cnt; i++)` at `:373` with `cnt`
     straight from the header, so up to four billion iterations over a buffer
     sized from the wrapped product.
  5. `initPartitionHandle(root, &gph->ph, startblk, endblk - startblk + 1)`
     at `:399` underflows to a huge count when `endblk < startblk`, and
     neither block is checked against the root geometry.
- EBR, the chain walk in `PartitionEBROpenPartitionTable`:
  1. No cycle detection.  The next link is taken from
     `ebr->pcpt[1].first_sector` at `:140` and the loop ends only when that
     value is zero or a read fails.  Two sectors pointing at each other loop
     forever.
  2. No depth limit.  The loop counter `i` at `:115` is a `UBYTE` that appears
     in no condition, so it bounds nothing at all.
  3. `block_no + AROS_LE2LONG(ebr->pcpt[0].first_sector)` at `:127` is an
     unchecked 32-bit addition.
  4. No range check against the root geometry for either value.
- MBR is the mildest, with a fixed four-entry loop at `:111`, but
  `PartitionMBRNewHandle` at `:64` passes `first_sector` and `count_sector`
  into `initPartitionHandle` with no bound beyond `first_sector != 0`.
- `initPartitionHandle` (`partition_support.c:126`) is the common funnel and
  the natural place for the geometry bound.  It derives cylinder counts by
  division and copies the parent DosEnvec, and it currently accepts any
  32-bit start and count without comparing them against the root's own
  extent.
- Why this matters here and not only in theory: the A1 work showed that
  `partition.library` was reading a zero-filled sector where this card holds
  its MBR, and reporting success.  The scanner was never actually exercised
  on real table data on this port, so none of the above has ever run against
  a real, let alone a hostile, table.
- Safety: no code, flash or media change for this entry.
- Open decision before implementation, recorded because it shapes the work:
  the acceptance gate wants truncated, cyclic, overlapping, overflowing and
  bad-CRC corpus images to fail within a documented budget.  Writing such
  images to the test card is excluded by the read-only boundary, and the
  medium hash must stay unchanged.  The candidate is a small RAM-backed
  block device in the diagnostic core that serves crafted tables, so the
  parser can be driven without touching any medium.  That keeps the corpus
  out of shared code and off the card.
- Next safe step: bound `initPartitionHandle` against the root geometry, then
  harden GPT in the order above, then EBR.  Do not enable automatic
  cold-start discovery until the corpus fails within budget.

### 2026-08-23 - A2 hardening in place; the card's own table reads correctly

- State: A2 stays `in development`.  The first acceptance point holds on
  hardware.  The hostile corpus has not run.
- Artifacts: BSP package 309,636 bytes, SHA-256
  `84fa8c90c9142c48c59e86d4f6d8922f218394c9540e957e30ac52e3bb2ac3c5` at
  `arosbsp` `0x820000`; diagnostic XIP core 157,536 bytes, SHA-256
  `e682d5642bce0c412d97b3d0d7baafd3cd0db1290fb5bbcf9ede389d8c01cf73` at
  `ota_0` `0x20000`, built with `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1`.
  Both independently `verify-flash`-verified.  The build produced no error and
  no warning across 30 partition sources.
- Result: the card's table is read correctly for the first time on this port.
  `LBA 0 result 0, hash 0xdebe99c1, nonzero 13, signature 55aa` through the
  public API, where before A1 the same call returned 512 zero bytes.
  `table type 2` is MBR, and discovery reports exactly one partition,
  `start 2048, sectors 249735168, dostype 0x46415402`.  Start and length match
  the MBR entry read independently in a host reader, type `0x0b`, and the DOS
  type is the expected FAT mapping.  A normal read after discovery returns
  `0x730d1cbd`, the card's own VBR hash, which is the gate's requirement that
  a plain read still works afterwards.
- Hardening applied, all of it additive: a valid table behaves exactly as
  before and only additional rejections were introduced.
  1. `partitionRangeIsSane()` in `partition_support.c` is the common guard.
     It computes start plus count in 64 bits so a 32-bit wrap is caught
     instead of folded into a plausible small number, refuses a zero count,
     which used to produce a zero-cylinder handle whose de_HighCyl underflows
     below de_LowCyl, and compares the end against the parent's extent.  The
     extent is the larger of dg_TotalSectors and cylinders times cylinder
     sectors, so neither a root handle nor a derived one is misjudged, and an
     unknown geometry falls back to the overflow check alone.
  2. All three table handlers call it before allocating for an entry: MBR in
     `PartitionMBRNewHandle`, EBR in `PartitionEBRNewHandle`, GPT per entry.
  3. GPT `HeaderSize` is now bounded above as well, by `GPT_MAX_HEADER_SIZE`
     and by the actual block size.  The constant already existed and had
     never been used, while the unbounded value was passed straight to the
     CRC as a length over a one-block buffer.
  4. The GPT entry array is bounded before use: entry size at least the
     structure the code reads and a multiple of eight, entry count non-zero
     and capped, and the product computed in 64 bits and checked against
     `GPT_MAX_TABLE_BYTES` before it is narrowed for the allocation.  The CRC
     length now comes from that checked value.
  5. EBR gets both bounds the gate asks for, a visited-sector set and a depth
     limit of `EBR_MAX_CHAIN`.  Neither replaces the other: the set catches a
     cycle of any length immediately, the limit catches a long non-repeating
     chain.  The chain link is range-checked before the read, and the logical
     start is computed in 64 bits before being narrowed.
- Defect found while hardening, and fixed: the GPT entry loop advanced its
  pointer at the bottom, and the `unused entry` path used `continue`, which
  skipped that advance.  After the first unused entry the pointer stopped
  moving and every later entry was read as a copy of that one, so the gaps
  the code's own comment promises to tolerate were in fact not tolerated.
  The advance now happens in the loop header, which makes every `continue`
  correct by construction.
- Safety: read-only throughout.  The card was not written and only the two
  authorized flash ranges were touched.  The partition code's write paths were
  not modified.
- Remaining risk: this is shared AROS code on all architectures.  The changes
  are additive rejections, and a valid table takes exactly the same path as
  before, but only this port has been built and run.
- Next safe step: the hostile corpus.  Build the RAM-backed block device in
  the diagnostic core, serve truncated, cyclic, overlapping, overflowing and
  bad-CRC tables from it, and require each to fail within a documented read
  and time budget with a working read afterwards.

### 2026-08-23 - A2 acceptance gate complete

- State change: A2 moves from `in development` to `hardware verified`.
- Artifacts: BSP package 344,696 bytes, SHA-256
  `abafa7948027f2bf918642e1dab11ce3e8ff13e57093c2fe8219f5391b92fc49` at
  `arosbsp` `0x820000`; diagnostic XIP core 159,968 bytes, SHA-256
  `43419d5cb331ff0af94edc170843574842aff5e02d06364af36884eb61c73e4a` at
  `ota_0` `0x20000`, built with `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1
  P4_LDSCRIPT=ldscript-xip.lds`.  Both independently `verify-flash`-verified.
- The corpus device: `arch/riscv-esp32p4/ramtest` is a new read-only block
  device carrying eleven deliberately malformed tables, one per unit.  The
  unit number selects the case, so no side channel was needed to tell it what
  to serve.  It holds only const data and has no write path at all, by
  construction rather than by policy, and it makes the corpus repeatable
  without ever touching a medium.  It is a test fixture and is to be dropped
  from any package that is not a diagnostic build.
- Result: `hostile table corpus passed`, all eleven cases within expectation,
  a working read after every one, and no case exceeding a handful of sector
  reads.
  | unit | case | outcome | sectors read |
  | :--- | :--- | :--- | :--- |
  | 0 | mbr entry past end of medium | 0 partitions | 4 |
  | 1 | mbr entry wraps 32 bits | 0 partitions | 4 |
  | 2 | mbr entry of zero length | 0 partitions | 4 |
  | 3 | ebr chain cycles on itself | nested table refused | 7 |
  | 4 | gpt header size 0xffffffff | 0 partitions | 5 |
  | 5 | gpt entry array wraps | refused, 212 | 4 |
  | 6 | gpt header bad crc | refused, 225 | 5 |
  | 7 | gpt truncated before entries | refused, 225 | 36 |
  | 8 | gpt entry size below the structure | refused, 212 | 4 |
  | 9 | gpt entry count zero | refused, 212 | 4 |
  | 10 | bad gpt behind a valid protective mbr | refused, 225 | 5 |
  The read budget is therefore between four and thirty-six sectors per case;
  the outlier is the truncated GPT, where the parser reads toward a 128-entry
  array before the medium stops answering, which is the behaviour under test.
- Two corrections the corpus forced on the test itself, both recorded because
  they change what a pass means:
  1. The first version only opened the top-level table.  A cyclic EBR chain
     was therefore never walked and the cycle guard never ran, while the test
     reported a failure for a reason that had nothing to do with the cycle:
     the extended entry is itself a legitimate MBR partition.  The test now
     descends into nested tables under a depth limit, and unit 3 reports
     `nested table at depth 1 refused`, which is the actual evidence that the
     guard works.
  2. The first version expected zero partitions everywhere.  That is wrong
     for a broken GPT behind a valid protective MBR: falling back to the MBR
     is correct behaviour.  A case now states how many partitions it may
     yield and which table type it must never accept, so unit 10 documents
     the fallback instead of failing it.
- A third defect, in the fixture: the unit array was fixed at eight while the
  corpus had eleven cases, and the run reported `passed` having silently never
  executed the last three.  The array is now sized generously and the device
  complains if the corpus outgrows it.
- Safety: read-only throughout.  Nothing was written to the SD card, whose
  content is unchanged, and only the two authorized flash ranges were used.
  The corpus device cannot write anything.
- Remaining risk: this is shared AROS code on every architecture.  The
  changes are additive rejections and a valid table takes the same path as
  before, but only this port has been built and run.  The corpus covers MBR,
  EBR and GPT reading; the write paths were not modified and are not tested.
  The recursive dosboot traversal named by the gate is still out of the
  picture, because AGENTS.md keeps `dosboot.resource` out of the package
  until A4.
- Next safe step: A3, a reproducible host-built read-only FAT32 image.  A2's
  discovery is now trustworthy enough to build on.

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
