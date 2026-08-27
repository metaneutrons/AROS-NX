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
| A3 | Reproducible, host-built read-only FAT32 `SYS:` image | `hardware verified` | Evidence entry 2026-08-23: byte-reproducible image, checked by the host parser, `fsck_msdos` and its manifest, and read correctly on the board at the values predicted from the image.  Every changed sector after the run is attributed to the host's mount |
| A4 | Minimal resident DOS/FAT bootstrap from flash PKG | `hardware verified` | Evidence entries 2026-08-23: all eight gate points.  `SYS:` assigned from the A3 image after `AROS.boot` was accepted, `Info()` reports `ID_WRITE_PROTECTED`, eight DOS mutations refused with error 214 and the file bit-identical afterwards, and a medium without `AROS.boot` falls back to a Shell prompt.  Five defects in shared code fixed on the way |
| A5 | Command and library loaded from MicroSD | `hardware verified` | Evidence entries 2026-08-23: a command and a library loaded from FAT and run from both a kickstart resident and the Shell, every address outside the resident ranges, a marker neither side can fake, and four refusal cases each failing with its reason named and nothing leaked.  Closes M6.  One point met differently and documented |
| B0 | Canonical D1001 display contract and provenance | `documented` | [display/DISPLAY-CONTRACT.md](display/DISPLAY-CONTRACT.md) is the one place display facts live, each value carrying an evidence class and a source file and line.  Three findings changed the plan: the running reference ignores the timing fields it is given and uses a different set, the rotation direction is no longer a hypothesis, and the DSI bridge registers differ by chip revision.  Nothing is `verified` yet, which is the honest state; the measured frame rate is B4's and is recorded as deferred |
| B1 | Calibrated 200 MHz PSRAM with measured headroom | `hardware partial` | The calibration works and 200 MHz holds over all 32 MB with four bus patterns, identically across six warm resets, and the whole storage stack passes on both media at 360 MHz.  The phase premise was wrong in an instructive way: the PSRAM bus was never the limit.  The CPU ran at 90 MHz because nothing configured it, and is now 360; sequential reads went 20 to 60 MB/s and internal SRAM 25 to 101.  The 100 MB/s gate is not assessable by a CPU loop and is reassigned to B5, with the reason recorded |
| B2 | Safe I2C1/PCA9535 panel-power sequence | `hardware verified` | An I2C master for both P4 controllers, OOP-free and shaped for the `WriteRead` method of AROS's existing `hidd.i2c` class so a later HIDD wraps rather than reimplements it.  The panel supply and reset pulse run twice and return to safe, with every unrelated expander bit provably unmoved.  Preservation is proved against a deliberately seeded one, not against a zero, because the board's battery means the expander has no reachable cold state.  Two defects of mine were found by hardware, not by reading |
| B3 | LDO3, DSI PHY/host and JD9365 command path | `hardware verified` | Stage one verified: the PLL locks and all three lanes reach stop state, which is also the evidence that the hardware-fixed PHY reference is the 40 MHz crystal.  Stage two transmits: command mode is entered and the whole JD9365 sequence goes out with no host error.  But there is no panel-side confirmation of anything, because DSI writes are unacknowledged and all five DCS reads are silent while the reference reads the same register successfully.  The read path is an open defect, recorded with what has been eliminated; it does not block B4 |
| B4 | Stable internal DSI test pattern | `superseded` | The host side is built and clean: bridge enabled without its pixel feed, pattern generator on, timing programmed and matching the contract's 33.82 Hz, no protocol error and no underrun.  The panel stays dark and unlit.  The backlight path is verifiably asserted end to end, including a measured 18 per cent PWM on GPIO14, and the panel still does not light, which the isolation test cannot explain and which points at something before all of it |
| B5 | Native `800 x 1280` PSRAM scanout | `hardware partial` | **A dimensionally correct, cleanly transmitted frame from PSRAM reaches the panel** - `P4_PANEL_VMUL=1` shows the complete regular grid, every line one continuous colour, with no payload error.  A hardware-observed colour card verifies native little-endian RGB565 and all primary/pair colours; the corrected exactly tiled checker is perfect and moving dirty rectangles leave no visible artefacts.  Runs at 40 MHz over 1500 Mbit/s lanes.  The sustained gate passes 1,800 seconds of concurrent 71 MB/s scanout, read-only SD reads and cache-forced PSRAM passes: all 1,800 samples clean, 112 MB SD verified, 1,800 MB PSRAM verified and zero failures.  Bounded commands and ordered teardown recover an active reset in the verified case, but a separately retained PSRAM failure still needed one vendor-firmware boot and remains part of the open boot-cycle risk.  The optional post-video DCS read can separately pin `GEN_RD_CMD_BUSY`, so it is off by default and excluded from acceptance.  Still open: explicit solid/corner/one-pixel observation, the 100 MB/s gate and the full ten-warm/ten-cold boot gate |
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

Done, as far as documentation can settle it.  The contract lives in
[display/DISPLAY-CONTRACT.md](display/DISPLAY-CONTRACT.md) and that file wins
over anything about the display written here or in the README.  Every value in
it names an evidence class and a source file and line, and nothing in it is
`verified`, because nothing in it has been measured on this port yet.

What the work changed, rather than confirmed:

- The timing contradiction is located, not merely noted.  The reference's
  panel layer declares timing fields, is handed one set and writes a different
  set literally into the DPI configuration, so the values in the board header
  and in the Seeed BSP have never driven this panel.  AROS starts from the set
  that runs.
- The rotation direction is no longer a hypothesis.  Two independent paths in
  the reference agree on 90 degrees clockwise from the logical landscape
  surface to the physical portrait buffer.  What stays open is only which
  physical corner the logical origin lands in, which a 180-degree mounting
  difference would hide from both paths.
- The reference disables PPA hardware rotation deliberately and rotates on the
  CPU.  Its reason does not transfer to AROS, but neither does its evidence,
  so bring-up uses the CPU path and the PPA becomes a measured experiment.
- The DSI host registers are identical across ESP32-P4 hardware versions and
  the bridge registers are not.  This board is revision 1.3, so `hw_ver1`, and
  five bridge registers that exist only in `hw_ver3` must not be touched.

Deferred with a reason rather than dropped: the measured frame rate and the
reference register/command trace.  Neither can be obtained without driving the
panel, so they belong to B4, and the contract says so in the entries they
would promote.

Acceptance gate: met for the documentation half.  One canonical table exists,
it distinguishes evidence classes rather than asserting facts, it records the
licence and provenance of every source, and the two entries needing hardware
are marked as such.

### B1 - calibrated 200 MHz PSRAM

The calibration half is done and verified.  The headroom half turned out to be
a different question than the phase assumed, and the reason is worth keeping.

**What was built.**  `kernel/psram_tuning.c` implements the two-stage read
sampling calibration the ESP32-P4 hardware's two knobs allow.  A 128-byte
reference block is written at 20 MHz, the clock is raised, and the block is
read back with each candidate setting: four DQS phases first, then thirty-one
steps of relative delay between strobe and data, each read a hundred times so
that a candidate which passes once and fails on the hundredth is rejected.
The middle of the widest passing run is chosen, and a run narrower than two
steps is refused as a coincidence rather than a margin.  On any failure the
bus returns to 20 MHz with neutral sampling.

`P4_PSRAM_MHZ=200` asks for it and `P4_CPU_MHZ=360` raises the CPU; both
default to off, because a rate becomes a default after it has been shown to
hold, not when it works once.

**What the premise got wrong.**  The phase was written expecting the PSRAM bus
to be the constraint on scanout.  It is not, and the measurement that settled
it took two builds: at 20 MHz the whole window read at 15 MB/s and at 200 MHz
at 20 MB/s.  A tenfold clock increase bought a third more bandwidth, so
whatever the limit was, it was not the bus.

It was the CPU.  This port configured no CPU clock at all and inherited what
the second-stage bootloader left, which `mcycle` against the 16 MHz system
timer measured at 90 MHz.  The CPLL was already at 360 with the CPU divider
sitting at four.  `kernel/cpuclock.c` moves the four root dividers to the only
configuration that gives 360 MHz within the MEM<=200 and APB<=100 constraints,
in the order that keeps every intermediate state slower than both endpoints.
After that the same reads give 60 MB/s from PSRAM and 101 MB/s from internal
SRAM.

**Why the 100 MB/s gate moves to B5.**  At 360 MHz the loop costs 13.6 cycles
per word even from internal SRAM, and it cost 13.6 at 90 MHz too.  A number
that does not change with the clock is a latency, not a bandwidth: 64-byte
lines mean one fill per sixteen words, and a scalar loop with no prefetch has
exactly one fill outstanding at a time.  So this measures how fast one CPU
thread can pull a cache line, which is not what a display needs.  Scanout is a
DMA read that pipelines many transfers, and its bandwidth cannot be measured
without a DMA engine driving the bus.  That engine arrives in B5, so the
threshold belongs there and the measurement here is recorded for what it is.

Acceptance gate, as met:

- complete address-uniqueness and four-pattern tests over all 32 MiB pass at
  200 MHz, at both 90 and 360 MHz CPU;
- six consecutive warm boots choose the identical window, 25 of 31 steps wide,
  index 17;
- the 20 MHz path is unchanged and still functional, and the fallback returns
  to it with neutral sampling;
- the CPU-visible read bandwidth is measured and reported rather than asserted,
  along with the internal-SRAM ceiling that bounds it.

Not met, and reassigned rather than dropped: the 100 MB/s figure, which needs
a DMA path to mean anything.

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

The concurrent-stress point passed on the D1001 on 2026-08-26.  The remaining
B5 points are the explicit solid/corner/one-pixel panel observation, the
DMA-based 100 MB/s bandwidth decision and the complete boot-cycle matrix.

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
| Package capacity | `arosbsp` is 0x7e0000 bytes and is now shared: the package has everything below `P4_FLASHDISK_PART_OFFSET` and the flash development volume the four megabytes above.  `kernel-package-esp32p4-riscv-checksize` fails the build if the package crosses the split, because past it the loader would read filesystem bytes as members.  Size and every member hash are checked on each expansion. |
| `krnP4FlashMap()` is not re-entrant | It owns a single scratch window, so two callers interleaving would each see the other's mapping.  `flashdisk.device` serves every request inside `Forbid()` and in 64 KB pieces, which is sufficient only because the map and the `CopyMem()` out of it are a few hundred cycles and no request waits on anything.  A writing path would have to erase and program, so it cannot reuse this pattern, and a second consumer of the map added anywhere has to be checked against this. |
| Undocumented DSI PHY constants | IDF's DSI bring-up writes `mipi_dsi_phy_ll_set_switch_time(50, 104, 46, 128)` and `set_max_read_time(6000)` with no derivation in any locally available source, and no ESP32-P4 technical reference manual is present on this machine.  They are carried over as opaque constants.  The failure mode is a PHY that locks but produces marginal signalling, which would look like a panel or timing problem rather than a PHY one, so a B4 pattern fault has to consider them before the timing set is blamed. |
| CPU clock inherited, not configured | This port set no CPU clock and ran at 90 MHz until 2026-08-23 because the second-stage bootloader's divider was never touched.  Nothing failed, everything was four times slower than the silicon allows, and no diagnostic said so; it was found only by measuring `mcycle` against the system timer while chasing a bandwidth figure.  Anything else this port inherits from that bootloader is unexamined in the same way, the flash clock and the cache configuration in particular. |
| CPU-loop bandwidth is a latency measurement | A scalar read loop costs 13.6 cycles per word from internal SRAM at both 90 and 360 MHz, which is one cache-line fill per sixteen words with a single fill outstanding.  It therefore measures fill latency and not the memory system's throughput, and no threshold about scanout can be argued from it.  A DMA engine is the only way to measure what the display will actually get, and until B5 exists any bandwidth claim about scanout is unfounded. |
| ~~The MSPI PLL does not calibrate from a cold boot~~ | **Closed.**  `PMU_RF_PWC` bit 26, `PERIF_I2C_RSTB`, holds the analogue peripheral I2C block in reset by default, and the PLL's calibration state machine runs over it.  The register interface works regardless, which is why every measurement said the bus was fine while the calibration never began, and the register survives a CPU reset, which is why a boot after other firmware worked and a cold one did not.  Released - and pulsed, so every boot starts from the state that used to fail.  Verified: `calib entry 0x24c done here`, 32 MB at 200 MHz. |
| The port expander survives a CPU reset | The PCA9535 has no reset pin and keeps its direction and output registers across every reboot, so its state at boot is whatever the last firmware left, not the datasheet default.  Any code that writes a whole register drives pins it never considered; B2's first version pulled the battery-charge enable low that way.  Read-modify-write is the only safe form here, and a check that assumes cold defaults passes vacuously on a warm board. |  And it cannot be cold-started from software at all: the board has a battery, so removing USB changes nothing, and releasing PWR_HOLD with the board on battery was tried cleanly and did not switch it off - the next boot still read the direction register as all-outputs where a cold device reads all-inputs.  Any test that wants the datasheet defaults has to say so out loud.
| The display has produced no panel-side evidence | Not one DCS reply and not one pixel.  The host reports a locked PHY, lanes in stop state, a clean command path, a running pattern generator with no underrun, and a measured PWM on the backlight pin, and the panel is dark and unlit.  Every register compared matches the vendor BSP and the working reference.  Until something comes back from the panel, every statement about the display path is a statement about the SoC. |
| DSI reads get no reply | Five DCS reads, the vendor identity register and four standard ones, all return nothing with no protocol error flagged and the host left waiting.  Espressif's driver reads the same register with an unbounded wait and works on this board, so the panel answers there and this port's read path is wrong.  A software reset and the divider encoding have been eliminated.  Nothing in the port depends on reads yet, but a panel that cannot be interrogated cannot be diagnosed either, and B6's orientation work would rather have the scanline register than a photograph. |
| Flash reads past 16 MB | `krnP4FlashMap()` refuses anything at or past the cache-mapping limit, so the `storage` partition at 0x1020000 is unreachable by that route.  Anything that needs it would have to use raw SPI commands with the cache suspended and a destination in internal SRAM, which is why the development volume was put inside `arosbsp` instead. |
| Panel timing | Start from measured Vellum behavior, not the contradictory 60 Hz comment. |
| PSRAM | 20 MHz remains the safe fallback; display scanout requires a calibrated, measured faster path. |
| Rotation | Native portrait first; correct landscape only after stable scanout and VSYNC ownership. |
| Buffering | One buffer first; extra buffers require measured need and explicit memory cost. |
| Cache/DMA | Every presented CPU-written region needs an explicit clean operation. |
| Source provenance | External Vellum/Waveshare/IDF sources are references; licence compatibility is checked before code reuse. |
| Touch | Not a GB0 dependency and not claimed as GSL3670 until probed. |
| Console input fairness | `econsole` polls `RawMayGetChar()` and yields, so its handler process is permanently ready at DOS's `dn_Priority` 10 while a prompt waits for a line; measured at an idle prompt, two tasks are ready, ECON at 10 and the Shell at 0.  Nothing below 10 runs, and any latency or throughput figure taken with a prompt open is distorted.  A short timed wait on `timer.device` was tried and wedges the machine (evidence entry 2026-08-23), so the standing rule is that anything which must run alongside a prompt sits above priority 10.  The real fix is interrupt-driven console input, which belongs with Track C's `con` handler and `keyboard.device` rather than in `econsole`. |
| `DoIO` from a handler's packet dispatch | Unexplained: `econsole`'s first `DoIO` on `timer.device` from inside `Raw_Read()` never returns, on either unit, and stops an unrelated pending timer request as well.  Anything in Track B or C that waits on a device from inside a handler's dispatch has to be treated as suspect until this is understood. |
| Build flags and stale objects | mmake does not invalidate objects when a `-D` flag or a mmakefile changes.  Observed twice on 2026-08-23: a kernel built once without the `P4_*` diagnostics was not rebuilt when the flags returned, giving a 144,432-byte core instead of 165,296 which was then flashed; and a package built with `DOS_DEBUG=1` still carried a silent `dos.library`.  Since this roadmap records configurations as evidence, an artifact can silently disagree with the line written beside it, and a result can be falsely positive or falsely negative.  Delete the affected objects before any build whose outcome is documented; the rule is also in AGENTS.md. | A related trap in the same file: a variable set on the make command line beats a plain assignment inside a makefile, so `P4_HEADLESS_BOOT=1 P4_CMDLINE="..."` silently dropped the headless words until that assignment was made `override`.  Any option that composes with a caller-supplied one has to be written that way.
| DMA destination alignment | `AllocMem` does not return cache-line-aligned memory on this target, and the IDMAC needs 64 bytes because cache maintenance works on whole lines.  FAT's 32-sector cache reads arrive 32-byte aligned and go through the SD backend's bounce buffer for exactly that reason.  Only the SD backend knows this today; every further DMA driver has to bounce or align, and Track B's framebuffer is the next one. |
| Storage diagnostics after dosboot | The A1, A2 and A3 diagnostics run after `krnStartExec()`, and `dosboot.resource` does not return from it.  A boot-capable build therefore cannot exercise them at all.  Regression-testing the storage stack needs a second build with `dosboot` out of the package, which is the same diagnostic-ordering boundary Track A was structured around, now permanent. |
| Trampoline cache maintenance | Anything built with `__AROS_SET_FULLJMP` is real instructions written through the data path and needs a `CacheClearE()` that is not conditional on `__AROS_USE_FULLJMP`; that macro means the library jump table holds instructions, which is a different question and false on 32-bit RISC-V.  Four sites exist and all four are correct as of 2026-08-23: `rom/dos/internalloadseg_elf.c`, `arch/ppc-chrp/dos/internalloadseg_elf.c`, `compiler/arossupport/createseglist.c` and `workbench/c/shellcommands/shellcommands_init.c`, the last only after being fixed.  The failure mode is an illegal-instruction trap on a valid instruction, which reads as a compiler or linker fault and is neither. |
| COLDSTART residents below -50 | `dosboot.resource` initialises at -50 and does not return, so any COLDSTART resident with a lower priority never runs while it is in the package.  `ram-handler` at -125 is the case that surfaced this: it cannot provide `RAM:` here, which is why A5's refusal fixtures are served from memory through `InternalLoadSeg()`'s own function array instead.  Anything added to the package has to be checked against -50, and `RTF_AFTERDOS` is the pass to use for work that needs a running DOS. |
| Shell ready while blocked | Unexplained: at an idle prompt the heartbeat's ready-list dump shows the Shell permanently ready at priority 0 while it is blocked waiting for a packet reply from the console handler.  Being ready rather than waiting is not what `WaitPkt()` should produce.  It may be harmless bookkeeping and it may be a scheduler or `WaitPkt` defect; either way it matters more for Track C's real console than for `econsole`. |

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

### 2026-08-23 - A3 image target builds and verifies on the host

- State: A3 moves from `not started` to `host verified`.  Every acceptance
  point that does not require a medium holds.  The image has not been written
  to a card, so there is no board run and no before/after pair from one yet.
- What was added, all host-side and none of it running on the board:
  `arch/riscv-esp32p4/image/mkfat32.py` writes the whole image, MBR through
  directory entries, with no root privileges and no external tools;
  `verify-image.sh` and `check-manifest.py` check the result with tools that
  did not build it; `content/` holds the tracked part of the tree; and
  `mmakefile.src` adds `kernel-image-esp32p4-riscv`, which is not part of any
  default build.
- Why the generator writes everything itself rather than calling a formatter:
  mtools is not present on every host, and `newfs_msdos` stamps a volume
  serial from the clock, which would make the output unreproducible.  Writing
  it here means every field is controlled, and the only two that a formatter
  would randomise, the volume serial and the per-entry timestamps, are build
  parameters with fixed defaults.
- Layout: 64 MiB, one MBR partition of type `0x0b` at LBA 2048, the same shape
  as the D1001 card so discovery sees something it already reads correctly.
  One sector per cluster, 126,976 clusters, two FATs of 1008 sectors.
- Acceptance evidence, from `verify-image.sh` on the built artifact:
  the host's own parser reports `FDisk_partition_scheme` and `DOS_FAT_32`;
  `fsck_msdos -n` completes all three phases with no error; the mounted
  contents match the manifest exactly, ten entries against ten; and the image
  hash is identical before and after verification.  Rebuilding through the
  make target from unchanged inputs produces a byte-identical image,
  `c68961921b3ddd986caa1b7dc9dfae4e20ac03a4af6d3cc2d0773359d9afd9f6`.
- Two defects the independent tools caught, which is the reason for using
  them:
  1. The `..` entry of every subdirectory pointed at the root's own cluster
     number.  The specification requires zero when the parent is the root, and
     `fsck` said so.
  2. The first image had 16,092 clusters with eight-sector clusters, and the
     host refused to mount it.  Correctly: FAT32 is only FAT32 above 65,524
     clusters, and below that the specification calls the volume FAT16.  The
     generator now refuses to emit such an image rather than producing one a
     host rejects.
- Verification detail worth recording: the mount is done on a copy, never on
  the image.  macOS writes to a FAT volume as soon as it mounts it, creating
  `.fseventsd`, which would change the image and invalidate exactly the
  before/after hash that board runs depend on.  The manifest check ignores
  that and the other known host artefacts by name, and reports anything else
  unexpected rather than filtering broadly.
- Content, deliberately minimal and to grow one audited line at a time:
  `AROS.boot` carrying the CPU marker `__dos_IsBootable()` searches for with
  `strstr`, taken from `$(AROS_TARGET_CPU)` so it cannot drift from the
  target; a `S:Startup-Sequence` that only echoes, because the standard
  sequence performs file management on `SYS:`; and the directory skeleton the
  assigns expect.  The A5 proof command and library are not in it, because
  they do not exist yet.
- Safety: nothing was written to any medium.  The SD card was not touched and
  the board was not involved.
- Remaining risk and the open decision: the gate's before/after requirement
  needs the image on a medium, and writing it to the test card would destroy
  the reference the A1 and A2 evidence rests on.  That needs either a second
  card or a deliberate decision to give up the current one.  Until then A3 is
  `host verified` and not more.
- Next safe step: decide the medium question, then A4.  Note that AGENTS.md
  keeps `dosboot.resource` out of the package until A1 and A2 pass, which they
  now do, so A4 is unblocked on that count.

### 2026-08-23 - A3 the generated image reads correctly on the board

- State change: A3 moves from `host verified` to `hardware verified`.
- Medium: a second card, 31.9 GB, 62,333,952 sectors, distinct from the
  reference card at 249,737,216 sectors, so no A1 or A2 evidence was put at
  risk.  It previously held a Zoom recorder's 140-byte `ZOOM.SYS` and an empty
  `TRASH`, nothing else; the configuration file was copied off before writing,
  SHA-256 `ef3ae44350e65c5f1c1e1b443f3e278b540db237725a4317866cab659b1aed1b`.
  The user confirmed the card and authorised overwriting it.
- Write and readback: the 64 MiB image, SHA-256
  `c68961921b3ddd986caa1b7dc9dfae4e20ac03a4af6d3cc2d0773359d9afd9f6`, was
  written with `dd` to `/dev/rdisk21` and read back bit-identically.
- Board result, with the values predicted from the image beforehand rather
  than read off afterwards:
  | quantity | predicted on the host | reported by the board |
  | :--- | :--- | :--- |
  | LBA 0, the MBR | `0x100fcd1c` | `0x100fcd1c` |
  | LBA 2048, the VBR | `0xaa126dff` | `0xaa126dff` |
  | partition start | 2048 | 2048 |
  | partition length | 129,024 sectors | 129,024 sectors |
  | table type | MBR | `table type 2` |
  Also `signature 55aa`, `nonzero 6`, which is exactly how many non-zero bytes
  this generated MBR has against the reference card's thirteen, `dostype
  0x46415402` and `partitions found 1, as expected`.  The trailing
  `card 0x730d1cbd` on the read-after-discovery line is the compiled-in
  reference for the *other* card and is meaningless here.
- The before/after requirement, answered precisely rather than with a single
  hash.  The sector hash does differ, `7b4b8d2a...` against `1ff47794...`, and
  the reason is the host, not the board.  Eight sectors changed: FSInfo, both
  FATs, the root directory and four clusters.  The only directory entry that
  appeared is `FSEVE~12`, which is `.fseventsd`, created by macOS the moment it
  mounted the card.  The MBR is unchanged, the VBR is unchanged, and every
  cluster holding generated content is unchanged.  So nothing the board did
  altered the medium, and the difference is fully attributed.
- Why a plain hash comparison cannot be had here: macOS mounts a FAT volume on
  insertion and writes to it before any command can intervene.  Getting an
  untouched after-image would need automount suppressed for the device, which
  is a system-level change and was not made.  Attributing every changed sector
  is the stronger evidence anyway, and it is what was done.
- Also recorded: an earlier attempt at this measurement produced the SHA-256 of
  an empty file, because the card had been removed while the read ran and
  `diskutil info` answered from cache.  The reading loop now waits for the
  device to actually return data.  An earlier claim in this session that all
  131,072 sectors had changed came from the same cause and was wrong.
- Safety: the reference card was never in the reader or the board during any
  of this.  Only the second card was written, once, deliberately and with
  confirmation.  No AROS build has a media write path.
- Remaining risk: the A1 matrix fails on this card, as it must, because its
  reference tables describe the other one.  A run that wants both would need
  either a second set of tables or a switch; today the two cards serve two
  different purposes and that is fine.
- Next safe step: A4, the resident DOS/FAT bootstrap.  AGENTS.md kept
  `dosboot.resource` out of the package until A1 and A2 passed; both now do,
  and A3 supplies the medium it needs.

### 2026-08-23 - A4 first measured step: the DOS side loads, without dosboot

- State change: A4 `not started` to `build verified, hardware partial`.  Nine
  new package members load, relocate and initialise on the D1001, and the
  pre-dosboot half of the A4 gate passes.  `dosboot.resource` is deliberately
  still absent, so nothing yet boots.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, over
  `/dev/cu.usbmodem101`.  **No SD card in the slot for this run**; the board
  reported `GPIO45 high: no card present`, so the card-dependent A1 and A3
  checks did not run and are still owed on the enlarged package.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `9d93afae8e`.
- Artifacts:
  - package `aros-bsp.pkg` 976,008 bytes,
    `c2f4f7e9fa5d20c9382885bb7f9e599d8a8e95b120e8b15774f60979e05e2bf3`;
  - final core `aros-esp32p4.bin` 162,096 bytes,
    `59103f1740cd4b84e86230953f1ef758a3324a786691c0cad93f9bd10176768f`.
  - Two earlier cores in the same session, kept because each answered one
    question: `a2f0882c...f2435` (162,032 bytes at the time of writing, no
    command line) showed the signed-priority defect and the dead
    `KrnGetBootInfo()`; `ee7fa807...2a9b7c` showed that adding `KRN_CmdLine`
    alone changed nothing, which is what pointed at `BootMsg`.
- Configuration: `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1 P4_DOS_PROBE=1
  P4_HEADLESS_BOOT=1 P4_LDSCRIPT=ldscript-xip.lds`.  Flashed only to `ota_0`
  at `0x20000` and `arosbsp` at `0x820000`, under the 2026-08-22 standing
  authorisation; both writes verified by esptool hash.

Package growth.  The declared set went from four members to twelve: added
`dos.library`, `bootloader.resource`, `FileSystem.resource`,
`lddemon.resource`, `shell.resource`, `shellcommands.resource` and the `fat`
handler, on top of `utility`, `partition`, `expansion`, `sdcard` and the A2
`ramtest` fixture.  976,008 bytes, 11.8 % of the `0x7e0000` partition, so
size is not a constraint at this stage.  `dos64` was not added: nothing asked
for it, and the phase requires evidence of a dependency rather than a guess.

Three things had to be fixed before any of it could be built or seen, and
each was a real defect rather than a configuration mistake.

**The RISC-V C runtime did not assemble for this ABI.**  `compiler-stdc-riscv`
failed with twelve `extension 'd' required` errors.  `arch/riscv-all/crt`
saves the callee-saved floating point registers with `fsd`/`fld`, which is
correct for `ilp32d` but not for this target: `configure.in:2251` selects
`rv32imafc_zicsr_zifencei_zaamo_zalrsc` with `-mabi=ilp32f`, so the f
registers are 32 bits wide and the D opcodes do not exist.  Five files
carried the same twelve lines: `stdc/setjmp.s`, `stdc/longjmp.s`,
`posixc/sigsetjmp.s`, `posixc/siglongjmp.s`, `posixc/vfork_longjmp.s`.  They
now select opcode and stride from `__riscv_float_abi_double` /
`__riscv_float_abi_single`, and skip the block entirely under a soft float
ABI, which is the same three-way split `arch/riscv-all/include/aros/
genmodule.h` already makes for `fa0`-`fa7`.  Verified by disassembly: all
five now emit `fsw`/`flw` at 4-byte stride from offset 56 to 100, and all
five agree on the slot addresses, which is the property that matters, since
a `setjmp` and a `longjmp` that disagree would corrupt silently.  `_JMPLEN 37`
is unchanged and still sized for the widest ABI.

**The DOS catalogs were missing.**  `rom/dos/displayerror.c` includes
`"strings.h"` for `MSG_STRING_RETRY` and its siblings, which
`rom/dos/genstrings.py` produces from `catalogs/dos.cd`.  `rom/dos/catalogs`
is a git submodule and was not initialised in this worktree, so the include
fell through to the C library's `strings.h` and the symbols were undeclared.
Initialising the submodule at the pinned `d06c8fc263` fixed it.  Worth
recording because the failure does not name a submodule anywhere.

**`KrnGetBootInfo()` returned nothing, so the command line was unreachable.**
`bootloader.resource` reads its whole world from that one call, and the
kernel global it returns, `BootMsg`, was never assigned by this platform.
The boot tags did reach exec, because they are also passed to
`krnPrepareExecBase()`, which is why nothing had noticed.  The first run
showed it plainly: `loader 0x00000000 ''`.  Adding a `KRN_CmdLine` tag alone
changed nothing, which is the observation that located the cause rather than
the symptom.  `krnPrepareBootTags()` now assigns `BootMsg`, as the sibling
`arch/riscv-native/sifive_u` port does in the same place.

New build options, both in `kernel/mmakefile.src`:

- `P4_CMDLINE="..."` adds the `KRN_CmdLine` boot tag.  There is no firmware
  source for a command line on this board, so the build is the only honest
  place for one.  `P4_HEADLESS_BOOT=1` sets the three words A4 requires,
  `econsole nomonitors nocomposition`.
- `P4_DOS_PROBE=1` asks the half of the A4 gate that can still be answered
  before `dosboot.resource` exists.  This split is the point of the step:
  dosboot's COLDSTART init never returns, so everything printed after
  `krnStartExec()` becomes unreachable the moment it joins the package.

Host-side audit, now reproducible as `boot/audit-package.py`.  It reads the
accepted relocation set out of `kernel_elf.c` rather than restating it, so it
cannot drift from the loader.  All twelve members are little endian ELF32
`REL` RISC-V objects with flags `0x3, RVC, single-float ABI`, and the nine
new ones introduce no relocation type the loader did not already implement:
the union across the package is 20 types, all handled.  `dos.library` at
274,368 bytes is the largest member and adds only `JAL` beyond what
`sdcard.device` already used.

Observed on the board:

- twelve modules loaded and relocated, 1,237,892 bytes of PSRAM reserved, no
  relocation rejected;
- the resident list matches what the `.conf` files declare, in order:
  `expansion` 110, `utility` 103, `bootloader` 100, `FileSystem` 80,
  `partition` 40, `sdcard.device` 4, `ramtest.device` 3, `fat-handler` -1,
  `SDCard boot wait` -49, `dos.library` -120, then `lddemon`, `shell` and
  `shellcommands` at -123.  This is the gate's "expected resident and its
  version in the intended order", checked against the source rather than
  against itself;
- `bootloader.resource` reports `loader 0x4001e3b0 'ESP32-P4 ROM'` and the
  three arguments `econsole`, `nomonitors`, `nocomposition`, in order;
- `FileSystem.resource` carries four entries from the FAT handler: the three
  DosTypes `0x46415400`, `0x46415401`, `0x46415402` plus the named
  `fat-handler` entry at DosType 0.  `0x46415402` is exactly what A3's board
  run reported for the partition, so the medium and the handler agree;
- `dos.library`'s romtag is present at -120 with `rt_Flags` 0, that is
  present but not self-starting.  It has to be found by `FindResident()` and
  started by hand from `dosboot_BootStrapDos()`; a COLDSTART bit here would
  mean it started on its own, before dosboot had chosen a boot node;
- the A2 corpus is unaffected by the larger package: all eleven malformed
  tables refused, each within 4 to 36 sector reads, a working read after
  every one of them;
- the heartbeat ran for the full 59-beat capture with the tick serving.

A defect in the port's own diagnostics, found by reading the output: signed
priorities were printed through `krnP4PutDec()`, so -120 appeared as
4294967176 and could not be compared with a `.conf` file at all.  Added
`krnP4PutDecS()` and used it for the three places that print a priority.
Every priority quoted above is from the corrected build.

- Acceptance points passed: package below the partition; every new ELF member
  and resident priority audited; UART lists every expected resident and its
  version in the intended order; `FileSystem.resource` contains the FAT
  entry; normal heartbeats continue.
- Acceptance points not yet reached, all of them requiring `dosboot`: the SD
  boot-wait resident running before dosboot; dosboot replacing the whole-disk
  node; FAT starting, locking the volume and assigning `SYS:`; `Info()`
  reporting write protection and mutations failing without a requester;
  missing media falling back without a hang.
- Safety: no media write path exists in any build.  The card was not in the
  board for this run, which is also why nothing could have touched it.
- Remaining risk: the card-dependent A1 and A3 checks have not been repeated
  against the enlarged package.  They must be, before dosboot is added, since
  the point of this step is to keep them observable while it is still
  possible.  Also unverified: that `econsole` will be reachable, because the
  handler is not yet in the package.
- Next safe step: re-run this core with the A3 card in the slot to close the
  card-dependent half; then FAT write protection, which is a source change
  needing no board; then `econsole` and `dosboot` together as the last step,
  after which this console goes quiet.

### 2026-08-23 - A4 second measured step: read-only propagated above the block device

- State change: none.  `build verified` only.  The board cannot judge any of
  this yet, because FAT is only started by `dosboot.resource` and that is
  still deliberately out of the package.  The one part that is testable
  without dosboot, the block device's own refusal, is built into the
  diagnostic and is waiting for a card.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `0410f8ddc2`.
- Artifacts, built and flashed but not yet exercised on a medium:
  - package 1,005,264 bytes,
    `4094f3c027f13adb3753598f6f30edbd801ffa39cb0dee66da22b5bd50faef24`,
    12.2 % of the partition.  Larger than the previous one because the
    `fat` handler is now built with `FAT_DEBUG=1`, 165,272 bytes against
    136,024;
  - core 163,728 bytes,
    `4c481e81ba62f022b54d628d331c49fb80f3b6bc50c21998c8bb80f78b58cd0b`.
  - Audited with `boot/audit-package.py`: twelve members, no relocation type
    outside what the loader implements.

The phase requires that read-only safety propagate above the block-device
denylist, and names the reason: `sdcard.device` already reports
`TD_PROTSTATUS` as protected and refuses writes, but FAT reported
`ID_VALIDATED` and a failed write could enter an interactive `Retry|Cancel`
loop.  Reading the code confirmed both, and found that the requester loop
is presently harmless only by accident: `ErrorMessageArgs()` opens
intuition.library, which does not exist on this platform, so
`EasyRequestArgs()` is never called and the zero it leaves behind reads as
Cancel.  That is a dependency on a library's absence, and it stops being
true at C2.

Four changes, arranged so that no single one of them is load-bearing.

1. `rom/filesys/fat/disk.c` gains `ProbeWriteProtection()`, which asks
   `TD_PROTSTATUS` and records the answer in `glob->disk_writeprotected`.
   It runs once when the device is opened and again on every disk change,
   before the super block is read; a medium already in the drive at mount
   time never produces a change event, which is why both call sites are
   needed.  A device that does not implement the command answers
   `IOERR_NOCMD`, and that is read as "not protected", because a device that
   cannot be asked has to be treated as writable or nothing would mount.
2. `rom/filesys/fat/packet.c` refuses every mutating packet before the
   dispatch switch can reach it, returning `ERROR_DISK_WRITE_PROTECTED`.
   Deciding this from `dp_Type` alone is what makes the refusal complete:
   nothing has been allocated, no directory handle taken and no cache block
   touched, so there is nothing dirty and nothing to undo.  The list is
   `WRITE`, `SET_FILE_SIZE`, `DELETE_OBJECT`, `RENAME_OBJECT`, `CREATE_DIR`,
   `SET_PROTECT`, `SET_DATE`, `SET_COMMENT`, `RENAME_DISK`, `FORMAT`,
   `MAKE_LINK`, `FINDOUTPUT` and `FINDUPDATE`.  `FINDINPUT` is deliberately
   not on it, since reading files is the entire point of mounting the volume.
3. `AccessDisk()` refuses a write before issuing any I/O, returning
   `TDERR_WriteProt` with `io_Actual` zeroed.  This is defence in depth and
   not redundant: the cache flush that runs off the timer does not arrive as
   a packet, so the packet guard alone would not cover it.  Because the
   refusal happens before the `while (retry)` loop, no requester can be
   opened for it either.
4. `FillDiskInfo()` reports `ID_WRITE_PROTECTED` instead of `ID_VALIDATED`.
   A second, redundant `id_DiskState = ID_VALIDATED` in the no-super-block
   branch was removed; it overwrote the value set a few lines above and would
   have quietly undone this for an unmounted volume.

`sdcard.device` was changed too, in both write handlers: a denied write now
reports `TDERR_WriteProt` rather than `IOERR_ABORTED`, and sets `io_Actual`
to zero explicitly.  The error code matters because a filesystem can act on
`TDERR_WriteProt` and can only guess at `IOERR_ABORTED`, and because
`TD_PROTSTATUS` on this unit already says protected, so the two now agree.
`io_Actual` is set rather than assumed, because "nothing was written" has to
be readable from the reply without knowing what the caller left in it.

New diagnostic, `krnP4SDCardWriteDenialTest()`, run from the existing
`P4_SDCARD_DEVICE_TEST` after the rejection test.  It asks `TD_PROTSTATUS`,
then issues a real `CMD_WRITE` at LBA 2048 with a `0x5a5a5a5a` pattern the
sector demonstrably does not contain, and requires all four of: an error,
that error being `TDERR_WriteProt`, `io_Actual` back to zero from a
deliberately poisoned `0xdeadbeef`, and the sector unchanged and still
matching the card reference afterwards.  The last is the one that matters:
an error code says the request was refused, and only the unchanged content
says nothing reached the card.

Observability, which is the reason for the whole measured-step structure.
The `D()` macros in `dos.library`, `dosboot.resource` and the FAT handler are
compiled out by default, and the tree's way of turning them on is to
uncomment a line in a mmakefile.  That cannot be reproduced from a build
command.  Three mmakefiles now also gate them on a make variable,
`DOS_DEBUG=1`, `DOSBOOT_DEBUG=1` and `FAT_DEBUG=1`, and the ten per-area
`DEBUG_*` defaults in `fat_fs.h` are wrapped in `#ifndef` so any of them can
be raised from a command line without a redefinition warning.  Without this,
the dosboot step would have been silent: that module's initialisation never
returns, so its own narration is the only account of a boot that failed.
The shipped `fat` handler is built with `FAT_DEBUG=1` for the bring-up.

One statement is made unconditionally rather than under `D()`: when a volume
is mounted from a protected medium, FAT says so once.  A volume that will
refuse every write is worth one line, and without it the first evidence is a
failure somewhere later with no statement anywhere of why it was inevitable.
The per-packet and per-write refusals stay under `D()`.

- Acceptance points addressed but not yet observed: FAT recognising the device
  protection state; mutations rejected before dirty cache state; a bounded
  error with no requester; `io_Actual == 0` on a denied block write.
- Safety impact: strictly increased.  Every path that could have reached a
  write now refuses earlier, and no path was opened.
- Remaining risk: the whole of point 1 to 4 above is source reasoning.  Until
  dosboot starts FAT on a real medium, the only part with hardware standing
  will be the device-level denial, and that needs a card in the slot.
- Next safe step: run the flashed core with the reference card in, which
  closes the device-level denial and re-checks the A1 matrix against the
  enlarged package; then `econsole` and `dosboot`.

### 2026-08-23 - A4 third measured step: the device-level write refusal, on hardware

- State change: the device half of A4's read-only requirement moves from
  `build verified` to `hardware verified`, and A1 and A2 are re-confirmed
  against the enlarged package.  A4 as a whole stays `hardware partial`;
  `dosboot.resource` is still out of the package, so nothing boots.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.  The
  **reference card** (`sdcard/test-card-reference.md`), 249,737,216 sectors,
  in the board slot.  Chosen over the A3 card because its content is what
  `p4sd_ref_sectors` describes, so both the A1 matrix and the new write test
  can be judged against known values rather than against ourselves.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `9372964c9b`.
- Artifacts:
  - core 163,728 bytes,
    `4c481e81ba62f022b54d628d331c49fb80f3b6bc50c21998c8bb80f78b58cd0b`;
  - package 1,026,228 bytes,
    `b99120eef91fdf71e97969456b23c5a903acc0c119998ef909194ca68da39169`,
    thirteen members, 12.4 % of the partition.  All thirteen pass
    `boot/audit-package.py`.
- Configuration: `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1 P4_DOS_PROBE=1
  P4_HEADLESS_BOOT=1 P4_LDSCRIPT=ldscript-xip.lds`, package built with
  `FAT_DEBUG=1`.  Flashed to `ota_0` at `0x20000` and `arosbsp` at
  `0x820000`, both verified by esptool hash.

`econsole` joined the package in this step, as the last member before
dosboot, so that dosboot's arrival is the only variable in the step after
this one.  It loaded and its romtag came up at priority -49, sharing that
priority with `sdcard.device`'s `SDCard boot wait` exactly as predicted.  The
tie is harmless and now recorded as such: one registers a boot node at
bootpri -127 and the other waits for pending bus tasks, neither reads the
other's state, and dosboot at -50 follows both whichever way the link order
falls.

The write denial test, which is the point of this run:

```text
[sddev]  write denial test starting
[sddev]    TD_PROTSTATUS: error 0, actual 0xffffffff, medium reported protected
[SDCard00] cmd_Write32: Error: Card is Locked/Write Protected
[sddev]    CMD_WRITE at LBA 2048: error 28 (TDERR_WriteProt), actual 0x00000000
[sddev]    sector hash before 0x730d1cbd, after 0x730d1cbd, unchanged and matching the card
[sddev]  write denial test passed
```

All four required properties hold.  `TD_PROTSTATUS` answers protected, which
is the value FAT's new `ProbeWriteProtection()` will read at mount.  A real
`CMD_WRITE` carrying a `0x5a5a5a5a` pattern is refused with error 28, that is
`TDERR_WriteProt` and not the former `IOERR_ABORTED`.  `io_Actual` comes back
zero from a deliberately poisoned `0xdeadbeef`, so a caller can read "nothing
was written" from the reply without knowing its prior contents.  And the
sector is byte-identical before and after and still matches the card
reference, which is the only one of the four that shows nothing reached the
card rather than merely that the request was refused.

Everything else in the run, against the larger package:

- thirteen modules loaded and relocated, 1,302,332 bytes of PSRAM reserved;
- the A1 matrix passes unchanged: 59 card-referenced cells, all with
  `cmd17 matches card, cmd18 matches card`, no `DIFFERS` anywhere, and zero
  cells unverified against a stale reference;
- the rejection test passes all seven cases with a correct follow-up read
  after each;
- A2 discovery on the card reports its one partition at start 2048,
  249,735,168 sectors, DosType `0x46415402`, and the read after discovery
  returns `0x730d1cbd`, matching the card;
- the eleven-case hostile corpus passes;
- the pre-dosboot probe passes, now including `econsole` in the resident list;
- the heartbeat ran 85 beats with the tick serving throughout.

A finding that changes the A5 plan rather than this one.  `econsole` reads its
input through `RawMayGetChar()`, which reaches `KrnMayGetChar()`, whose
generic implementation in `rom/kernel/maygetchar.c` returns -1 and which this
platform does not override.  `ECON:` is therefore output-only here: it can
give `dos.library` a console to write to and the synthetic `ECON:AROS.boot`
to boot from, but nothing typed will ever arrive.  A4 does not need input,
and the fallback requirement is about not hanging rather than about
interaction.  A5 does: of the two routes the phase names, "an AFTERDOS probe
or an interactive command", only the first is available until `krnMayGetC()`
is written for the USB Serial/JTAG and UART0 receive paths.  Both channels
are already understood by `kernel_console.c` on the transmit side, so this is
a small piece of work, but it is A5's and not this phase's.

- Acceptance points passed: read-only safety propagated to the block-device
  reply in a form a filesystem can act on; a denied block write reports
  `io_Actual == 0`; A1 and A2 unaffected by a package two and a half times
  the size; normal heartbeats continue.
- Safety impact: the only write ever issued to a card by any AROS build so
  far was issued by this test, deliberately, and was refused before it
  reached the controller.  The sector hash proves it: `0x730d1cbd` before and
  after, equal to the reference.
- Remaining risk: everything above the block device is still source
  reasoning.  FAT's `ID_WRITE_PROTECTED`, its packet refusal and its
  `AccessDisk()` guard cannot be observed until dosboot starts the handler.
- Next safe step: add `dosboot.resource`, with `DOS_DEBUG=1` and
  `DOSBOOT_DEBUG=1` so the boot narrates itself, and accept that this
  console's post-`krnStartExec()` diagnostics go silent from that point.

### 2026-08-23 - A4 fourth step: dosboot boots, and four defects it found

- State change: A4 stays `hardware partial`, but the boot itself now happens.
  `dosboot.resource` takes over COLDSTART, replaces the whole-disk node with
  the partition node, `dos.library` starts, FAT mounts the card's volume,
  and when the medium turns out not to be bootable the system falls back to
  the emergency console and reaches a Shell prompt.  What is still owed is
  the bootable case, which needs the A3 card.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, with
  the **reference card** in the slot.  That card carries a FAT32 volume
  called `Amigatausch` and deliberately no `AROS.boot`, which is what made it
  a good first test: it exercises mount, lock, reject and unmount rather than
  only the happy path.  Note the serial device renamed itself from
  `/dev/cu.usbmodem101` to `/dev/cu.usbmodem1101` partway through the
  session; both are the same board.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `5839e1900f`.
- Artifacts:
  - core 165,296 bytes,
    `30736daeeaf140f29102250be600b1a9afa3c461210c7b5a4efd16c815d0a969`;
  - package 1,163,992 bytes,
    `b06915b49ac54952fb076e1dca3f4bb8f00c4d6e8c45709e4b664a03f9a9e60c`,
    fourteen members, 14.1 % of the partition, all passing
    `boot/audit-package.py`.
- Configuration: core with `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1
  P4_DOS_PROBE=1 P4_HEADLESS_BOOT=1 P4_HEARTBEAT_TASK=1
  P4_LDSCRIPT=ldscript-xip.lds`; package with `FAT_DEBUG=1 DOS_DEBUG=1
  DOSBOOT_DEBUG=1`, so the boot narrates itself.  Note that mmake does not
  invalidate objects when a mmakefile changes: the first `DOS_DEBUG=1`
  package still contained a silent dos.library and the objects had to be
  deleted by hand.

What the boot did, in order, all of it read off the UART:

- `dosboot_Init` found the three command-line arguments and selected the
  emergency console;
- `dosboot_BootScan` opened `partition.library`, found the MBR partition and
  replaced the whole-disk node `MMC0` with `SDCARD0P0` at DosType
  `0x46415402`, leaving `ECON` as the second node.  This is the gate's
  "dosboot replaces the whole-disk node with the expected partition node";
- `dosboot_BootDos` found `dos.library`'s romtag and initialised it by hand;
- `dos.library` matched `SDCARD0P0` against `FileSystem.resource` and started
  the FAT handler as a process at priority 10;
- FAT opened the device, detected NSD 64-bit support, read the boot sector at
  sector 2048, identified FAT32 with 3,901,159 clusters, read the FSInfo
  block, named the volume `Amigatausch` and put it in the DOS list;
- `dos.library` locked the volume, looked for `:AROS.boot`, got error 205 and
  reported "Does not have a bootable filesystem, unmounting...";
- FAT shut down cleanly - volume destroyed, super block freed, disk-change
  interrupt removed, device closed;
- dosboot moved on to `ECON`, dos.library was initialised a second time, and
  the Shell reached its prompt with the AROS banner.

Write protection, which is what A4 asked for above the block device:

```text
[fat] TD_PROTSTATUS: error 0, actual -1
[fat] the medium is write protected; every mutating packet will be refused
```

Four defects had to be fixed to get this far, and each was found by a
failure that named itself.

**1. `CacheClearE()` cleared nothing, so freshly written code was not
executable.**  The first dosboot run took an illegal-instruction trap at
`mepc 0x4ff73d18` with a backtrace through `CallEntry` in dos.library.  The
bytes at that address disassemble to `auipc t0,0; lw t0,12(t0); jr t0` - a
perfectly valid trampoline, which is what `CreateSegList()` builds through
the data path before jumping to it.  It calls `CacheClearE()` in between
exactly as it should; the problem was on the other end of that call.
`arch/riscv-all/exec/cachecleare.c` is a bare `fence rw, rw` and says in its
own comment that each platform is expected to replace it.  This one had not.
`arch/riscv-esp32p4/exec/cpu_init.c` now installs `CacheClearE_P4` and
`CacheClearU_P4` with `SetFunction` from `ADD2INITLIB`, both calling the
port's existing `krnP4SyncCode()`, which does the three steps this SoC needs:
write back the data caches, invalidate both L1 instruction caches and the L2,
and `fence.i`.  A caller asking only for `CACRF_InvalidateD` is asking about
a DMA buffer, so only the clear cases are routed there.  The ELF loader had
been doing this by hand since M6, which is why nothing had noticed.

**2. `sdcard.device` never initialised its disc-change interrupt list.**  The
next run died in `cmd_AddChangeInt` with a Store/AMO access fault at
`mtval 0x00000004` - a write through a zeroed `lh_Head`.  `sdcu_SoftList` is
never `NEWLIST`ed at unit creation, and a zeroed list header is not an empty
list.  `scsi.device` and `ata.device` both do this in their unit init;
`sdcard.device`, the newest of the three, does not.  Nothing on this side had
ever called `TD_ADDCHANGEINT`, and a filesystem does it on mounting, which is
why it survived this long.  One `NEWLIST` in `sdcard_bus.c`.

**3. The backend refused FAT's reads.**  FAT's cache reads 32 sectors at a
time into a buffer it got from `AllocMem`, which is 32-byte aligned on this
target and therefore unusable as an IDMAC destination: the cache maintenance
around the descriptor chain works on whole 64-byte lines, and invalidating a
partially covered line would discard a neighbour's dirty data.  The bounce
buffer that exists for exactly this case was 512 bytes, so a 16 KiB request
was refused - and FAT retried it 134,101 times in one boot, which is how the
log reached 402,538 lines.  The bounce is now `P4SD_MAX_DATA_LEN`, 64 KiB.
Splitting the request would also have worked, but it would mean issuing
several commands where the card expects one, and the memory is not scarce:
the buffer lives in the module's `.bss`, which the loader places in the 32 MB
external window, so it costs PSRAM and none of the 230 KB internal heap.  It
is also the already-proven DMA destination, since every unaligned read since
A1 has landed there.

**4. The heartbeat task was starved, and the priority took three attempts.**
Recorded in full in the comment at `krnP4HeartbeatTask()`.  At -20 it never
ran, because `krnTimerWait()` is a busy spin at priority 0 and
`Reschedule()` only picks a ready task of equal or higher priority.  At 5 it
beat correctly before dosboot and went silent after it: DOS starts the ECON
handler as a process at `dn_Priority` 10, and `Raw_Read()` has no way to
block, so it spins on `RawMayGetChar()` and `Reschedule()` at that priority
for as long as a shell waits for a keystroke.  Since `KrnMayGetChar()` has no
implementation on this platform and always returns -1, that is for ever, and
nothing below priority 10 runs again once a shell reaches its prompt.  At 20
it beats through the whole run.  Diagnosing it needed two extra print
statements in the task, because a task that prints nothing gives no way to
tell how far it got; those prints are kept.

The heartbeat is what makes "normal heartbeats continue" answerable at all
now, and it answers it: fifteen beats over the capture, 500 ticks apart at
100 Hz, reporting 2 ready and 6 waiting tasks and a steady 30,658,576 bytes
of free memory.  No trap anywhere in the run.

- Acceptance points passed: UART lists every expected resident and version in
  the intended order; the SD boot-wait resident at -49 runs before dosboot and
  is bounded; `FileSystem.resource` contains the FAT entry; dosboot replaces
  the whole-disk node with the expected partition node; FAT starts and locks
  the volume; a medium without `AROS.boot` falls back to the emergency console
  without a hang; normal heartbeats continue.
- Acceptance points still owed, both needing the A3 card: FAT accepting
  `AROS.boot` and `SYS:` being assigned from it; `Info()` reporting the volume
  write-protected and representative mutations failing immediately with no
  dirty cache and no requester.
- Safety impact: the reference card was mounted, read and unmounted, and its
  content is unchanged - the same run's write-denial test reports sector 2048
  hashing `0x730d1cbd` before and after a deliberate `CMD_WRITE`.  FAT
  announced the medium as write protected before touching it.
- Remaining risk: `ECON:` is output-only, because `KrnMayGetChar()` is
  unimplemented here, so the Shell prompt cannot be typed into.  That closes
  the interactive route for A5 and leaves the AFTERDOS probe as the only one
  until `krnMayGetC()` is written for the USB Serial/JTAG and UART0 receive
  paths.  It also means the ECON handler spins at priority 10 for the rest of
  the machine's life, which any later task below that priority has to account
  for.
- Next safe step: the A3 card in the slot, to close the two remaining gate
  points.  After that, `krnMayGetC()` so the console can be typed into, which
  A5 wants anyway.

### 2026-08-23 - A4 complete: SYS: from the SD card, and every mutation refused

- State change: A4 `hardware partial` to `hardware verified`.  All eight
  acceptance points of the phase are met on the D1001.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, with
  the **A3 test card** in the slot - the one carrying the byte-reproducible
  FAT32 image from the 2026-08-23 A3 entry.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `0df1a96b08`.
- Artifacts:
  - core 170,368 bytes,
    `0f0a421e893f3fe632ae28d4bd7bbf203ebcf918edaf3b01d0824d2598c4c9bd`;
  - package 1,164,016 bytes,
    `41fea833f4fd3ad01853a8febbf3352a3d7e12b8c3153e82d6ed6747c2241b7c`,
    fourteen members, 14.1 % of the partition.
- Configuration: `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1 P4_DOS_PROBE=1
  P4_AFTERDOS_PROBE=1 P4_HEADLESS_BOOT=1 P4_HEARTBEAT_TASK=1
  P4_LDSCRIPT=ldscript-xip.lds`; package with `FAT_DEBUG=1 DOS_DEBUG=1
  DOSBOOT_DEBUG=1`.  Note that the A1, A2 and A3 diagnostics in
  `kernel_startup.c` no longer run in a build like this: they sit after
  `krnStartExec()`, and dosboot does not return from it.  That is the
  diagnostic-ordering boundary this phase was structured around, and it has
  now been crossed deliberately.

The boot, from the A3 medium:

- `dosboot_BootScan` replaced the whole-disk node with `SDCARD0P0`, DosType
  `0x46415402`;
- `dos.library` matched it against `FileSystem.resource`, started the FAT
  handler, and FAT mounted the volume;
- `__dos_IsBootable` opened `:AROS.boot`, read it and reported
  `Signature 'riscv' found`, which is the `cpu riscv` line
  `image/mmakefile.src` writes;
- `Dos/CliInit: Proposed SYS: lock is: 482928a0`, and `SYS:` was assigned
  from that volume;
- the Shell reached its prompt on `ECON:` with the AROS banner.  No trap
  anywhere in the run, and ten heartbeats over the capture.

Two new instruments, both needed because there is no `C:` to load a command
from and no resident `Info` in the shellcommands set.

**Console input.**  `KrnMayGetChar()` had no implementation on this platform,
so `RawMayGetChar()` always returned -1 and `ECON:` was output-only: a prompt
that could be printed and never answered.  `arch/riscv-esp32p4/kernel/
maygetchar.c` now returns `krnP4GetC()`, which reads the USB Serial/JTAG OUT
endpoint when `serial_out_ep_data_avail` is set, or UART0's FIFO when
`rxfifo_cnt` is non-zero; both field names and positions were taken from
`components/soc/esp32p4/register/hw_ver1/soc/*_struct.h` in ESP-IDF v6.0.1
rather than from memory.  A pending transmit is flushed first, since a prompt
written without a trailing newline otherwise sits in the endpoint buffer while
the reader waits for a reply to it.  Verified by typing at the prompt: the
Shell echoed the line and answered.

**An RTF_AFTERDOS probe.**  `rom/dos/cliinit.c` calls
`InitCode(RTF_AFTERDOS)` once `SYS:` and the boot assigns exist and before the
Shell starts, which is exactly the window this needs.  The resident lives in
the kickstart, opens dos.library, and asks:

```text
[sysfs]  Lock("SYS:") = 0x482932d0
[sysfs]  Info() id_DiskState 80 (ID_WRITE_PROTECTED)
[sysfs]    blocks 129024, used 2064, block size 512, disk type 0x444f5300
[sysfs]  read SYS:AROS.boot = 43 bytes, hash 0xf949eb96
[sysfs]    Open(MODE_NEWFILE): result 0x00000000, IoErr 214  refused as write protection
[sysfs]    Open(MODE_READWRITE): result 0x00000000, IoErr 214  refused as write protection
[sysfs]    CreateDir: result 0x00000000, IoErr 214  refused as write protection
[sysfs]    DeleteFile: result 0x00000000, IoErr 214  refused as write protection
[sysfs]    Rename: result 0x00000000, IoErr 214  refused as write protection
[sysfs]    SetProtection: result 0x00000000, IoErr 214  refused as write protection
[sysfs]    SetComment: result 0x00000000, IoErr 214  refused as write protection
[sysfs]    Relabel(device): result 0x00000000, IoErr 214  refused as write protection
[sysfs]  re-read SYS:AROS.boot = 43 bytes, hash 0xf949eb96  unchanged
[sysfs]  Info() id_DiskState after 80 (ID_WRITE_PROTECTED)
[sysfs]  AFTERDOS probe passed, 8 mutation cases
```

`id_DiskState` 80 is `ID_WRITE_PROTECTED`; before the FAT change it would have
been 82, `ID_VALIDATED`.  129,024 blocks is exactly the partition size the A3
image declares.  214 is `ERROR_DISK_WRITE_PROTECTED`, and each of the eight
came back immediately with no requester, because the refusal happens in
`ProcessPackets()` before the dispatch switch.  The two reads either side of
the mutations are what makes the claim about dirty cache state checkable:
43 bytes hashing `0xf949eb96` both times, so nothing was written and nothing
was left half-written in the cache either.

One correction to this probe, made after the first run.  `Relabel("SYS:", ...)`
returned `ERROR_DEVICE_NOT_MOUNTED` (218), which looked like a failure of the
filesystem and was a defect in the test: `Relabel` wants a device, `SYS:` is
an assign, and the packet never reached the handler.  Aimed at the device node
`SDCARD0P0:` it is refused as write protection like the rest.  The first run's
verdict of FAILED on 8 cases is therefore void; the corrected run passes all
eight.

One more defect in shared code had to be fixed on the way, found by typing
`echo` at the prompt and getting an illegal-instruction trap at
`mepc 0x4ff9d4ac` - again on a valid `auipc` at the head of an
`__AROS_SET_FULLJMP` trampoline.  `workbench/c/shellcommands/
shellcommands_init.c` builds such a trampoline for every resident command and
flushes it with `CacheClearE()`, but the flush sat inside
`#ifdef __AROS_USE_FULLJMP`.  That macro means something else: it says the
*library jump table* holds instructions, which is why `MakeFunctions()` and
`SetFunction()` consult it.  On 32-bit RISC-V `struct JumpVec` is a bare
pointer, so the macro is correctly undefined, while the trampoline is three
real instructions.  The flush is now unconditional, matching
`CreateSegList()`, which has never guarded it.  Every resident shell command
was unreachable until this.

- Acceptance points passed, all eight: UART lists every expected resident and
  its version in the intended order; the SD boot-wait resident at -49 runs
  before dosboot and is bounded; `FileSystem.resource` contains the FAT entry;
  dosboot replaces the whole-disk node with the expected partition node; FAT
  starts, locks the volume, accepts `AROS.boot` and `SYS:` is assigned;
  `Info()` reports the volume write-protected and eight representative
  mutations fail immediately with no dirty cache and no requester, while a
  denied block write reports `io_Actual == 0` (2026-08-23 device test);
  missing media falls back to the emergency console without a hang
  (2026-08-23, reference card); normal heartbeats continue.
- Safety impact: the A3 card was mounted, read, subjected to eight deliberate
  mutation attempts and read again, and `AROS.boot` is bit-identical across
  all of it.  Every mutation was refused above the block device, and the block
  device would have refused it again.
- Remaining risk and one defect found in A3's own deliverable: FAT named the
  volume `00D1-505A` rather than a label.  `mkfat32.py` writes
  `AROSP4TEST` into the VBR's `BS_VolLab` field, but AROS's FAT handler reads
  the name from a volume-label entry in the root directory, which the
  generated image does not have.  The reference card, formatted by a host, has
  one and was named `Amigatausch` correctly.  This is cosmetic for A4 but it
  is a real gap in the generator, and the card currently in the board carries
  the image without it.
- Also outstanding: `ECON:` input now works, but the ECON handler still spins
  at DOS's handler priority 10 whenever a shell waits for a keystroke, because
  `Raw_Read()` has no way to block.  Anything below priority 10 is starved for
  as long as a prompt is open, which is why the heartbeat sits at 20.
- Next safe step: fix the volume-label entry in `mkfat32.py`, then A5 - load
  and run a command and a library from the card.  A5's interactive route is
  open now that the console can be typed into, and its AFTERDOS route is the
  resident added here.

### 2026-08-23 - econsole's idle poll: the starvation measured, the fix rejected

- State change: none.  A4 stays `hardware verified`.  This records an attempt
  that failed and the measurement that replaced a wrong explanation, and adds
  two rows to the risk table.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, A3 test
  card in the slot.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `96d38d619d`.

The starting point was the limitation noted in the A4 entry: `econsole`'s
`Raw_Read()` has no way to block, so it polls `RawMayGetChar()` and yields
with `Reschedule()`.  That call hands the CPU to the highest priority *ready*
task, and the ECON handler is one of them for as long as a shell waits for a
line, so nothing below DOS's `dn_Priority` 10 runs.  The heartbeat had to be
moved to 20 because of it.

**The measurement first, because it corrected the explanation.**  The
heartbeat now names the ready tasks instead of counting them, and at an idle
prompt it reports:

```text
[beat]   1  ticks 631  irqs 632  avail 30656528  tasks ready 2 waiting 6  ready: 'ECON'@10 'Shell'@0
```

Two tasks permanently ready, not one: ECON at 10 and the **Shell at 0**.  The
Shell being ready while blocked on a packet reply was not expected and is not
yet explained.  Counting had hidden it; the count was 2 in every earlier run
and I had read that as "the heartbeat plus ECON", which was wrong.

Reasoning from priorities alone had already produced one wrong conclusion
earlier in the day, recorded in the comment at `krnP4HeartbeatTask()`.  This
is why the dump names them now.

**The fix that was tried and rejected.**  Replace `Reschedule()` with a short
timed wait on `timer.device`, so the handler waits rather than stays ready.
That removes the starvation by construction, costs no CPU at an idle prompt,
and adds a keystroke latency below what a person notices.  It does not work.

- 10 ms on `UNIT_VBLANK`: the first `DoIO()` from `Raw_Read()` never returns.
- 50 ms on `UNIT_VBLANK`: same.
- 50 ms on `UNIT_MICROHZ`, a different request list and a different processing
  path in `rom/timer/lowlevel.c`: same.

In every case the console's last line is the instrumented entry to the wait,
and after it there is no output from any task at all - including the heartbeat,
whose own `timer.device` request was already pending and had been completing
on schedule at exactly 500-tick intervals until that moment.  So this is a
wedge of the whole machine, not a scheduling problem.

Ruled out by reading the source rather than by assumption: the generic
`addToWaitList()` keeps each list sorted ascending and `TimerProcessVBlank()`
walks it and breaks at the first request not yet due, which is consistent;
`common_BeginIO()` makes `tr_time` absolute against `tb_Elapsed` for both
units; and the `addedhead` return value that might have re-based elapsed time
is ignored by the generic `BeginIO()`.  Concurrency is not the trigger either,
since `fat-handler` and the heartbeat both hold `UNIT_VBLANK` requests
throughout a normal boot and both are served.

What is left, and unexplained, is the context: a `DoIO()` issued from inside a
DOS handler's packet dispatch, before the current packet is replied.  That is
the one thing econsole does differently from `fat-handler` and from the
heartbeat task.

- Decision: the change is reverted.  A working spin is better than a fix that
  stops the machine, and shipping a guess here would have traded a bounded
  fairness defect for a total one.  What stays is the measurement, a comment
  in `econsole.c` recording exactly what was tried and what happened, and two
  rows in the risk table: one for the fairness rule that anything running
  alongside a prompt must sit above priority 10, and one for the unexplained
  wedge, because anything in Track B or C that waits on a device from inside a
  handler's dispatch is suspect until it is understood.
- Kept from the attempt: `P4_HEARTBEAT_PRI`, so the fairness of everything
  below the heartbeat can be tested rather than assumed, and the ready-task
  name dump.
- Safety impact: none.  No media access changed.
- Remaining risk: as the two new risk rows state.  The proper fix is
  interrupt-driven console input.  Both halves exist on this SoC and were
  checked while investigating: `serial_out_recv_pkt_int_ena` in bit 2 of the
  USB Serial/JTAG interrupt registers, and `ETS_USB_SERIAL_JTAG_INTR_SOURCE`
  and `ETS_UART0_INTR_SOURCE` as CLIC sources.  What is missing is a handler
  table in the port's interrupt dispatch, which today is a single
  `if (line == P4_TIMER_LINE)` in `kernel_traps.c`, and a blocking read path
  for econsole to use; `RawMayGetChar()` is non-blocking by contract and
  `KrnObtainInput()` is a setup call despite its name.  That work belongs with
  Track C's `con` handler and `keyboard.device`, not duplicated in econsole.
- Next safe step: A5, unaffected by any of this.

### 2026-08-23 - A5: code loaded off the card, on both routes

- State change: A5 `not started` to `hardware partial`.  DOS/LoadSeg fetches
  code from FAT, relocates it and runs it, and lddemon opens a library the
  same way; both routes the phase names work.  What is still owed is the
  negative fixtures and the two gate points discussed at the end.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, A3 test
  card in the board.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `a77d48f921`.
- Artifacts:
  - core 173,120 bytes,
    `df08189ab3c767f4e2746b89586a2da031e08b1faeb6bef48d4a411d53681c41`;
  - package unchanged at
    `41fea833f4fd3ad01853a8febbf3352a3d7e12b8c3153e82d6ed6747c2241b7c`;
  - image 67,108,864 bytes,
    `77550d704b0deb6bd86454233f75390b9edd6a5a954fd9fb7c75fd158c24104b`,
    thirteen manifest entries, `fsck_msdos` clean, manifest matching the
    mounted contents, hash unchanged after verification;
  - on the card and checked against the manifest after writing:
    `C/sdboot-test` 38,140 bytes `45e43154...`, `C/sdload-test` 5,872 bytes
    `fdf14c49...`, `Libs/sdproof.library` 18,292 bytes `871c9bd4...`.
- Configuration: core with `P4_A5_PROBE=1` added to the A4 set; proof files
  built by `kernel-proof-esp32p4-riscv`, with `STARTUP_DEBUG=1` and
  `AUTOINIT_DEBUG=1` so the stretch between a program's entry point and its
  main() narrates itself.

Three files, none of which exists in the kickstart or the flash package, so
running any of them is evidence in itself.  `sdproof.library` is opened
through lddemon; `sdboot-test` is a normal AROS command; `sdload-test` is the
same proof built without the C startup, for reasons that emerged during the
run and are recorded below.

Audited on the host by `proof/audit-proof.py`, a companion to
`boot/audit-package.py` and deliberately a separate tool because it checks a
different loader: the package is placed by `kernel_elf.c`, these files by
`rom/dos/internalloadseg_elf.c`, and a relocation type one implements says
nothing about the other.  As in the package audit the accepted set is read out
of the loader's source rather than restated.  All three pass: 1,328, 178 and
593 relocations in 18, 14 and 19 types, every type implemented, all ELF32
little-endian `REL` RISC-V with OS/ABI AROS and flags `0x3, RVC, single-float
ABI`.

The library, through lddemon:

```text
[sdproof] init: id A5-sdproof-library-1, base 0x482bc370, marker 0x5d9200f1, id at 0x482bbc68
[a5]     library base at 0x482bc370  outside the kickstart and the package
[a5]     library id string at 0x482bbc68  outside the kickstart and the package
[a5]     SDProofQuery marker 0xf8920af4 expected 0xf8920af4  match
```

The marker is the point.  `SDProofQuery()` is a register-argument call at LVO
5 which returns the value its own initialisation wrote into the library base,
mixed with a value the caller passes in.  Neither side can satisfy that with a
constant, so a jump table built but wired to the wrong entry, or a base that
was never initialised, would fail it.

The command, non-interactively, from the AFTERDOS resident:

```text
[a5]     LoadSeg("SYS:C/sdload-test") = 0x482b9de4
[a5]     segment at 0x482b9de4  outside the kickstart and the package
[sdload] entered, id A5-sdboot-test-1-nostartup
[sdload] entry at 0x482b9df8
[sdload] id string at 0x482ba15c
[sdload] argsize 0x00000001 SysBase 0x4ff074c0
[sdload] sdproof base 0x482bc370
[sdload] query marker 0xf8920af4 expected 0xf8920af4  match
[sdload] passed, loaded from the card without the C startup
[a5]     RunCommand returned 0, the command reports success
```

And the same command interactively, typed at the Shell prompt:

```text
__startup_entry_body("\n", 1, 4ff074c0)
Entering __startup_fromwb()
[__startup_stdiowin] Entering
Entering __startup_initexit
__startup_main: entering main ...
[sdboot] entered main
[sdboot] id A5-sdboot-test-1
[sdboot] main at 0x482cdffa
[sdboot] id string at 0x482ce2e4
[sdboot] sdproof base 0x482bc370 version 1
[sdboot] query marker 0xf8920af4 expected 0xf8920af4  match
[sdboot] passed, loaded from the card
__startup_entry_body: returning 0
```

Note the two addresses for `sdproof.library`: `0x482bc370` in both the
AFTERDOS probe and the interactively run command.  It is the same open
library, opened twice, which is what a working lddemon should give.

**The finding that shaped the design, and it is not a defect of this port.**
`sdboot-test` run from the AFTERDOS probe reached `__startup_entry_body()`
with the right arguments and then stopped, with no trap and no further output
from anything - the whole boot with it.  Bracketing it needed two build
switches that did not exist: `compiler/startup/startup.c` and five files in
`compiler/autoinit/` all had `#define DEBUG 0` written into the source, so the
stretch between a loaded program's entry point and its `main()` could not be
made to narrate from a build command.  Both are now `#ifndef`-guarded with
`STARTUP_DEBUG=1` and `AUTOINIT_DEBUG=1` gates, the same treatment `dos`,
`dosboot` and `fat` got for A4.

With that, the last line was `Entering __startup_fromwb()`, and the cause is
plain in `compiler/autoinit/fromwb.c`: when the calling process has no CLI
structure, that function concludes the program was started from Workbench and
does `WaitPort(&myproc->pr_MsgPort)` for a `WBStartup` message.  `RTF_AFTERDOS`
runs inside dos.library's boot process, which has no CLI, so the wait never
ends; and since it is the boot process, `cliInit()` never returns, `__dos_Boot()`
is never called and the Shell never starts.  Any AROS would behave the same.

So the two routes are covered by the file each one fits: `sdload-test`,
without the C startup and therefore without that chain, for the resident
route, and `sdboot-test`, a normal command, from the Shell where a CLI exists.
The probe still loads and address-checks both and says in one line why it runs
only one.

Two smaller corrections made during the run, both mine:

- the address judgement treated the kickstart as one interval from
  `__text_start` to `__kernel_end`.  In an XIP build those are in different
  windows - code around `0x40000000`, data around `0x4ff00000` - so the
  interval spanned the entire external window and a segment correctly loaded
  into PSRAM was reported as being inside the kickstart.  It is now three
  intervals: kickstart code, kickstart data, and the package.  The link script
  warns about the same gap in its own comment;
- the AFTERDOS resident sat at priority 0, ahead of lddemon at -123, so the
  first `OpenLibrary()` of a disk-based library asked a question lddemon did
  not yet exist to answer, and hung.  Now -126, behind lddemon, shell and
  shellcommands.

- Acceptance points passed: DOS/LoadSeg executes the command from FAT and
  lddemon opens the library; the identities printed match `proof/proof_id.h`,
  which is the single place they are defined, and the file hashes on the card
  match the image manifest; the addresses of code and of rodata in all three
  files lie outside the kickstart and the flash package; the system remains
  alive, with 21 heartbeats and no trap in the run.
- Acceptance points still owed: the negative fixtures - missing, malformed,
  wrong-machine and deliberately unsupported-relocation files, each of which
  has to fail cleanly.  Those need files on the medium, which is the next
  step and the reason for the flash volume below.
- One acceptance point cannot be met as written, for the same reason as in
  A4: "the test succeeds through the normal boot path as well as the recovery
  path".  The normal path means `S:Startup-Sequence`, which requires
  `Open("CON:")`, which requires a `con` handler and a console device that
  are not in the package and belong to Track C.  `econsole` is mutually
  exclusive with it by design, since `BF_EMERGENCY_CONSOLE` sets
  `BF_NO_STARTUP_SEQUENCE` in `rom/dos/boot.c`.  What has been shown instead
  is that the load works from two genuinely different callers, a kickstart
  resident and a Shell, which is the substance of the point.  Recorded as met
  differently, as A1's two points were.
- Safety impact: no write path was exercised.  The same run's A4 probe
  reports the volume write protected and eight mutations refused, and
  `SYS:AROS.boot` hashing `0xf949eb96` before and after.  The card was written
  once on the host, deliberately, with all three file hashes checked against
  the manifest afterwards through a read-only mount.
- Remaining risk: iteration on a proof file costs a card handoff, and this
  phase took four.  That is the case for the flash-backed volume decided
  next.
- Next safe step: the flash `storage` partition as a read-only block device
  with a FAT16 volume, so the negative fixtures and everything after can be
  written with `esptool` instead of by hand.  Note that FAT32 cannot be used
  there: it needs 65,525 clusters minimum, that is 33.5 MB at 512-byte
  clusters, and the whole flash is 32 MB with `storage` at 15.875 MB.

### 2026-08-23 - A5 complete: the four refusals, served from memory

- State change: A5 `hardware partial` to `hardware verified`.  With this, M6
  is closed and Track A is complete through A5.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, A3 test
  card in the board, unchanged from the previous entry.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `43bd5afe37`.
- Artifacts: core 176,528 bytes,
  `d08df586ea1ca79f50e0072337f69a1f4cfbc6e6f18cdad9b3e1791ba8e8f85c`;
  package and image unchanged from the previous entry, so nothing was written
  to any medium for this.
- Configuration: as the previous entry.

The gate names four inputs that must fail cleanly: a missing file, a
malformed one, one for the wrong machine, and one carrying a relocation the
loader does not implement.  Only the first is about the medium.  The other
three are about what `rom/dos/internalloadseg_elf.c` does with the bytes it
is handed, so they are handed to it directly.

`InternalLoadSeg()` takes its read, seek, allocate and free functions as an
argument array - `LoadSeg()` itself only supplies four wrappers around
`Read()`, `Seek()`, `AllocMem()` and `FreeMem()` - so a caller may serve the
file from anywhere.  Serving it from memory is what made this possible at all
without more hardware handling: there is no writable filesystem in this
package, and `ram-handler` cannot be added to it, because its `residentpri` of
-125 puts it behind `dosboot.resource` at -50 in the COLDSTART pass, which
never returns.  Fixtures on the card would have meant a card handoff each.
The loader sees the same code path either way, which is the point of using its
own documented interface rather than a shortcut.

Each fixture is the known-good `sdload-test` mutated in one field, not a blob
invented for the purpose.  That is the difference between proving a rejection
and merely observing one: a hand-made file that fails proves only that
something about it was wrong, while a file differing from a proven-loadable
one in a single field pins the refusal to that field.  The unmodified bytes
are loaded first through the same memory path, so nothing below can be blamed
on the path.

```text
[a5rej]  missing file: LoadSeg = 0x00000000, IoErr 205  refused as not found
[a5rej]  reference image 5872 bytes, machine 243
[a5rej]  unmodified through memory: seglist 0x482ba4e4  loaded, the memory path is sound
[a5rej]  truncated to 40 bytes: seglist 0x00000000, IoErr 305  refused as not executable
[a5rej]  e_machine 243 changed to 3: seglist 0x00000000, IoErr 305  refused as not executable
[a5rej]  relocation type 23 at file offset 0x00000c34 changed to 200: seglist 0x00000000, IoErr 305  refused as not executable
[a5rej]  good load after the refusals: seglist 0x482bebe4  still works
[a5rej]  rejection cases passed
```

And, from the same run with `DOS_DEBUG=1`, the loader naming its own reason in
each case:

```text
[ELF Loader] elf_read_block (offset=0, size=52)          <- the 40-byte image
[ELF Loader] machine    is 3 - should be 243
[ELF Loader] Unknown relocation #0 type 200
```

That is what makes each refusal attributable rather than merely present: the
truncated image failed reading a 52-byte header out of 40 bytes, the wrong
machine was named by the field that was changed, and the unknown relocation
was named by number, at entry 0 of the first RELA section, which is exactly
where it was patched.  Type 23 is `R_RISCV_PCREL_HI20`; 200 is unassigned in
the psABI.  Each failure was followed by `freemem` lines for everything the
loader had allocated, so a refusal leaks nothing, and a real `LoadSeg()`
afterwards still works.

One correction to the test, and it made the check stricter rather than
weaker.  It expected `ERROR_BAD_HUNK` for the relocation case, which the ELF
loader does set - and which `InternalLoadSeg()` then overwrites, because the
last thing it does on any failure is `SetIoErr(ERROR_NOT_EXECUTABLE)`; its own
comment acknowledges that ELF "has a mess of SetIoErr() calls in it".  So 305
is the contract of the interface and the specific reason lives on the console,
not in `IoErr()`.  All three mutation cases now assert 305 through one shared
judgement, where before two of them asserted only "not zero".

A second correction, mine, caught by reading rather than by running: the
reference image's length was computed with three `Seek()` calls where two are
needed.  `Seek()` reports the position it had, not the one it moved to, so
going to the end and back to the beginning returns the length; the third call
returned zero and would have made the whole test report that it could not read
the reference command.

- Acceptance points passed, completing the phase: DOS/LoadSeg executes the
  command from FAT and lddemon opens the library (previous entry); command,
  library and UART identities match `proof/proof_id.h` and the file hashes on
  the card match the image manifest; missing, malformed, wrong-machine and
  unsupported-relocation inputs all fail cleanly, with the reason named and
  nothing leaked; the system remains alive - 21 heartbeats, no trap - and the
  medium is unchanged, `SYS:AROS.boot` hashing `0xf949eb96` before and after
  the same run's eight refused mutations.
- One point met differently and documented, as in A4: "through the normal boot
  path as well as the recovery path".  The normal path needs
  `S:Startup-Sequence` and therefore `Open("CON:")`, which needs a `con`
  handler and console device belonging to Track C, and `econsole` excludes it
  by design since `BF_EMERGENCY_CONSOLE` sets `BF_NO_STARTUP_SEQUENCE`.  What
  is shown instead is the load working from two genuinely different callers, a
  kickstart resident and a Shell.
- Safety impact: nothing was written to any medium in this step, on the host
  or on the board.  The mutations were made to a copy in RAM of a file read
  from a write-protected volume.
- Remaining risk: `ram-handler` cannot be used in this package while dosboot
  is present, which is a specific instance of a general trap worth
  remembering: any COLDSTART resident with a priority below -50 never
  initialises here.  Added to the risk table.
- Next safe step: the flash `storage` partition as a read-only block device
  with a FAT16 volume, to end the card handoffs for Track B and C.  FAT32
  cannot be used there: it needs 65,525 clusters, that is 33.5 MB at 512-byte
  clusters, and the whole flash is 32 MB with `storage` at 15.875 MB.  An
  Amiga filesystem was considered and rejected for now: it would need RDB,
  whose handler exists but which A2's hardening does not cover, and a second
  filesystem in the package where `fat` is already verified.

### 2026-08-23 - a FAT16 development volume in flash, read from the board

- State change: none to any roadmap phase.  This is infrastructure: the first
  two of three measured steps towards writing test content with `esptool`
  instead of by hand.  The generator can emit FAT16 and the board can read the
  volume; a block device and a boot node come next.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.  The A3
  card stayed in the board throughout and was not touched.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `63868f24d6`.
- Artifacts:
  - `aros-flashdisk.img` 4,194,304 bytes,
    `1ee0c7059c1471b1448d53b5ddfefc639c710e38ff6ad663d7da00ed4343035c`;
  - core 178,704 bytes,
    `3e235b2d577737f429056d2ec23c000600e743292f0bf0837e8e99115240b270`;
  - the A3 image rebuilt to
    `77550d704b0deb6bd86454233f75390b9edd6a5a954fd9fb7c75fd158c24104b`,
    byte-identical to before the generator changed, which is the regression
    check that matters here.
- Written to `ota_0` at `0x20000` and to `arosbsp` at `0xc00000`, both
  verified by esptool hash.  The second offset is new, and the standing
  authorisation in AGENTS.md was extended to cover it on the user's decision.

**Why FAT16 and not FAT32, and not an Amiga filesystem.**  FAT32 is only
FAT32 above 65,524 clusters, which is 33.5 MB at 512-byte clusters, and the
whole flash is 32 MB.  So the choice was FAT16 or an Amiga filesystem.  Both
have independent host tooling - `fsck_msdos` and a mount for one, `xdftool`
and `rdbtool`, both installed here, for the other - so that was not the
deciding factor as first assumed.  What decided it: an FFS volume in an MBR
partition is not recognised, because `rom/partition/partition_types.c` maps
type bytes to FAT, NTFS and Linux DosTypes, so it would need RDB, whose
handler exists but which A2's hardening does not cover.  A2 needed eleven
malformed tables to secure MBR and GPT; a new unhardened table path is not a
small price for a development convenience.  Beyond that it would mean a second
filesystem in the package, where `fat` is already there and verified.

**Where the volume lives, and why not in `storage`.**  The ESP-IDF table
declares `storage, data, fat, 0x1020000, 0xfe0000`, and that is unusable:
`krnP4FlashMap()` refuses anything at or past the 16 MB cache-mapping limit,
and `storage` begins at 16.9 MB.  Reading it would need raw SPI commands with
the cache suspended and the destination in internal SRAM rather than PSRAM,
which is a great deal of new risk for a convenience.  So the volume takes the
last four megabytes of `arosbsp` instead, which is a custom type this port
defines and whose internal structure is therefore its own to decide.  The
package uses 1,164,016 of the 4,063,232 bytes below the split, and
`kernel-package-esp32p4-riscv-checksize` fails the build if it grows past it,
because a package over the split would have the loader read filesystem bytes
as members.

**FAT16 in the generator.**  `mkfat32.py` grew a `--fat-bits` option.  The two
formats differ in less than they share: the width of a FAT entry, where the
root directory lives, how many reserved sectors precede the FATs, and the
layout of the boot sector's second half.  Everything else - the MBR, short and
long names, directory entries, cluster chains, the volume label entry - is
written once for both.  The FAT16 cluster count is checked against its own
range, 4085 to 65524, the same way FAT32's floor already was; a 3 MB volume
was rejected at 4031 clusters, which is how that check earned its place.

Verified by tools that did not build it: macOS reports the image as
`Windows_FAT_16` named `AROSP4DEV`, `fsck_msdos` finds 13 files and no errors,
and the two proof files read off a mount hash to the manifest values.  And the
FAT32 path is unchanged, proved by the A3 image rebuilding to the same
sha256 it had before.

**Read from the board**, through the same `krnP4FlashMap()` the package loader
uses, before exec exists and before any block device:

```text
[fdisk]  partition at 0x00820000 size 0x007e0000, volume at 0x00c00000 size 0x00400000
[fdisk]  MBR signature 0x0000aa55, hash 0xb239b878
[fdisk]  partition entry: type 0x0000000e, first sector 2048, sectors 6144, FAT16 LBA
[fdisk]  boot sector hash 0x623c3a14, signature 0x0000aa55
[fdisk]    bytes/sector 512, sectors/cluster 1, reserved 1, fats 2
[fdisk]    root entries 512, fat sectors 24, total sectors 6144
[fdisk]    serial 0xa5051d10, label 'AROSP4DEV  ', type 'FAT16   '
[fdisk]  flash volume probe passed
```

Every value matches the host.  The two FNV hashes are the decisive ones:
`0xb239b878` for the MBR and `0x623c3a14` for the boot sector, computed
independently from the image file and reproduced byte for byte from flash.
Type `0x0e` is what `partition_types.c` maps to `FAT\1`, the DosType the FAT
handler already registers, so the package needs no change at all.

A tooling improvement fell out of this and is worth keeping.  The probe runs
before the package load, and its output was invisible: `esptool --after
hard-reset` resets the board and exits, and a reader attaching afterwards has
already missed the first lines.  The capture script now opens the port first
and pulses RTS itself, the same line esptool uses, so nothing is lost in the
gap.  That also made the bootloader's own partition listing readable, which
independently confirms `arosbsp unknown 40 00 00820000 007e0000`.

- Acceptance: the generator emits a FAT16 volume that three independent host
  tools accept; the FAT32 output is unchanged; the board reads the volume's
  MBR and boot sector from flash with both hashes matching the host; and the
  A4 and A5 probes in the same run still pass, with no trap.
- Safety impact: the SD card was untouched.  The flash write went only to the
  region behind the package inside `arosbsp`, and the package size assertion
  ran before it.
- Remaining risk: `krnP4FlashMap()` has one scratch window and is not
  re-entrant, so a block device built on it has to serialise its reads.  The
  probe runs before the package loader for that reason and says so.
- Next safe step: the block device over this volume, then a boot node with a
  lower priority than the SD partition so a present card still wins, and a
  `bootdevice=` option to force the flash.

### 2026-08-23 - flashdisk.device, and partition.library over it

- State change: none to any roadmap phase.  Third and last measured step of
  the flash-volume infrastructure: the volume is now a block device, and the
  same discovery code A2 hardened finds the FAT16 partition through it.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.  The A3
  card stayed in the board and was not touched.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `a7e5a6a42c`.
- Artifacts: core 183,808 bytes,
  `090124a25639f7cf9070db9b82d317f4c025ffbb3711f448e220a347042147be`;
  `flashdisk_device.o` 13,088 bytes as a kickstart object.  The volume in
  flash was not rewritten, so its hash from the previous entry still stands.
- Test defines: `P4_A1_DIAGNOSTIC=1 P4_PARTITION_TEST=1 P4_DOS_PROBE=1
  P4_AFTERDOS_PROBE=1 P4_A5_PROBE=1 P4_FLASHDISK_PROBE=1 P4_FLASHDISK_TEST=1
  P4_HEADLESS_BOOT=1 P4_HEARTBEAT_TASK=1 P4_LDSCRIPT=ldscript-xip.lds`.
- Written to `ota_0` at `0x20000`, verified by esptool hash.

**Why it is a kickstart object and not a package member.**  This was settled
before any code was written, by reading `kernel_elf.c`: its `symbol_value()`
fails on `SHN_UNDEF`, so a package member cannot call `krnP4FlashMap()` at
all; it can only reach outside itself through library vectors.  A device that
reads flash therefore belongs where that function is linked, which is the
kickstart, the same place `timer.device` sits.  `BSP_DEVS := timer flashdisk`
is the whole mechanism, and `ramtest.device` from A2 was the template.

**What the test asks.**  The earlier probe read the volume through a map and
said the bytes are there.  This asks whether the stack above them behaves:

```text
[fddev]  flashdisk.device open
[fddev]  geometry: error 0, sector size 512, sectors 8192
[fddev]  TD_PROTSTATUS actual 0xffffffff, protected
[fddev]  CMD_WRITE: error 28 (TDERR_WriteProt), actual 0x00000000, refused as write protection
[fddev]  OpenRootPartition = 0x482b7ad0
[fddev]  table type 2
[fddev]    partition: start 2048, sectors 6144, dostype 0x46415401  FAT16
[fddev]  partitions found 1, as expected
[fddev]  read after discovery: result 0, hash 0xb239b878
[fddev]  flash block device test passed
```

The decisive line is the last read.  `ReadPartitionDataQ()` at offset 0 of the
root hands back `0xb239b878`, which is the MBR hash computed on the host from
the image file and reproduced in the previous entry from a raw flash map.  The
same bytes therefore arrive through three independent paths: the host's own
generator, `krnP4FlashMap()` before exec exists, and now a device request
served to `partition.library` after DOS is up.  Table type 2 is MBR, one
partition at sector 2048 of 6144 sectors, DosType `0x46415401` which is
`FAT\1`; every value matches what the generator wrote.

**A defect the test found in its own subject.**  The first run refused
`CMD_WRITE` with `IOERR_NOCMD` and left `io_Actual` at the caller's
`0xdeadbeef`, because writes fell through to the `default` case.  Both halves
are wrong for the same reason A4 established for `sdcard.device`: a filesystem
can act on `TDERR_WriteProt` and can only guess at `NOCMD`, and "nothing was
written" has to be readable from the reply without knowing what the caller
left in the field.  Writes are now named cases returning `TDERR_WriteProt`
with `io_Actual = 0`, which also makes them agree with what `TD_PROTSTATUS`
says one command earlier.  The test was tightened to require both properties
rather than merely a non-zero error, which is what caught it.

**Serialising the map.**  `krnP4FlashMap()` owns one scratch window and is not
re-entrant, the risk the previous entry recorded.  Every use in the device is
inside `Forbid()`, and requests are served in 64 KB pieces because that is one
MMU page and mapping costs a cache invalidate over what it covers.  `Forbid()`
is sufficient here only because no request waits on anything: the map and the
`CopyMem()` out of it are a few hundred cycles.  A future writing path, which
would have to erase and program, cannot use this pattern.

- Acceptance: the device opens; geometry agrees with the constant the volume
  was generated against; write protection is reported and every write refused
  as write protection with a zero count; `partition.library` finds exactly one
  FAT16 partition at the expected place; a read through the public API after
  discovery reproduces the host's MBR hash; and the A4 and A5 probes in the
  same run still pass, with no trap.
- Safety impact: the SD card was untouched and the volume in flash was not
  rewritten.  The only write was the core to `ota_0`.
- Remaining risk: the device is not mounted and has no boot node, so nothing
  reaches it through a path name yet.  Adding one puts it in dosboot's
  MountList next to the card's partition, and which of them wins is a
  decision with consequences, so it is the next step rather than part of this
  one.
- Next safe step: a boot node for the flash volume at a lower priority than
  the SD partition, so a present card still wins, plus a `bootdevice=` option
  to force the flash.  Then read `SYS:` content off the flash volume, which is
  the point of the whole exercise.

### 2026-08-23 - the flash volume mounted, and booted from

- State change: none to any roadmap phase.  This completes the flash-volume
  infrastructure: `FLASHDISK0P0:` is mounted on every boot, the card still
  wins when it is present, and `bootdevice=FLASHDISK0P0` boots the system
  entirely out of flash.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.  The A3
  card stayed in the board for all three runs and was not touched.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `1fc3707e8e`.
- Artifacts: three cores, all written to `ota_0` at `0x20000` and verified by
  esptool hash:
  - the boot-node run, 185,200 bytes,
    `81b83b68fac6103a28cd0d7433c109048704c4d0dde519feaa5c2f5f441532b6`;
  - with the DOS-level test, 187,392 bytes,
    `9532c0aba5075bd19c213a135240f9fd2e1faadd028cd4db2e7a5cf8be864d05`;
  - with `bootdevice=FLASHDISK0P0`, 187,408 bytes,
    `51d161ec1a9a8006b3054a1026dbdaeaf6e9f30923ebbc2a4b02ef6f8a4b406f`.
- The volume in flash was not rewritten; its hash from two entries ago stands.

**How the card keeps winning, without touching shared code.**  This was the
one real design question and reading settled it.  Both partitions end at
`DE_BOOTPRI` 0 and nothing in the MBR path changes that: `initPartitionHandle()`
copies the root handle's `DosEnvec` into each partition's, and the root's comes
from `TD_GETGEOMETRY`, which has no boot priority in it.  `Enqueue()` is FIFO
among equals.  So what decides the boot is the order in which the *whole-disk*
nodes were scanned, and `dosboot_BootScan()` walks them in MountList order,
which is priority order.  A negative priority on the flash device's whole-disk
node is therefore the entire mechanism: the card is scanned first, its
partition is enqueued first, and `dosboot_Init()` promotes the head of the
list to 127.  Observed exactly so:

```text
[FlashDisk] boot node fd0 at priority -10, 8192 sectors
[DOSBoot:bootscan] CheckPartitions('MMC0')
[DOSBoot:bootscan] AddPartitionVolume: AddBootNode(SDCARD0P0, 0, 0x46415402, NULL)
[DOSBoot:bootscan] CheckPartitions('fd0')
[DOSBoot:bootscan] AddPartitionVolume: AddBootNode(FLASHDISK0P0, 0, 0x46415401, NULL)
[DOSBoot] 3 devices in mountlist
[DOSBoot:bootstrap] dosboot_BootStrap: Attempting SDCARD0P0 with DOS
```

`0x46415402` is `FAT\2` and `0x46415401` is `FAT\1`, so the two volumes are
recognised as FAT32 and FAT16 respectively, from the MBR type bytes alone.
The override needed nothing added: `bootdevice=` is dosboot's own argument and
`selectBootDevice()` matches it against the DeviceNode name.

**The volume through DOS**, which is the point of the whole exercise:

```text
[fdvol]  volume name 'Arosp4dev', the generated label
[fdvol]  id_DiskState 80 (ID_WRITE_PROTECTED)
[fdvol]    blocks 6144, used 215, block size 512, disk type 0x444f5300
[fdvol]  AROS.boot from flash: 43 bytes, hash 0xf949eb96
[fdvol]  AROS.boot from card:  43 bytes, hash 0xf949eb96
[fdvol]  identical, FAT16 in flash and FAT32 on the card agree
[fdvol]  Open(MODE_NEWFILE) = 0x00000000, IoErr 214 (ERROR_DISK_WRITE_PROTECTED)
[fdvol]  DOS-level test passed
```

The cross-check is the sharp line.  The same staged file exists on the card as
FAT32 and in flash as FAT16, and both come back as 43 bytes hashing to
`0xf949eb96`.  Two filesystems on two media, served by two different block
devices, produce identical bytes; neither the generator nor the FAT handler is
transforming anything.

**Booted entirely from flash.**  With `bootdevice=FLASHDISK0P0` and the card
still in the board:

```text
[DOSBoot:bootstrap] dosboot_BootStrap: Attempting FLASHDISK0P0 with DOS
[sysfs]    blocks 6144, used 215, block size 512
[sysfs]  AFTERDOS probe passed, 8 mutation cases
[a5]     OpenLibrary("sdproof.library", 1) = 0x48403ee0
[a5]     SDProofQuery marker 0xf8920af4 expected 0xf8920af4  match
[a5]     LoadSeg("SYS:C/sdload-test") = 0x48401924
[a5]     RunCommand returned 0, the command reports success
[a5]     load proof passed
```

`SYS:` has 6144 blocks, so it is the flash volume and not the 129,024-block
card, and the A5 proof library and command were loaded and run from it.  That
is the Track B enabler stated plainly: new test code reaches the board with
one `esptool` command and no card handling at all.

**Two wrong expectations in the test, both mine, both instructive.**  The first
run reported the volume as `Arosp4dev`, not `AROSP4DEV`.  That is deliberate:
`GetVolumeIdentity()` in `rom/filesys/fat/volume.c` keeps the first character
of each word and lowercases the rest, so the eleven bytes on disk are not what
comes back.  DOS comparison is case-insensitive, so both spellings address the
volume; the test now compares without case and says why.  The second: `Info()`
reports `id_DiskType` as `ID_DOS_DISK` (`0x444f5300`), not the `FAT\1` DosType
the volume was mounted with, because `FillDiskInfo()` reports `ID_DOS_DISK` for
every volume the FAT handler serves.  The card's volume reports the same value,
which is how that was confirmed rather than guessed.  `Info()` is therefore not
a way to tell FAT16 from FAT32.

**A build trap fixed on the way.**  `P4_HEADLESS_BOOT=1` assigned
`P4_CMDLINE` plainly, and a variable set on the make command line beats a
plain assignment in a makefile.  So `P4_HEADLESS_BOOT=1
P4_CMDLINE="bootdevice=..."` silently dropped all three headless words and
would have booted without a console.  The assignment is now `override` and
appends what the caller passed, so the two combine.

- Acceptance: the flash volume is mounted as `FLASHDISK0P0:` on every boot;
  with a card present the card is booted and the flash volume is only mounted;
  the volume reports its generated label, its partition's block count and
  `ID_WRITE_PROTECTED`; a file read off it is byte-identical to the same file
  on the card; a mutation is refused with error 214; `bootdevice=` boots the
  system out of flash and the A5 proof library and command load and run from
  it; and the A4, A5 and block-device tests pass in every one of the three
  runs.
- Safety impact: the SD card was untouched in all three runs and the volume in
  flash was not rewritten.  The only writes were cores to `ota_0`.
- Remaining risk: the automatic fallback, meaning a boot with no card in the
  board, is not tested.  The mechanism is that `sdcard.device` registers no
  unit and therefore no boot node, leaving the flash volume as the only
  candidate, but a missing card is a different case from A4's card without
  `AROS.boot` and has not been run.  It needs the card physically removed.
- Next safe step: Track B, the MIPI-DSI framebuffer.  Test content for it can
  now be written with `esptool` and read from `FLASHDISK0P0:`, which is what
  this infrastructure was for.

### 2026-08-23 - the fallback, with no card in the board

- State change: none.  This closes the one point the previous entry recorded
  as untested.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.  The A3
  card was removed from the board by the user for this run.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `1a889c72ac`.
- Artifacts: core 188,240 bytes,
  `8a65559c092fba8159420e95f26f6ebf6e176dec7200f1fb52061e07e60c0034`, written
  to `ota_0` at `0x20000` and verified by esptool hash.  Test defines as in
  the previous entry, without `bootdevice=`.

With no card the chain is exactly what the design predicted, and nothing had
to be added for it:

```text
[P4SD00] GPIO45 high: no card present
[DOSBoot:bootscan] CheckPartitions('fd0')
[DOSBoot:bootscan] CheckPartitions('ECON')
[DOSBoot] 2 devices in mountlist
[DOSBoot:bootstrap] dosboot_BootStrap: Attempting FLASHDISK0P0 with DOS
```

`sdcard.device` reads the card-detect line, finds nothing and registers no
unit and therefore no boot node, so `fd0` is the only disk bootscan has to
scan and `FLASHDISK0P0` is the only candidate left.  `AvailMem` reports
30,636,976 bytes against 29,297,648 with a card present, the difference being
the SD stack's buffers never allocated.  The A4 probe, the block-device test,
the DOS-level test, the A5 load proof and the A5 rejections all pass, and the
run reaches the Shell prompt with a stable heartbeat.

**A defect in the A4 probe, found by this run.**  The first attempt reported
`AFTERDOS probe FAILED, 8 mutation cases`, with seven refusals at error 214
and the eighth, `Relabel`, at 218, `ERROR_DEVICE_NOT_MOUNTED`.  The target was
the compile-time constant `SDCARD0P0:`, which does not exist when no card is
in the board.  That is worth stating precisely because the constant was not
careless: `getdevpacketinfo()` in `rom/dos/packethelper.c` refuses anything
whose `dol_Type` is not `DLT_DEVICE`, so an assign or a volume name returns
218 without the packet ever reaching the handler, and an earlier version of
this probe had already been corrected from `SYS:` to a device name for that
reason.  What was wrong was pinning *which* device, since dosboot names that
node from the device, the unit and the partition position and it is
`FLASHDISK0P0` when the flash volume booted.

`krnP4SysDeviceName()` now finds it at runtime by resolving `SYS:` to a
handler port and matching that port against the device list, which is exact
and assumes nothing about what booted.  `-DP4_PROBE_DEVICE="..."` still pins
it if a build wants to.  A case that cannot be set up at all is now left out
of the count instead of being scored, so a missing device would read as seven
cases rather than as a failure.  With the fix:

```text
[sysfs]    SYS: is served by FLASHDISK0P0:
[sysfs]    Relabel(device): result 0x00000000, IoErr 214  refused as write protection
[sysfs]  AFTERDOS probe passed, 8 mutation cases
```

**A second, smaller one in the makefile.**  Making `P4_CMDLINE` an `override`
in the previous step left a trailing space in the string literal when the
caller passed nothing, which changed the core's hash for no reason and handed
the argument parser an empty word.  `$(strip)` fixes it, and the proof is that
the plain headless build now hashes to `9532c0aba507...` again, the same value
it had before `override` existed.

- Acceptance: with no card present the flash volume is booted automatically,
  `SYS:` is the 6144-block flash volume, all eight DOS mutations are refused
  with error 214, and the block-device, DOS-level, A5 load and A5 rejection
  tests pass.
- Safety impact: no card was in the board, so no medium could be touched.  The
  volume in flash was not rewritten.  The only write was the core to `ota_0`.
- Remaining risk: none outstanding from this step.  The regression with the
  card back in the board was run on the same core and is recorded below.
- Next safe step: Track B, the MIPI-DSI framebuffer.

**The regression, card back in.**  Same core,
`8a65559c092fba8159420e95f26f6ebf6e176dec7200f1fb52061e07e60c0034`, nothing
rebuilt and nothing rewritten, only a reset with the card in the board:

```text
[SDBus00] MMC0: [30436MB Capacity]
[DOSBoot:bootscan] AddPartitionVolume: AddBootNode(SDCARD0P0, 0, 0x46415402, NULL)
[DOSBoot:bootscan] AddPartitionVolume: AddBootNode(FLASHDISK0P0, 0, 0x46415401, NULL)
[DOSBoot:bootstrap] dosboot_BootStrap: Attempting SDCARD0P0 with DOS
[sysfs]    blocks 129024, used 2187, block size 512
[sysfs]    SYS: is served by SDCARD0P0:
[sysfs]  AFTERDOS probe passed, 8 mutation cases
[fdvol]  identical, FAT16 in flash and FAT32 on the card agree
```

The lookup reports `SDCARD0P0:` here and reported `FLASHDISK0P0:` with no card,
from the same binary, which is what it was written to do.  `SYS:` is the
129,024-block card again, all eight mutations are refused with error 214, and
the block-device, DOS-level, A5 load and A5 rejection tests pass.  Both boot
paths therefore work from one core with no build-time choice in it.

### 2026-08-23 - B0: the display contract, and three things it changed

- State change: B0 `not started` to `documented`.  No hardware was involved
  and nothing is claimed as verified.
- Hardware / revision: none used.  The contract records the target as ESP32-P4
  revision 1.3, which is what decides the register set.
- Source: worktree on `feat/riscv32-esp32p4-v2` at `ee92a00214`.
- Artifact: `arch/riscv-esp32p4/display/DISPLAY-CONTRACT.md`, 221 lines.
- Licence resolved: the copyright holder of the Vellum D1001 driver relicensed
  it for AROS on this date, which removes the AGPL question the phase raised.
  Espressif's JD9365 initialisation table stays Apache-2.0 in a file of its own
  with its SPDX header intact rather than being mixed into an APL file.

Every entry in the contract names an evidence class and a source file and
line.  There is no `verified` row, because nothing has been measured on this
port; `reference` means a product demonstrably drives this panel with that
value, which is a different and weaker claim.

**The timing contradiction is a defect, not a discrepancy.**  B0 was written
expecting to pick between two plausible sets.  It is not that.
`lcd_jd9365_config_t` declares `hsync, hbp, hfp, vsync, vbp, vfp`
(`lcd_jd9365.h:17,18`), `panel_lcd.c:184` fills them from the board header with
40/140/40 and 4/16/16, and `lcd_jd9365.c:55-60` then ignores the struct and
writes 20/20/40 and 4/30/30 as literals into
`esp_lcd_dpi_panel_config_t`.  So the values in the board header and,
identically, in the Seeed BSP have never driven this panel, and editing them
changes nothing.  Reported to the reference's author.  AROS starts from the set
that runs, 33.82 Hz by arithmetic, and B4 measures it.

Both sources label the mode 60 Hz.  Neither set produces it: 60 Hz at the
running totals of 880 x 1344 needs 70.96 MHz, not 40.  Recorded as
`unresolved` so it cannot be used as an expectation.

**The rotation direction is settled as a reference fact.**  B0 called it a
hypothesis.  Two independent paths in the reference agree on 90 degrees
clockwise: `panel_lcd.c:172` selects `ESP_LV_ADAPTER_ROTATE_270`,
counter-clockwise by that API's convention, and `lcd_rotation.h:13` maps the
raw path as `(logical_width - 1 - logical_x) * physical_width + logical_y`,
which is the same transform for 800 and 1280.  What stays open is narrower and
now stated as such: which physical corner the logical origin occupies, since a
180-degree mounting difference would be invisible to both paths.

**A correction to my own recommendation.**  I proposed the PPA as the primary
rotation path because `SOC_PPA_SUPPORTED` is 1 and IDF v6.0 ships
`esp_driver_ppa` with a rotation angle enum.  The working reference disables it
(`panel_lcd.c:214`, `enable_ppa_accel = false`) because IDF 6.0 needs an
out-of-tree workaround for rotated triple-partial refresh and its own updates
are small dirty regions.  That reason does not transfer, since AROS uses no
LVGL adapter, but neither does the reference's evidence.  Bring-up therefore
uses the CPU path, which has evidence, and the PPA becomes a measured
experiment.  The number that decides it: a full-frame CPU rotation is 4 MB of
PSRAM traffic per frame, 135 MB/s at 33.82 Hz, which is above what B1 aims to
establish.

**A register-set finding that would have been expensive to hit later.**  IDF
splits the ESP32-P4 register definitions by hardware version
(`components/soc/CMakeLists.txt:36-40`).  `mipi_dsi_host_reg.h` is
byte-identical between `hw_ver1` and `hw_ver3` apart from a copyright year,
which fits it being the unchanged DesignWare host core.
`mipi_dsi_bridge_reg.h` is not: 73 lines exist only in `hw_ver3`, adding
`DSI_BRG_DPI_TYPE`, `DSI_BRG_DPI_DBG_EN`, `DSI_BRG_DSI_BRIG_RST`,
`DSI_BRG_VSYNC_INT_CLR` and a version-date register.  This board is revision
1.3, so `hw_ver1`, and those five must not be touched.

**One deliberate deviation from the reference, recorded now rather than
discovered later.**  `d1001_board.c:165` sets the whole expander to output and
only then writes the individual levels.  The PCA9535 output register powers up
all-ones, so that order briefly drives LCD_BL_EN and AMP_EN high.  Harmless in
a product that wants the backlight on; not harmless in B2 through B4, which
must keep the panel dark.  This port writes the output latch first and changes
direction afterwards.

- Acceptance: met for the half documentation can settle.  One canonical table
  exists, evidence classes replace assertions, every source's licence and
  provenance is recorded, and the two entries that need hardware, the measured
  frame rate and the reference command trace, are marked deferred to B4 rather
  than dropped.
- Safety impact: none, no hardware was touched.
- Remaining risk: four items carried in the contract's own list, of which the
  sharpest is that IDF's PHY constants
  `set_switch_time(50, 104, 46, 128)` and `set_max_read_time(6000)` have no
  derivation in any local source and no ESP32-P4 technical reference manual is
  present on this machine.  They will be carried over as opaque constants with
  that note attached.
- Next safe step: B1, the 200 MHz PSRAM calibration.  It is the prerequisite
  with no display risk in it, and without the bandwidth headroom scanout has
  no margin at all.

### 2026-08-23 - B1: the calibration works, and the bus was never the problem

- State change: B1 `not started` to `hardware partial`.  The calibration half
  is verified; the bandwidth threshold is reassigned to B5 with a reason.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `75e72e03eb`.
- Artifacts: seven cores across the investigation.  The two that matter:
  `f3cce70cb4ccdc9e1e8b8df770c0cbfae4941b41f7646f4bce2247dd7a2a4a17`,
  165,792 bytes, 200 MHz PSRAM at the inherited CPU clock; and
  `402d57621df5b3ed3ba960b1f0f4122b383128a6ccd67eb83e7ba2116caea3e9`,
  172,080 bytes, the same with the CPU at 360 MHz and the stress test in.  All
  written to `ota_0` at `0x20000`, each verified by esptool hash.
- New build options: `P4_PSRAM_MHZ`, `P4_CPU_MHZ`, `P4_PSRAM_STRESS`, all off
  by default.

**The calibration.**  Two stages, and the reference block is written at 20 MHz
before the clock rises, so the write is never the thing being measured.  Four
DQS phases, then thirty-one steps of relative delay between strobe and data
with a hundred reads per step.  On this board:

```text
[psram]  tuning phases  1111  window 4, chose 67.5 degrees
[psram]  tuning delays  0000011111111111111111111111110
[psram]                                  ^
[psram]  window 25 of 31, index 17 means data delay 2, strobe delay 0
[psram]  calibrated, running at 200 MHz
```

Twenty-five of thirty-one steps pass, and the chosen index is the middle of
that run rather than its edge.  Six consecutive warm resets produced the
identical line, character for character, and so did the runs at 90 and at 360
MHz CPU, which is the evidence that the sampling does not depend on the core
clock.

All four phases passing means the phase stage discriminated nothing here and
the delay stage did all the work.  Worth recording rather than glossing: the
phase choice of 67.5 degrees is the first of a run of four, so it rests on the
tie-break rule and not on a measurement.

**Correctness over the whole window**, at 200 MHz, which `krnPSRAMVerify()`'s
one word per megabyte could not have shown:

```text
[stress] address uniqueness over 32 MB passed, write 43 MB/s, read 35 MB/s
[stress] four bus patterns over 32 MB passed
```

Every one of 8,388,608 words carries a value derived from its own address, so
an aliased mapping fails rather than passing on a constant.  Then all-zeros,
all-ones and both alternating patterns over the same 32 MB, which is what a
marginal sampling point fails on after surviving an address test.

**Three builds to find out the bus was not the limit.**  The phase assumed the
PSRAM clock was the constraint.  The first two measurements said otherwise:

| PSRAM clock | CPU | sequential read |
|---|---|---|
| 20 MHz | 90 MHz | 15 MB/s |
| 200 MHz | 90 MHz | 20 MB/s |
| 200 MHz | 360 MHz | 60 MB/s |

A tenfold bus clock bought a third more bandwidth; a fourfold CPU clock
tripled it.  Two intermediate hypotheses were tested and dropped on evidence
rather than argued about: `volatile` on the loads makes no difference, and
moving the loop itself into SRAM so it is not fetched from flash makes no
difference either, so neither load serialisation nor instruction fetch was the
constraint.

**The CPU ran at 90 MHz and nothing said so.**  `mcycle` against the 16 MHz
system timer measured it.  This port configures no CPU clock and inherited the
second-stage bootloader's: the CPLL was already at 360 with the CPU divider at
four.  ESP-IDF's own comment names the only three configurations the MEM<=200
and APB<=100 constraints allow, and the board was in the slowest of them.
`kernel/cpuclock.c` moves APB, SYS, MEM and CPU in that order for an upscale,
which is the order that keeps every intermediate state slower than both
endpoints; the reverse order would briefly run APB or MEM above its limit, and
ESP-IDF warns the hardware may silently correct an illegal divider without
reflecting it in the register, which would leave the real frequencies
unknowable.

```text
[clock]  as found  cpu /4  mem /1  sys /1  apb /1  root cpll
[clock]  set to    cpu /1  mem /2  sys /1  apb /2, asked for 360 MHz
[stress] cpu 360 MHz measured from mcycle against the 16 MHz timer
[stress] psram read, not volatile  60 MB/s
[stress] sram read, same loop      101 MB/s
```

APB ends at 90 MHz either way, which is why nothing clocked from it needed
reconfiguring and the console and timer were unaffected.  The 100 Hz heartbeat
still counts 500 ticks per five-second interval.

**Why the 100 MB/s gate cannot be argued from this.**  The loop costs 13.6
cycles per word from internal SRAM at 360 MHz, and it cost 13.6 at 90 MHz.  A
figure that does not move with the clock is a latency: 64-byte lines are one
fill per sixteen words, and a scalar loop with no prefetch has one fill
outstanding at a time.  So this measures how fast one thread pulls a cache
line, and a display does not read its framebuffer that way.  Scanout is a DMA
read that pipelines, and the number that matters cannot exist before a DMA
engine drives the bus.  The threshold moves to B5 and the risk table now says
that any scanout bandwidth claim before then is unfounded.

- Acceptance passed: the full-window address and pattern tests at 200 MHz under
  both CPU clocks; six warm resets choosing an identical 25-of-31 window; the
  20 MHz path unchanged; the fallback returning to it with neutral sampling;
  and the A4, A5, block-device and DOS-level storage tests all passing at
  360 MHz CPU with 200 MHz PSRAM.
- Acceptance not met and reassigned: the 100 MB/s figure, to B5, with the
  reason above.
- Safety impact: no medium was written.  The calibration's scratch is 128 bytes
  at PSRAM offset 0x80, before exec exists and before the memory header is
  created.  The only writes were cores to `ota_0`.
- Remaining risk: everything else inherited from that bootloader is
  unexamined in the same way the CPU divider was, the flash clock and the cache
  configuration in particular.
- Next safe step: B2, the PCA9535 panel power sequence, which touches no data
  path at all.

**The SD path at 360 MHz**, run afterwards on the same core with the card back
in the board.  This was the one open regression, because MEM_CLK doubled and
the card is read by IDMAC:

```text
[clock]  set to    cpu /1  mem /2  sys /1  apb /2, asked for 360 MHz
[SDBus00] MMC0: [30436MB Capacity]
[DOSBoot:bootstrap] dosboot_BootStrap: Attempting SDCARD0P0 with DOS
[sysfs]    blocks 129024, used 2187, block size 512
[sysfs]  read SYS:AROS.boot = 43 bytes, hash 0xf949eb96
[sysfs]  AFTERDOS probe passed, 8 mutation cases
[a5]     load proof passed
[a5rej]  rejection cases passed
```

The card wins the boot as it should, `SYS:` is the 129,024-block FAT32 volume,
and `AROS.boot` hashes to `0xf949eb96`, the same value every earlier run
produced and the same value the FAT16 flash volume produces.  No CRC error, no
data timeout and no `refusing unaligned` retry appears anywhere in the capture;
the only nine matches for those patterns are the FAT handler's own write
refusals, which is what the A4 probe asked for.  No trap, and the heartbeat
counts 500 ticks per five seconds throughout.

### 2026-08-23 - B2: panel power, and two defects hardware found

- State change: B2 `not started` to `hardware partial`.  The sequence is
  verified on a warm board; against a cold expander it is not.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, CPU at
  360 MHz.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `a67331f4b2`.
- Artifact: core 175,024 bytes,
  `c49b3e8d21d138bf3679f7f1c551ddaea1e6709f3daa29999e020ed92edca1f4`, written
  to `ota_0` at `0x20000` and verified by esptool hash.
- New build option: `P4_PANEL_PROBE=1`, off by default.
- Nothing on the data path was touched: no LDO, no DSI, no command to the
  panel, and the backlight was never enabled.

**AROS already has an I2C subsystem, and it decided the shape.**
`workbench/hidds/i2c` provides `CLID_Hidd_I2C` under `CLID_Hidd_Bus` with
`WriteRead` and `ProbeAddress` alongside byte-level methods, and
`arch/riscv64-opensbi/hidd/dwi2c` is the precedent for a real controller under
it: an OOP-free `dwi2c_hw.c` whose entry point is
`DWI2C_HWTransfer(ctrl, address, ...)`, a thin class file above it, hardware
implementations of `WriteRead` and `ProbeAddress`, and `Start`, `Address`,
`PutByte` and `GetByte` refusing with a log line because a command-FIFO
controller cannot hold the bus between calls.

`kernel/i2c_hw.c` is written to that shape.  It takes a port, two GPIOs and a
bus rate, so it covers both of the P4's controllers, and its one data-moving
entry point is `krnP4I2CTransfer(address, wbuf, wlen, rbuf, rlen)`, which is
the `WriteRead` method's signature with the object removed.  A later
`i2c-esp32p4.hidd` should compile this file rather than reimplement it.  It is
in `kernel/` for now because the panel sequence has to be provable before
there is a graphics stack to hang a HIDD from; it belongs in
`arch/riscv-esp32p4/i2c/` once one exists.

**The first defect: push-pull instead of open drain.**  The first bus scan
found exactly one device at 0x6a where four are documented, and it found the
same one at 10 kHz, so a rise-time problem it was not.  The raw status words
said the hardware was behaving: a genuine NACK at 0x20 and a genuine
completion at 0x6a.

The cause was two bits I set that ESP-IDF never touches.  `I2C_SDA_FORCE_OUT`
and `I2C_SCL_FORCE_OUT` are documented as "1: Direct output, 0: Open drain
output" with a default of zero, and I set both, reasoning about them as
"force the peripheral to own the pin".  With the master driving SDA high
through the acknowledge slot, a slave pulling it low is fighting a push-pull
driver, and only whichever device wins that contest is ever seen.  One device
answering out of five is the signature of exactly that.  Cleared, the bus is
populated:

```text
[panel]  i2c1 sda 20 scl 21 at 100 kHz: 0x18 0x20 0x40 0x51 0x6a
```

0x18 is the codec, 0x20 the port expander, 0x51 the real-time clock and 0x6a
the inertial sensor; 0x40 is unidentified and recorded as such rather than
guessed at.  `i2c0` on GPIO37/38 answers nothing, which is consistent with the
touch controller sharing the panel rail that this phase deliberately leaves
off, and is not evidence of a fault.

**The second defect: the expander is not reset by a CPU reset.**  This is the
finding worth keeping.  The PCA9535 has no reset pin and keeps its direction
and output registers across every reboot, so on this board it is found with all
sixteen pins already outputs, left that way by whatever firmware ran last.  The
phase's own rule - preload a safe latch, preserve unrelated bits - was written
for a cold device, and a whole-register write to a warm one drives eleven pins
nobody asked about.  My first version did that and pulled the battery-charge
enable low, which the run reported.  Every write is now read-modify-write
against the device's own register, and the four owned bits are the only ones
any value in this file can change.

The check had to change too.  "Every other bit is an input" is only true on a
cold board and would have passed vacuously here; what is verifiable is that the
four owned bits are outputs, that no other direction changed, and that no level
outside those four moved:

```text
[panel]  as found: config 0x00000000, output 0x00000100
[panel]  our four are outputs, every other direction unchanged
[panel]  no level outside our four moved
[panel]  after the reset pulse: output 0x00000105, powered, reset released
[panel]  returned to safe, reset asserted and dark
[panel]  B2 passed, twice
```

**A third, smaller one.**  The transport's wait was bounded by a loop count,
and on a bus with nothing on it - where neither a NACK nor the controller's own
timeout arrives - scanning 112 addresses took long enough to look like a hang.
It is bounded by the system timer now, 50 ms against a longest allowed transfer
of about 30 ms.  A loop count is also a bound whose meaning changes with the
CPU clock, which B1 has just shown can be somewhere nobody expected.

- Acceptance passed: configuration and output read back matching each intended
  state; the sequence run twice with no unrelated bit moved and PWR_HOLD held
  throughout; every rail, reset and backlight transition bounded and logged;
  and every failure path returning to reset asserted with the backlight dark,
  which the two aborts in the first runs exercised for real.
**The board has a battery, so there is no cold expander to test against.**
Removing USB does not power the board down: the battery keeps it alive, and the
run after unplugging and replugging read exactly the same registers as the one
before it.  The only way to drop the rails is to release PWR_HOLD while on
battery, which powers the whole board off deliberately, so the expander's
datasheet defaults are not reachable by any accident and not reachable at all
without that decision.

Which made the preservation check weak in a way worth fixing rather than
excusing: every unrelated bit was already zero, and preserving zero proves
nothing about read-modify-write.  So the probe now seeds one first.
BAT_READ_EN is set high before the passes, chosen because the reference driver
holds it high in normal operation, and written directly rather than through the
panel driver because refusing to touch bits outside its four is the property
under test:

```text
[panel]  seeded bit 6 high, output now 0x00000140
[panel]  claimed: output 0x00000140, supply off, in reset
[panel]  after the reset pulse: output 0x00000145, powered, reset released
[panel]  after both passes, output 0x00000140, the seeded one survived
[panel]  B2 passed, twice
```

`0x145` is the four owned bits in their powered state plus the seeded one, and
nothing else.  That is a stronger statement than a cold start would have made,
because the pattern was chosen to contain a one where the cold default would
have been indistinguishable from an accident.
- Safety impact: no medium was written and no data path touched.  The backlight
  was never enabled and the panel supply was returned to off.  One unintended
  change was made and is recorded above: the battery-charge enable was pulled
  low by the first version, which per the reference means charging enabled,
  the harmless direction.
- Remaining risk: the audio amplifier enable is one of the bits this port now
  preserves rather than controls, and its safe value depends on what ran
  before.  If a future phase needs the codec, that bit becomes someone's
  responsibility and it should be claimed explicitly rather than inherited.
- Next safe step: B3, the LDO and DSI PHY with the JD9365 command sequence,
  which is the first step that touches the data path.

### 2026-08-23 - B3 stage one: the PHY locks

- State change: B3 `not started` to `hardware partial`.  The supply, clocks and
  PLL are verified; command mode and the panel's own sequence are not started.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, CPU at
  360 MHz.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `30224ba0d6`.
- Artifact: core 178,384 bytes,
  `c8d9e22f206d051dbb2f829a708e987c1a29c6b3d019d42f4aa9693058d424a1`, written to
  `ota_0` at `0x20000` and verified by esptool hash.
- New build option: `P4_DSI_PROBE=1`, off by default, and it needs
  `P4_PANEL_PROBE=1` because the panel has to be powered and out of reset first.
- The backlight was never enabled, so nothing was displayed and nothing could
  be.

Passed on the first hardware run, which is worth saying plainly because it is
unusual in this port and it is the consequence of deriving the whole sequence
and writing it down before writing any code:

```text
[dsi]    ldo 0x40200180 ana 0x93000000, dref 9, mul 6, enabled
[dsi]    pll n 2 m 50, range 0x2a, 1000 Mbit/s per lane if the reference is 40 MHz
[dsi]    phy status 0x000015bd  locked, lanes in stop state
[dsi]    B3 stage one passed
```

`0x15bd` has bit 0, the PLL lock, and bits 2, 4 and 7, the stop states of the
clock lane and both data lanes.  The LDO reads back `dref 9, mul 6` with the
enable set, which is the exact uncalibrated solution for 2500 mV.

**The lock is evidence for something no register states.**  On revision 1.x the
PHY PLL reference source select and its divider do not exist, so the reference
is fixed by hardware and nothing reports what it is.  N=2 and M=50 are the
exact solution for 1000 Mbit/s from 40 MHz, the range selector 0x2A is the one
for [1000, 1050) Mbit/s, and a PLL given a reference of any other frequency
with those factors would either fail to lock or land outside that range.  It
locked, so the reference is the crystal.  That was the one assumption this
stage existed to test.

**Three writes the reference driver makes were left out.**  `hw_ver1` has no
`MIPI_DSI_DPHY_PLL_REFCLK_SRC_SEL`, no `_DIV_NUM` and no
`DSI_BRG_DSI_BRIG_RST`; all three are `hw_ver3` additions, and ESP-IDF reaches
them through configuration guarded on the chip revision.  Writing them here
would have been writing reserved bits.  This is the second time B0's
register-set finding has changed code rather than merely being recorded, and it
is the reason that finding was worth making before any of this was written.

Two smaller things this stage does differently from the reference.  It clears
the PHY test interface before the first PLL write, because a stale address left
in it would send that write somewhere else.  And both waits are bounded by the
system timer with distinct failure names, where the reference loops on each
condition with no bound at all; a bring-up step that can hang is worse than one
that reports which of the two conditions it was.

- Acceptance passed, for the half this stage covers: every clock and PHY wait
  is bounded and each failure names which condition it was; the sequence is
  recoverable, with `krnP4DsiPhyDown()` reversing it in the opposite order and
  removing the supply last; and a failure returns to UART with the panel back
  in reset and the backlight dark, which the panel-claim failure path exercises.
- Acceptance not yet met: the command path.  Entering command mode, sending the
  JD9365 sequence over DBI and attempting the DCS `0x04` identity read are
  stage two.
- Safety impact: no medium was written.  The panel supply was raised and
  lowered, the backlight was never enabled, and the panel was returned to reset
  before the probe finished.
- Remaining risk: the LDO's eFuse trim is still not read, so the rail is at its
  nominal 2.5 V rather than a per-part corrected one.  The PLL locking says the
  rail is close enough for the PHY to work at room temperature on this part; it
  says nothing about margin.
- Next safe step: stage two.  Command mode, the JD9365 sequence over DBI with
  the backlight still dark, and the identity read attempted without an expected
  value because no source states one.

### 2026-08-23 - B3 stage two: the host transmits, the panel says nothing

- State change: none.  B3 stays `hardware partial`.  Command mode and the
  panel sequence are implemented and the transmit side is clean; the read path
  does not work and is recorded as an open defect rather than worked around.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, CPU at
  360 MHz.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `d3621e8941`.
- Artifact: core 181,600 bytes, written to `ota_0` at `0x20000` and verified by
  esptool hash.
- The backlight was never enabled.  `0x29`, display on, was sent, so the panel
  is driven and nothing is lit.

**What works.**  The host enters command mode with the clock lane in low power
and every command type configured for low-power transmission, and then the
whole sequence goes out: page unlock, `MADCTL` 0x00, `COLMOD` 0x55, two data
lanes, and the eight-command vendor table ending in display on.  No command
FIFO ever failed to drain and no protocol error was ever flagged.

**What that is worth, which is less than it sounds.**  A DSI write is not
acknowledged.  The host transmits and moves on, so a sequence that completes
says the host sent it and nothing whatever about whether the panel listened.
The identity read was the one chance of panel-side evidence in this stage, and
it produced none.

**Five reads, all silent, and the registers say why not.**  0x04 is
vendor-defined and a panel may simply not implement it, so four standard DCS
reads were tried as well: power mode, address mode, pixel format and scanline.
All silent.  The host's own registers were then read after a failure:

```text
[dsi]    dcs 0x0a power mode: no reply, pkt 0x00060054 int0 0x00000000 int1 0x00000000
```

`0x54` is read-command-busy set, both payload FIFOs empty, the command FIFO not
full.  So the read went out, the host is still waiting for a reply, and neither
interrupt-status register flags anything.  That is not a protocol error; it is
silence.  A failed read also leaves the host waiting, so every later read
stacks on a busy controller and means nothing - the probe now stops at the
first silence, which is why only one status line appears.

**The reference answers this read, so the defect is here.**  Espressif's panel
driver reads register 0x04 during init with `while
(mipi_dsi_host_ll_gen_is_read_fifo_empty(...))` and no bound at all.  If this
panel did not answer, the reference would hang rather than continue, and the
reference runs on this board.  So the panel answers there and this port's read
path is wrong.  That is a deduction from the reference's structure rather than a
measurement, but it is a sound one and it is what makes this a defect rather
than a board fact.

Two hypotheses were tested and eliminated:

- **The software reset.**  The first version sent `SWRESET` before the read,
  reasoning that the Espressif panel driver's reset function does so.  It turns
  out that function is never called on this board: the board layer pulses the
  reset line through the port expander itself and goes straight to the panel's
  init, so the working path contains no software reset at all.  Removing it is
  a correction worth keeping either way, and it changed nothing.
- **The divider encoding.**  Espressif dividers are often written as the value
  less one; `set_escape_clock_division` and `set_timeout_clock_division` are
  not, so the 7 and 13 this port writes are what the reference writes.

What has not been eliminated: some host register the reference sets that this
port does not, the semantics of the maximum-read-time value, and whether the
escape clock is actually running at the rate the divider implies.

**This does not block B4.**  The internal test pattern generator needs the
initialisation sequence to have been accepted; it does not need the read path.
So B4 is both the next step and the first thing that can tell whether these
writes landed, which is the same question the read path was supposed to answer.
If a pattern appears, the writes are landing and the read path is a separate
bug in a separate direction.  If nothing appears, both are suspect together.

- Acceptance passed: command mode is entered, every wait in the command path
  is bounded and names which condition failed, the sequence completes without
  host error, and a failure leaves the panel in reset with the backlight dark.
- Acceptance not met: the identity read.  B3's own text says not to invent an
  expected value, and this port does not; but it also expected a stable
  response to become a documented board fact, and there is no response.
- Safety impact: no medium was written, the backlight was never enabled, and
  the panel was returned to reset before the probe finished.
- Remaining risk: no panel-side confirmation of anything exists yet.  Every
  claim in this entry is about what the host did.
- Next safe step: B4, the internal test pattern, which produces the first
  panel-side evidence and does not depend on the read path.

### 2026-08-23 - B4: the host is clean, the panel is dark

- State change: B4 `not started` to `blocked`.  Everything on the SoC side is
  implemented and reports success; nothing appears on the panel.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3, CPU at
  360 MHz.
- Source: dirty worktree on `feat/riscv32-esp32p4-v2` at `57db021f59`.
- New build options: `P4_PATTERN_TEST=1` and `P4_BACKLIGHT_ONLY=1`, both off by
  default.

**What the host reports.**  The pattern generator runs with the timing the
contract's Set A produces, and the arithmetic came out exactly as derived:

```text
[b4]     host timing: hsa 63, hbp 63, hact 2499, hfp 125, hline 2750
[b4]     880 x 1344 at 40 MHz is 33.82 Hz, derived and not measured
[b4]     after 100 ms: pkt 0x00050015 int0 0x0 int1 0x0 brg 0x0  no underrun
```

**Two real findings on the way there.**  The first version left the DSI bridge
untouched, on the reasoning that a pattern generator inside the host is
self-contained.  It is not: the host's DPI interface is fed by the bridge, and
with the bridge off there is no timing to hang a frame on.  Enabling the bridge
fully then produced `DPI_PLD_WR_ERR` in the host's second interrupt-status
register - the bridge pushing pixels into a host that generates its own and
consumes none.  The answer is `dsi_en` set and `dpi_en` clear, two bits that
exist separately for exactly this.  In B5 it will be the other way round.

**The frame rate is derived and cannot be measured here.**  Revision 1.x has no
VSYNC interrupt in the bridge, only underrun, and the panel's scanline register
is behind the read path that does not answer.  So there is no event this port
can count, and B4's gate asking for a measured VSYNC is not satisfiable in
software on this revision.  What would satisfy it: an external instrument, or
the read path working.  33.82 Hz is arithmetic from an exactly divided clock and
the programmed totals, and it is labelled as such wherever it appears.

**The backlight, which turned into its own investigation.**  Three attempts,
each eliminating something:

1. A static high on GPIO14 with the expander's enable set.  Panel dark.  Every
   register read back asserted, so the first suspicion was the reading rather
   than the writing - and it was partly right: the pad's input enable was off,
   so the level read was a blind register.  Fixed, the pin reads high.
2. Still dark with the pin verifiably high.  The reference drives a 5 kHz PWM
   rather than a level, and a backlight driver whose dimming input wants a
   switching signal treats DC as nothing, so LEDC was implemented.  The first
   version produced a constant high: 20,000 of 20,000 samples.  The timer has
   its own parameter-commit bit, separate from the channel's, and without it the
   divider and resolution never take effect, the counter never advances, and the
   channel holds its output at whatever the comparison gives with the counter at
   zero.  Which is high, and looks exactly like a working full-brightness
   backlight that lights nothing.
3. With that bit set the pin carries a real waveform, 3,680 of 20,000 samples
   high, 18 per cent against 20 requested, which is sampling noise rather than
   misconfiguration.  The panel is still dark.

So the backlight path is asserted end to end and measured, and the panel does
not light.  Every register in it matches the vendor BSP and the working
reference: GPIO14, expander bit 0 for the panel supply, bit 7 for the backlight
enable, ten-bit resolution, non-inverted duty.

**The leading hypothesis, and why the isolation test was inconclusive.**  The
`P4_BACKLIGHT_ONLY` build was meant to separate "the backlight path is wrong"
from "something in the DSI sequence undoes it".  It cannot, if the panel
module's LED string is fed from a rail the panel itself brings up once it is
initialised - which some integrated modules do.  In that case a backlight can
never be tested in isolation on this hardware, the isolation result means
nothing, and the real question is the one that was already open: whether the
JD9365 sequence is landing at all.  Which is also what the five silent DCS reads
suggest.

- Acceptance not met, and this is the phase's own gate: no pattern is visible,
  so nothing can be said about geometry or RGB order.
- Acceptance not satisfiable on this revision: a measured VSYNC.
- Safety impact: no medium was written.  The backlight is only ever enabled
  after a pattern is on the link, never on a failure path, and at a fifth of
  full brightness.
- Remaining risk: the whole display path has produced no panel-side evidence of
  any kind - not one reply, not one pixel.  Every claim about it is a claim
  about what the SoC did.
- Next safe step: the decisive test is not another register.  Flashing the
  working firmware and confirming the panel lights would separate "this port's
  software" from "this board's hardware or its connector" in one attempt, and
  that is the user's call because it overwrites AROS.  Failing that, the next
  software step is to compare against the reference at the level of a full
  register dump after its own initialisation rather than function by function.

### 2026-08-23 - PSRAM stopped answering, and the board cannot be cold-started

- State change: none to a phase.  This records a regression that blocks the
  display work and a failed attempt to clear it.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 revision 1.3.

**What happened.**  PSRAM identified normally all session and then stopped,
with the binary unchanged: `panel7.log` reports 32 MB at 20 MHz and
`panelcold.log`, the very next run of core
`c49b3e8d21d138bf3679f7f1c551ddaea1e6709f3daa29999e020ed92edca1f4`, reports a
floating bus.  Between them the user unplugged and replugged USB.  Since then
every boot reads vendor 0x1f and mode register 2 as 0xff, which is no chip
answering at all.

That it was the same binary is the important part: this is not a change I made.

**What has been measured since, rather than assumed.**  The MPLL bring-up
returned one value for two different faults, so it now names which and reports
the PLL's own registers, read back over the configuration bus:

```text
[psram]  mpll   state 0x00709931, ana_pll_ctrl0 0x0000034c, bus 20 MHz, identify tried 4
[psram]  chip   no answer - vendor 0x0000001f mr2 0x000000ff
```

`0x99` in the divider byte is exactly what this port writes: a divider field of
19 and a reference divider of 1, which is 400 MHz.  `0x34c` has both the
calibration-end and calibration-stop bits set.  The bus is at the 20 MHz that
has always worked.  So the PLL is configured as intended, the calibration
completed, and the chip is silent regardless.

The identify is now retried three times, on the reasoning that the chip's mode
registers survive a CPU reset exactly as the port expander's do, so a boot
after other firmware finds the part in a bus width and latency it did not
choose - and mode register 8 selects the width, so a write sent at the wrong
width may not be received at all.  Three attempts change nothing here.  The
retry is kept because the reasoning holds independently of this fault.

**The board cannot be cold-started from software.**  Two attempts, and the
second was clean: the image was written with `--after no-reset` so the countdown
started when this port chose, the user unplugged USB with fifty-odd seconds to
spare, and PWR_HOLD was released with the board on battery.  The next boot read
the expander's direction register as `0x0000`, all outputs, where a cold PCA9535
reads `0xffff`.  So the board did not lose power and releasing PWR_HOLD does not
switch it off, which means the assumption about that pin was wrong.

This has a consequence beyond PSRAM: B2's preservation check can never be run
against the expander's datasheet defaults, and the seeded-one test remains the
strongest form available.  The risk table already said the cold state needs
deciding to reach; it now says it cannot be reached from software at all.

- Remaining risk: the display work needs PSRAM, because following the working
  reference means a real framebuffer over DW-GDMA rather than the host's
  pattern generator.  While PSRAM is silent that path cannot be built against
  hardware.
- Next safe step: none in software that I can see.  What would separate the
  remaining possibilities is a physical power interruption - the battery
  disconnected, or whatever button or connector the board provides - and that
  is not something this port can do to itself.

### 2026-08-23 - PSRAM bring-up compared against ESP-IDF value by value: no deviation

- State change: the bring-up now follows `esp_psram_impl_enable`'s order step
  for step, and the connected check runs where the reference runs it.  Neither
  changed the outcome.  PSRAM remains silent.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, battery attached.
- Test defines: none, then `P4_CPU_MHZ=360`, both with
  `P4_LDSCRIPT=ldscript-xip.lds`.  Core 163,904 bytes, written to `ota_0` at
  `0x20000`, esptool-hash verified on each write.

**The comparison, and it found nothing.**  Every value this port writes was read
against ESP-IDF's own source and matches it:

| what | reference | this port |
| :--- | :--- | :--- |
| MPLL target | `AP_HEX_PSRAM_MPLL_DEFAULT_FREQ_MHZ` 400 | `P4_PSRAM_MPLL_HZ` 400000000 |
| MPLL divider byte | `(400/20-1)<<3 \| 1` = `0x99` | measured `0x99` |
| latencies, slow set | rd 2, wr 2 | 2, 2 |
| latencies, fast set | rd 4, wr 1 | 4, 1 |
| dummy bit lengths | `2*(5-1)`, `2*(10-1)`, `2*(5-1)` | identical |
| CS setup / hold / hold delay | 4 / 4 / 3 | 4 / 4 / 3 |
| `SMEM_AC` field positions | `[6:2]`, `[11:7]`, `[30:25]`, bit 31 | identical |
| DLL bit | bit 5, in both timing registers | bit 5, both |
| DQS strobe | `XPD` bit 0, both DQS pins | identical |
| pin drive | 2, all twenty pins | 2, table of twenty |
| mode register commands | `0x4040` read, `0xC0C0` write | identical |
| vendor id expected | `0xD` | `0x0D` |
| ROM transaction entry points | `0x4fc00108/010c/0110` | identical |
| ROM operating mode | `OPI_DTR` = 7 | 7 |
| chip-select mask | `1 << 1` | `1 << 1` |
| controller used for mode registers | MSPI id 3 | 3 |

Three real deviations existed and two are now gone.  The order: the reference
sets the pin drive and the strobe, then the analogue timing, and only then the
divider; this file set the divider first.  `krnPSRAMClockUp` is split into
`krnPSRAMControllerUp` plus the divider so the steps can run between them.  The
connected check: the reference writes a word to address zero and reads it back
*before* reading the identity, this port did it after and returned early on the
identity, so it never ran.  The third stays: the mode registers are written
absolutely rather than read-modify-write, because a read before the chip is
configured returns a floating bus, and the reasoning for that is unchanged.

**The chip does not carry data.**  This is new, and it is a stronger statement
than every previous run made.  `krnPSRAMRoundTrip` now runs before the identity
read, and it fails: a word written to address zero does not come back.  So the
failure is not that the mode registers cannot be addressed while the array
works.  Nothing crosses the bus in either direction.

**The handover state, measured.**  `krnPSRAMEntryRead` reads what the
second-stage bootloader leaves, and the first version of it hung the boot dead -
which is itself the first measurement.  Reading `SPI_MEM_S_SRAM_CLK` before the
MPLL is up does not fault, it waits: the bootloader leaves
`psram_clk_src_sel = MPLL` while the MPLL is off, so those registers have no
clock.  The reads were moved to where they answer, after the module clock and
before the reset, and the state is:

```text
[psram]  entry 0xe6df97ef 0x0000d05d 0x00000100 0x00030103 0x00030103
               0x00000001 0x00000001 0x8000b084 0x00000000
```

In order: `soc_clk_ctrl0`, `peri_clk_ctrl00`, `hp_rst_en0`, MSPI2 `sram_clk`,
MSPI3 `clock`, `timing_cali`, `smem_timing_cali`, `smem_ac`, `psram_dqs_0`.
Decoded, and this closes the bootloader hypothesis:

  - `psram_sys_clk_en`, `psram_pll_clk_en`, `psram_core_clk_en` all set, source
    MPLL, core divider 1.  Flash runs from SPLL with core divider 6.
  - `hp_rst_en0` bit 8 is `RST_EN_CORE1_GLOBAL`, its reset default.  No MSPI
    reset is held.
  - `smem_ac` `0x8000b084` is the reset default bit for bit: setup time 1, hold
    time 1, ECC hold 3, skip page corner set, split transactions set.
  - `psram_dqs_0` is zero, so the strobe arrives disabled, as expected.

So the bootloader configures the flash half and leaves the PSRAM half at its
defaults.  It is not handing this port a controller in a state that explains
anything.

**The CPU clock is not the cause.**  360 MHz was the one variable that changed
between the last working PSRAM and the first silent one, and raising the clock
draws visibly more current, so it was worth one build.  With the clock left
where the bootloader put it - `cpu /4`, 90 MHz, no `[clock] set to` line at all
- PSRAM fails identically: same MPLL state, same `0x1f`/`0xff`, same lost
round-trip.

- Acceptance points passed: the comparison is complete and reproducible; the
  connected check and the handover state are now measured rather than assumed.
- Acceptance points failed: PSRAM is still not available, so B5 and the
  reference display path remain blocked.
- Remaining risk: the fault is now outside everything this port writes and
  outside the state a power cycle clears.  Four hypotheses are eliminated -
  wrong constant, wrong order, bootloader handover, CPU clock - and no software
  hypothesis remains that I can name.
- Next safe step: write the backed-up flash image at
  `/Volumes/Dev/d1001-backup/d1001-e8f60ae0464c-flash-32MB.bin` back and see
  whether the reference firmware still brings PSRAM up on this board *now*.
  It did on 23 August, but that was before the battery was disconnected and
  before the bootloader was rebuilt, so it is no longer a current measurement.
  If the reference also fails, the fault is the board and not this port, and
  that is the one question worth answering next.  It needs explicit
  authorisation: it overwrites the bootloader, the partition table and both
  app slots.

### 2026-08-23 - PSRAM restored: the reference firmware brings the chip back, and it stays back

- State change: PSRAM works again.  32 MB mapped at `0x48000000`, verified, in
  exec's memory list, `AvailMem` 34,080,304 bytes, the BSP package loading all
  fourteen modules into it and the boot reaching the shell.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, battery attached.
- Procedure: the full 32 MB backup at
  `/Volumes/Dev/d1001-backup/d1001-e8f60ae0464c-flash-32MB.bin`, sha256
  `8923f1e0...92e44` confirmed before writing, written to `0x0` and
  esptool-hash verified.  Booted the reference firmware once.  Then AROS
  restored: bootloader to `0x2000`, partition table to `0x8000`, `otadata`
  erased at `0x10000`, core to `0x20000`, BSP package to `0x820000`, flash
  volume to `0xc00000`, each esptool-hash verified.

**The reference firmware works on this board, now.**  That was the question, and
it settles the one that mattered:

```text
I (401) hex_psram: vendor id    : 0x0d (AP)
I (402) hex_psram: density      : 0x07 (256 Mbit)
I (403) hex_psram: BitMode      : 0x01 (X16 Mode)
I (403) hex_psram: Readlatency  : 0x04 (14 cycles@Fixed)
I esp_psram: Found 32MB PSRAM device
I esp_psram: Speed: 200MHz
I (1112) esp_psram: SPI SRAM memory test OK
```

`0x0d` is the vendor byte this port asks for and had been reading as `0x1f` for
the whole investigation.  So the silicon is sound, the solder is sound, and the
fault was never the board.

**And after that, this port works too.**  AROS restored on top, three resets in
a row, PSRAM up every time, and then a fourth boot with the full package
reaching the shell.  Nothing in the port changed between the failing runs and
these: same core, same sequence, same constants.

The conclusion this forces is narrow and worth stating plainly.  **This port's
PSRAM bring-up is not self-starting.**  It works from the state the reference
firmware leaves and it could not work from the state it was in before.  The mode
registers survive a CPU reset - this file has said so since the retry loop was
written - and the reference leaves them at read latency 4, write latency 1, X16,
2048-byte bursts, which is its 200 MHz set.  What the chip held before is not
known, because the only way to read it is a transaction that needs the timing to
be right already.  That is the shape of the fault: a chip state this port cannot
address, and cannot leave, because leaving it needs the state it cannot reach.

- Acceptance points passed: PSRAM up, mapped, verified, carrying the module
  package; the boot completes; three consecutive resets hold.
- Acceptance points failed: the bring-up's robustness.  It depends on the chip
  arriving in a compatible state, and it has no way to recover from one it is
  not.
- Remaining risk: this can happen again after any firmware that configures the
  chip differently, and the retry loop does not help because all three attempts
  use the same dummy lengths.  The fix is to sweep the latency sets rather than
  assume one: try the identity read with the slow pair and the fast pair before
  concluding the chip is absent.  That is a bounded change and it does not need
  the cause to be proven first.
- Next safe step: the reorder from the panel bring-up commit has still never run
  against DSI hardware.  With PSRAM back, B3 stage two and B4 can be re-run, and
  the reference display path over a real framebuffer becomes buildable.

### 2026-08-23 - the MSPI PLL does not calibrate from cold, and it is not this port's code

- State change: the failure is located.  It is not PSRAM and not this port's
  PSRAM sequence.  The MSPI PLL's calibration never starts on a cold-booted
  board, and ESP-IDF's own implementation of that calibration, run in the
  bootloader, does not start it either.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, cold-started by
  disconnecting battery and USB.
- Artifacts: core 165,168 bytes and bootloader 22,928 bytes, both
  esptool-hash verified.  New bootloader component
  `bootloader/project/bootloader_components/aros_mpll/`.

**What was measured.**  `ANA_PLL_CTRL0` traced through the sequence, from a
board cold-started so nothing was inherited:

```text
[psram]  trace  0x0000024c 0x0000004c 0x0000004c 0x0000004c spins 1000000
```

Entry, after clearing `MSPI_CAL_STOP`, after the divider write, after the wait.
The stop bit is cleared exactly as ESP-IDF clears it, every analogue register
reads back the value written - divider byte `0x99`, which is what
`(400/20-1)<<3 | 1` gives - and `MSPI_CAL_END` is absent after a million polls.
Decoded, `0x4c` also says `CPU_PLL_CAL_END` and `SYS_PLL_CAL_END` are both set:
the calibration machinery works, for the two PLLs the bootloader brings up, and
not for this one.  The calibration is not failing.  It never starts.

**What was eliminated, each by a run on hardware.**

| hypothesis | result |
| :--- | :--- |
| digital supply too low | Cold start reads `PMU_HP_ACTIVE_DCM_VSET` = 27, above the 26 the vendor firmware sets.  Not it - and forcing the reset default of 20 hung the board so hard it stopped answering USB, which is how the supply step was learned to belong before the CPU clock rather than in the PSRAM bring-up |
| CPU clock at 360 MHz | Fails identically with the clock left at the bootloader's `cpu /4` |
| PLL not powered | Powering it down and back up before calibrating changes nothing |
| no settling time | A millisecond after powering it changes nothing |
| analogue master on 160 MHz | This port selected `I2C_ANA_MST_CLK160M`, which IDF never does for the P4.  Removing it changes nothing |
| a missing register or bit | Every one verified against IDF: `PMU_RF_PWC` +0x15c bit 24, `LP_CLKRST_HP_CLK_CTRL` +0x40 bit 28, `CAL_END` bit 8, `CAL_STOP` bit 9, all four regi2c registers and their shifts |
| ESP-IDF does it differently | It does not.  A bootloader component was written that calls IDF's own `clk_ll_mpll_enable`, `regi2c_ctrl_ll_mpll_calibration_start`, `clk_ll_mpll_set_config` and waits on `regi2c_ctrl_ll_mpll_calibration_is_done`.  With IDF's unbounded wait it hung the bootloader; with a bounded one it logs `mspi pll calibration did not complete`.  IDF's own code, on this silicon, from cold |
| the bootloader could do it via `CONFIG_SPIRAM` | It cannot.  The IDF bootloader has no MPLL code at all - `grep` over `bootloader_support` finds nothing |
| the ROM could do it | No MPLL or regi2c entry point exists in the P4 ROM symbol table |

**So what does the vendor firmware do?**  It calibrates successfully, every
time, and it is an ordinary IDF application.  Between the bootloader and
`esp_psram_impl_enable` it runs `pmu_init`, `rtc_init` and the rest of the
system startup, none of which this port or the bootloader performs.  The
calibration therefore has a precondition established somewhere in that startup.
Finding which one is the next question, and it is now a well-posed one rather
than a search.

**Fixed on the way, and worth keeping regardless.**

  - The digital supply is raised to 26 before the CPU clock, in cpuclock.c.
    Not the cause here, but the ordering is right and the failure it prevents
    is a hard hang.
  - The mode registers are written before anything is read, then the identity
    is read, and only if that fails are all eight read latencies swept.  The
    first version swept before writing and hung the boot dead on a
    cold-started chip: the part is in its power-on width, and a read at the
    wrong width does not fail, it does not return.  A write needs no dummy
    cycles and is the one transaction safe to send into an unknown state.
  - A settling wait after mode register 8, which selects that width.
  - A missing `MSPI_CAL_END` stays fatal.  Letting the bring-up continue past
    it was tried and hung at the first controller register: the bit is a real
    signal, the PLL does not run uncalibrated.

- Acceptance points passed: the fault is isolated to one analogue step, with
  the port's own code eliminated as the cause by running IDF's implementation
  of that step under IDF-like conditions.
- Acceptance points failed: PSRAM is unavailable from a cold boot.  Everything
  needing a framebuffer stays blocked.
- Remaining risk: the port currently requires the chip to have been calibrated
  by other firmware, which is not a state anyone installing AROS can be asked
  to produce.  This is a release blocker, not a diagnostic curiosity.
- Next safe step: bisect the IDF application startup for the precondition.
  `pmu_init` and `rtc_init` are the candidates, both are self-contained, and
  both can be called from the bootloader component that now exists - which
  makes the next experiment a small addition to a file already in the tree
  rather than new scaffolding.

### 2026-08-23 - pmu_init tested in the bootloader: not the precondition, and the bootloader is the wrong place

- State change: none to the port.  Two candidates eliminated and one place
  ruled out.  The bootloader component written for the experiment has been
  removed again; the bootloader is byte-identical to the one before it.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**rtc_init does not exist for this chip.**  There is no `rtc_init.c` in
`esp_hw_support/port/esp32p4`.  What the P4 has is `rtc_clk_init.c`, and the
bootloader already calls into it before any hook of ours runs, so there was
nothing to add there.

**pmu_init changes nothing.**  It was the strongest remaining candidate: it
writes the full high-power and low-power parameter sets and forces the
power-domain defaults, which is the kind of analogue groundwork a PLL
calibration might wait on, and IDF's P4 port compiles `pmu_init.c` for
bootloader builds so it could be called for real rather than approximated.
Called from `bootloader_after_init` immediately before IDF's own calibration
sequence, `MSPI_CAL_END` still never appeared.

Two mechanical obstacles were cleared on the way and both are worth recording,
because each cost a diagnosis:

  - `pmu_init.c` carries a `.spm.text` section the bootloader link script has
    no place for, and the bootloader links `--orphan-handling=error`.
  - Its PVT branch calls into `pmu_pvt.c`, which IDF excludes from bootloader
    builds.  Compiling that source into our own component needs IDF-internal
    include paths, so the test ran with `CONFIG_ESP_ENABLE_PVT=n`.  PVT is
    therefore *not* eliminated - it is the one part of `pmu_init` that did not
    run.

**The bootloader is the wrong place for this.**  Two separate failures, neither
about the calibration itself:

  - A 2,000,000-poll bounded wait ran long enough for the bootloader watchdog
    to reset the board.  A completing calibration takes microseconds; the bound
    has to be small.
  - Even with a short bound and `pmu_init` removed, touching the MSPI PLL from
    a bootloader hook produced repeated `SW_SYS_RESET`.  The bootloader executes
    from flash through the same MSPI block, so disturbing that clock tree is
    disturbing the code that is running.

That is a real constraint on where this can be fixed, and it points back at the
kernel: the kernel runs its PSRAM bring-up from SRAM precisely so it may reset
and reconfigure MSPI, and it is the only context on this board that can.

**Also found and fixed:** the partition table at `0x8000` had been corrupted
during the session's flashing (`partition 0 invalid magic number 0xb7c5`),
which showed as a `SW_SYS_RESET` loop indistinguishable from the hook's.
Rewritten and verified.

- Remaining risk: unchanged.  PSRAM is unavailable from a cold boot and that
  is a release blocker.
- Next safe step: two things are now known to be untried rather than merely
  unknown.  PVT, which is the part of `pmu_init` that did not run and which
  adaptively sets the digital supply - it can be called from the kernel, where
  there is no bootloader link script to fight.  And a bootloader built with
  `CONFIG_SPIRAM=y`, which is how the vendor's bootloader is configured: it
  contains no MPLL code, but it may change cache or clock configuration in ways
  that matter, and that has not been tested rather than ruled out.

### 2026-08-23 - PVT's calibration bits tried in the kernel: still no MSPI_CAL_END

- State change: none to the port beyond one kept correctness fix.  Three more
  candidates eliminated.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**What was tried.**  ESP-IDF's PVT path is where the last untested part of
`pmu_init` lives, and three of its writes looked like they could gate another
analogue calibration in the same domain:

  - `PVT_SYS_CLK_EN` in `SOC_CLK_CTRL1`, which IDF enables before any PVT work
  - `PMU_DIG_DBIAS_INIT` in `PMU_HP_ACTIVE_HP_REGULATOR0`, which IDF's own
    comment describes as starting a calibration
  - a settling wait after both

Set in the kernel immediately before the MSPI PLL calibration, from a cold
boot: `MSPI_CAL_END` still absent, trace unchanged at `0x24c 0x4c 0x4c 0x4c`.
The writes have been removed again; the register definitions are kept because
a full PVT implementation will need them.

**A reset pulse with no width was also ruled out.**  The two regi2c writes that
lower and raise `IR_CAL_RSTB` go back to back in IDF, and a pulse too short for
the state machine to observe would look exactly like a calibration that never
starts.  A wait between them changes nothing.  The wait is kept anyway: a reset
that is a pulse should have a width, and each regi2c transaction is already
microseconds of bus traffic, so it costs nothing.  Kept as correctness, not as
a fix.

**And the measurement is sound, which was worth confirming.**
`krnPSRAMMPLLState` reads all three analogue registers back over the
configuration bus rather than reporting what was written.  So `rstb 0x31` with
the reset released, `div 0x99` for 400 MHz and `dhref 0x70` are real read-backs:
the bus works, the values are in the PLL, and the calibration state machine
does not run.

- Remaining risk: unchanged, and it is a release blocker.
- Next safe step: two options, and they differ in kind rather than in size.
  Write PVT out in full - some forty registers plus two eFuse fields, with
  IDF's `pmu_pvt.c` as an exact template; mechanical, and it either fixes the
  calibration or eliminates PVT completely.  Or compare against the working
  case directly: add a register dump to the vendor firmware immediately before
  its `esp_psram_init`, which needs a change to that firmware and is therefore
  the owner's call, and read what state it has that this port does not.  The
  second answers the question rather than guessing at it, which after this many
  eliminations is worth more than another candidate.

### 2026-08-23 - the MSPI PLL needs the analogue I2C block out of reset, and that bit was never set

- State change: PSRAM comes up, at 200 MHz, with the tuning succeeding and the
  identity answering on the first attempt.  The missing piece is one bit.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Method: a read-only register dump added to the vendor firmware on a test
  branch (`test/mpll-state-dump` in that tree), printing the eleven registers
  involved at the top of `app_main` - after its calibration has succeeded -
  for comparison against the same set after this port's has failed.

**The difference was one register.**

```text
                     vendor firmware   this port
PMU_RF_PWC           0x0d000000        bit 24 only
```

`0x0d000000` is bits 24, 26 and 27.  Bit 24 is `MSPI_PHY_XPD`, which this port
sets.  Bit 26 is **`PERIF_I2C_RSTB`** and bit 27 is `XPD_PERIF_I2C`: the reset
and the power of the analogue peripheral I2C block.  `PERIF_I2C_RSTB` defaults
to zero, meaning reset asserted, so a cold-booted chip holds that block in
reset - and the PLL calibration state machine runs over it.

This explains every measurement taken over the whole investigation:

  - The regi2c reads and writes worked throughout, returning correct values,
    because the register interface is not what the reset holds down.  Every
    diagnostic said the bus was fine, and it was.
  - `CPU_PLL_CAL_END` and `SYS_PLL_CAL_END` were set while `MSPI_CAL_END` was
    not, because those two were calibrated before this port ran.
  - The calibration never *started* rather than failing, which is what a held
    reset looks like.
  - `PMU_RF_PWC` survives a CPU reset, so a boot after the vendor firmware
    inherited the released reset and worked, and a boot from cold did not, with
    an unchanged binary.  That is the regression that opened this whole line.
  - And reading ESP-IDF's P4 sources could not find it: IDF releases these two
    bits in `bootloader_soc.c` for the C5 and the C61, and nowhere for the P4.

**Also confirmed by the same run:** with the PLL calibrating, the 200 MHz path
works end to end.  `[psram] chip 32 MB at 200 MHz, vendor 0x0000000d, a word
written and read back after 1 attempt`, phase window 4 at 67.5 degrees, a
29-wide delay window, and the module package loading into PSRAM afterwards.

- Acceptance points passed: PSRAM up at the target clock; the cause identified
  by measurement rather than elimination; the fix is one register write with a
  documented reason.
- Acceptance points failed: none yet, but the proof is incomplete - see below.
- Remaining risk: this run followed the vendor firmware, so the calibration
  could have been inherited rather than performed.  The kernel now prints
  `[psram] calib entry <ANA_PLL_CTRL0> inherited|done here` from the value read
  before it clears `MSPI_CAL_STOP`, which distinguishes the two, and a cold
  boot is needed to read it under the condition that matters.
- Next safe step: cold-start the board and confirm `done here`.  If it says
  that, the release blocker is closed and B3/B4's panel reorder can finally be
  run against DSI hardware.

### 2026-08-23 - proved without opening the case: the port calibrates the PLL itself

- State change: the release blocker is closed.  PSRAM comes up at 200 MHz from
  a state where the calibration has not been done, on every boot, and the
  proof no longer needs a battery disconnected behind a screwed-down panel.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The reset is pulsed, not just released.**  `PERIF_I2C_RSTB` survives a CPU
reset, so a boot after firmware that already released it inherits a working
block and demonstrates nothing - the failure only appears when the block
arrives held down.  Asserting the reset first and then releasing it puts the
block back into the state a cold boot leaves it in, so every boot exercises the
path that was broken.  That also removes the need to reach the battery to test
it, which on this board means removing the display.

**And it is measured, not assumed.**  The kernel prints `ANA_PLL_CTRL0` as
found, before it clears `MSPI_CAL_STOP`:

```text
[psram]  chip   32 MB at 200 MHz, vendor 0x0000000d, a word written and read back after 1 attempt
[psram]  calib  entry 0x0000024c done here
[psram]  found  read latency 4, set to 4
[psram]  tuning phases  1111  window 4, chose 67.5 degrees
```

`0x24c` has bit 8 clear: no calibration had been done when this boot started.
PSRAM then comes up anyway, so this port performed it.  Three consecutive boots,
identical.

**A tool, because the measurement kept being the hard part.**
`tools/reset-and-log.py` holds the serial port open and asserts the reset over
the control lines, so capture starts before the ROM's first line.  esptool has
to own the port to reset the board, and by the time it releases it the early
output is gone - on a boot that loads the module package the log is 60 KB
against a much smaller USB CDC buffer.  Several findings this session were
measured twice for that reason, and one dump had to be relocated into a later
report just to be readable.  It is not a diagnostic of the port; it is the
thing that should have existed on day one.

- Acceptance points passed: PSRAM at the target clock from an uncalibrated
  start, reproducibly, with the origin of the calibration printed rather than
  inferred.
- Acceptance points failed: none.
- Remaining risk: none outstanding on PSRAM.  The vendor firmware's test branch
  still carries the diagnostic dump that found this and should be cleaned up
  when its owner is ready.
- Next safe step: B3 stage two and B4 against DSI hardware.  The panel bring-up
  reorder from the 23 August restore commit - supply, PHY lock, command mode,
  then the panel's reset pulse - has never run, because PSRAM blocked it, and
  the reference display path over a real framebuffer is now buildable.

### 2026-08-23 - B3/B4 with the reorder: the backlight lights, and the pixel path is located

- State change: the panel's backlight comes on, which it never did before, and
  the question of where pixels come from is settled by measurement.  The image
  is still dark.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_PATTERN_TEST=1`, and
  `P4_PATTERN_BRIDGE_FEED=1` for the two measurements below.  Core 174,128
  bytes.

**The reorder ran, for the first time.**  PSRAM had blocked it since it was
written.

```text
[dsi]    phy status 0x000015bd  locked, lanes in stop state
[dsi]    panel out of reset, with the lanes already in stop state
[dsi]    the jd9365 sequence completed, display on, backlight still dark
```

The panel now leaves reset with the lanes already in LP-11, which is what the
D-PHY specification asks for and what the previous order got backwards.

**The backlight works.**  Reported by the board's owner: lit, where every
earlier B4 run left it dark and unlit.  Three real defects were fixed to get
there over the previous sessions - the IOMUX input enable, DC instead of PWM,
and the LEDC timer's own commit bit - and this is the run where that path
finally shows.

**The DCS read path is still silent.**  So the reorder was not its cause.  It
stays as B3's open defect, now with one more hypothesis eliminated.

**The pixel path runs through the bridge, not through the host's generator.**
This was the open architecture question and it is now measured.  With the
bridge's pixel feed enabled:

| configuration | host int1 | bridge |
| :--- | :--- | :--- |
| feed off, generator on | `0x00000000` | no underrun |
| feed on, generator on | `0x00000080` | no underrun |
| feed on, generator off | `0x00000080` | no underrun |

`int1` bit 7 is `DPI_PLD_WR_ERR`.  The bridge delivers pixels the host cannot
take, and it does so whether or not the generator is running - so the error is
not a collision between the two.  And the bridge never underruns, which means
it is not starved either.  What that leaves is a bridge configured too thinly:
this port writes its timing, pixel type and flow control and none of the pixel
count, underrun-discard count, burst length, empty threshold or multi-block
settings the reference sets.

That also settles B4's premise.  The host's own pattern generator is a
DesignWare feature the vendor reference never uses on this SoC, there is no
counter that says whether it emits anything, and the panel stays dark with it
running.  An internal test pattern is not the cheap first image it was planned
as; the framebuffer of B5 is the shorter path.

**Verified identical to the reference, so not causes:** two data lanes, 1000
Mbit/s per lane, the Set A timings (hsync 20, hbp 20, hfp 40, vsync 4, vbp 30,
vfp 30, 40 MHz), RGB565 on both sides of the bridge, and 16-bit colour coding
configuration 1.

- Acceptance points passed: the reorder holds, the PHY locks, the panel is lit,
  and the pixel path is identified.
- Acceptance points failed: no image.  B4's own gate - a stable internal test
  pattern - is not reachable the way the phase assumed.
- Remaining risk: the DCS read path remains unexplained, and without it there
  is still no panel-side acknowledgement of anything sent.
- Next safe step: B5, and it is now the shorter route to a first image rather
  than the next phase after one.  A framebuffer in PSRAM, DW-GDMA feeding the
  bridge, and the bridge configured as the reference configures it.  PSRAM is
  available and calibrated, which is what blocked this until today.

### 2026-08-23 - the PSRAM calibration measured properly: 30 of 30, one attempt each

- State change: the calibration is retried rather than merely waited for, and
  its reliability is now measured on a sample that can support a claim.  Two of
  my own statements from earlier today were wrong and are corrected here.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**First correction: six successes proved nothing.**  Earlier today I reported
six consecutive boots as confirmation that a settling wait had fixed a
one-in-four failure.  It is not confirmation of anything: a one-in-four failure
rate produces six clean boots about eighteen per cent of the time.  Reasoning
from that sample was a mistake, and it is the kind that makes a marginal
bring-up look finished.

**Second correction: most of those failures were my instrument.**  The capture
tool's reset landed in the ROM's download stub on roughly half of all attempts
- `rst:0x17, boot:0x207 (DOWNLOAD)` - and a boot that never happened looks
exactly like a boot that produced no diagnostic.  So the "three boots in four"
figure was largely a measurement artefact.  One genuine failure was seen, in
the first B3/B4 run, and one is not a rate.

The tool now reads enough of the ROM banner to see which way the board went and
retries past the stub.  Getting the line states right took three attempts and
they fail in both directions: driving DTR high stops the reset taking at all,
toggling it as part of the pulse lands in the stub, and even with esptool's own
sequence the ROM sometimes latches IO0 anyway - so retrying is what makes it
reliable, not the sequence.

**The calibration is now retried, which is the right structure regardless.**
`krnPSRAMMPLLUp` splits into a single-attempt `p4_mpll_calibrate` and a loop of
up to four, each attempt asserting the analogue block's reset afresh and
power-cycling the PLL - the same work a cold boot does.  A longer wait can only
move a failure rate; another attempt removes the failure.  The number of
attempts used is printed, so a chip that starts needing three is visible rather
than quietly marginal.

**The measurement.**  Thirty consecutive boots:

```text
  30 done here, 1 attempt
```

`done here` from `ANA_PLL_CTRL0` read before `MSPI_CAL_STOP` is cleared, so
every one of those thirty calibrated the PLL itself rather than inheriting it.
And every one needed a single attempt, so the retry is insurance and not a
crutch.  Against a hypothetical one-in-four failure rate, thirty clean boots
have a probability of about 0.018 per cent.

- Acceptance points passed: the calibration is reliable on a sample that
  supports saying so, and the failure path is bounded rather than absent.
- Remaining risk: whether the settling wait is doing any work is not
  established - the retry would cover its absence, and no failure has been seen
  since to distinguish them.  Recorded as unknown rather than resolved.
- Next safe step: unchanged, B5.

### 2026-08-23 - B5 first light on the data path: the DMA moves a frame, then stalls

- State change: pixel data crosses from PSRAM towards the panel for the first
  time in this port.  8,704 bytes of it, and then the transfer stops.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_SCANOUT_TEST=1`.  Core 175,328 bytes.
- New: `kernel/dsi_scanout.c`, the bridge's full configuration, a DesignWare
  AXI DMA channel and one link-list item; `krnP4CacheWriteback`.

**What was built.**  The reference's path, with its values: a 2,048,000-byte
RGB565 frame at the base of the PSRAM window, the bridge as a puller rather
than a pusher, and one DMA link-list item carrying the whole frame - which is
not a simplification, the reference's own comment says it assumes exactly that.
The item's next-pointer points at itself, so the transfer repeats without an
interrupt to restart it; the reference marks its item last and restarts from a
transfer-done callback, which this port has no handler for yet.

**Three defects found, each by measurement.**

  - *No module clock.*  `GDMA_CPU_CLK_EN`, `GDMA_SYS_CLK_EN` and the GDMA
    system reset live outside the DMA's own register block.  Without them the
    block still answered reads, still cleared its soft reset and still accepted
    a channel enable - and never fetched a descriptor.  Everything readable
    looked configured; only the engine was not running.
  - *The pattern generator was still on.*  `krnP4DsiPatternOn` enables it
    unless the bridge-feed switch is set, and B5 does not set that switch, so
    both sources were driving the link.  Now excluded for the scanout path too.
  - *The descriptor sat in cache.*  This is the one that mattered: the DMA
    reads the link-list item over AXI and does not see the CPU's caches, so an
    item in a dirty line is an item of zeroes to the engine.  It reads back as
    a channel that is enabled and never starts, which is exactly what the first
    two runs showed - `sar` stuck at zero.  With a writeback before the channel
    is pointed at it, `sar` reads `0x48002200`: the engine loaded the
    descriptor and walked 8,704 bytes into the frame.

**Where it stops.**

```text
[b5]     dma  cfg1 0x0a020001 llp 0x4ff02680 sar 0x48002200
[b5]     brg  flow 0x00000010 rawnum 0x0003e800 misc 0x00003201 int 0x00000001
[b5]     host pkt 0x00040055 int0 0x00000000 int1 0x00000080  DPI_PLD_WR_ERR
[b5]     sar 0x48002200 then 0x48002200  stalled
```

The bridge is configured as intended - flow controller DMA, 256,000
sixty-four-bit words, DPI enabled, 800 as the discard count - and it now
reports `INT_RAW` bit 0, an underrun, which it never did before.  The host
still reports `DPI_PLD_WR_ERR`.  So the bridge ran dry while the host was
refusing payload, and the DMA stopped with the frame one two-hundredth
transferred.

Read together those three say the host is not draining what the bridge hands
it, the bridge therefore empties, and the DMA's hardware handshake goes quiet
because nothing is asking.  The underrun-discard count exists to recover from
exactly that and does not, which points at the host side rather than the feed.

**One known deviation left on the host side.**  The reference enables frame
acknowledge - `mipi_dsi_host_ll_dpi_enable_frame_ack(host, true)` - and this
port does not.  Everything else in the video-mode configuration matches:
burst with sync pulses, packet size 800, no chunking, no null packets, the Set
A timings.  Frame acknowledge asks the panel to answer each frame, which is
uncomfortable given that no read from this panel has ever answered, but it is
the remaining difference and it is cheap to try.

- Acceptance points passed: none of B5's yet.  The data path exists and moves.
- Acceptance points failed: no sustained scanout, so no image.
- Remaining risk: the host-side stall is unexplained, and the silent read path
  makes frame acknowledge a test that could hang rather than fail.
- Next safe step: try frame acknowledge, and if that does not free the host,
  read the host's own FIFO and timing registers back rather than assuming the
  write took - the same discipline that found the descriptor in cache.

### 2026-08-23 - B5: the host will not take pixels, and the backlight follows the video stream

- State change: two host-side deviations from the reference corrected, one of
  them a real bug in this port.  The stall is unchanged.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The backlight tracks the video stream, which settles an old question.**
Reported by the board's owner: dark, with no backlight, during the scanout run.
The same call in the same place lit it during the B4 pattern run, and the
electrical state is identical either way - `latch 0x0000f7ff`, and a measured
18 per cent PWM, `duty 3681/20000`.  So the panel is not being lit by the
backlight circuit alone; it lights when it receives a valid video stream and
does not when the stream stalls.  That is the hypothesis raised during B4's
isolation test, and it is now supported rather than speculative.  It also means
the backlight is a symptom in this phase, not a subsystem to debug.

**A real bug: frame acknowledge was written to the wrong bit.**
`FRAME_BTA_ACK_EN` is bit 14 of `VID_MODE_CFG`.  The first attempt used bit 11,
which is `LP_VACT_EN` - so it enabled a low-power transition while believing it
enabled an acknowledge.  Found by checking the position against the register
header instead of trusting the first reading of it.

**A wrong decision: all low-power transitions were disabled.**  B4 cleared
every one of them deliberately, to keep the number of moving parts down while
diagnosing.  The reference sets all of them - its `disable_lp` flag is left
false for this panel - so that traded a configuration known to work on this
hardware for one that does not.  The transitions are what give the host
somewhere to go between lines; without them it carries high-speed continuously.
All eight bits now match the reference on the scanout path.

**And it changes nothing that matters.**  `CMD_PKT_STATUS` moves from
`0x00040055` to `0x00060054`, so the bits are reaching the host, but:

```text
[b5]     dma  llp 0x4ff02680 sar 0x49e0e200
[b5]     brg  flow 0x00000010 rawnum 0x0003e800 misc 0x00003201 int 0x00000001
[b5]     host pkt 0x00060054 int1 0x00000080  DPI_PLD_WR_ERR
[b5]     sar 0x49e0e200 then 0x49e0e200  stalled
```

The transfer stops after exactly `0x2200` bytes every time - 1,088 sixty-four
bit words, which is suspiciously close to a 1,024-entry FIFO plus one burst.
So the DMA fills the bridge, the bridge cannot hand on, the host reports a
payload write error, and everything stops.  The frame's own position is no
longer a factor: it was moved to the top of the PSRAM window because the base
is where the module package is loaded, and the stall is identical.

- Acceptance points passed: none of B5's.
- Acceptance points failed: no sustained scanout.
- Remaining risk: the host's refusal is unexplained and now has no candidate
  left from comparing configuration values - every one of them matches.
- Next safe step: stop comparing what was written and read back what the host
  actually holds.  `MODE_CFG` first: this port clears `CMD_VIDEO_MODE` to enter
  video mode and has never verified that it took.  A host still in command mode
  has no video path at all, which is exactly what a payload write error into a
  full FIFO would look like.  That is the same discipline that found the
  descriptor sitting in cache, and it is the only kind of step left.

### 2026-08-23 - B5: the host is configured correctly and its data lanes never leave stop state

- State change: three more deviations from the reference corrected, one of them
  a real bug.  The host's refusal is now precisely characterised and no longer
  a matter of comparing written values.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**Read back rather than assumed, which is what this step was for.**

```text
[b5]     host mode 0x00000000  video mode, pwr_up 0x00000001, lpclk 0x00000003 hs
[b5]     host vid_mode 0x0000bf02 phy 0x000015b9 colour 0x00000000
[b5]     host pkt 800 hsa 63 hbp 63 hline 2750 vact 1280
```

Every write took.  The host is in video mode, powered, the clock lane under
automatic control, packet size 800 pixels, the scaled horizontal timing
consistent - 63 + 63 + 2499 + 125 = 2750 - vertical active 1280, colour coding
RGB565 configuration 1.  `MODE_CFG` was the suspicion and it is clean.

**And `PHY_STATUS` says why nothing arrives.**  `0x15b9` decodes to: PLL
locked, direction transmit, clock lane *out* of stop state, and
`STOPSTATE0LANE` and `STOPSTATE1LANE` both set - **both data lanes are in stop
state**.  The host is not transmitting at all, while its payload FIFO overflows
from the bridge.  That is a much sharper statement than "the panel is dark",
and it is where the next step starts.

**Three corrections on the way.**

  - *A real bug.*  `FRAME_BTA_ACK_EN` is bit 14 of `VID_MODE_CFG`; the first
    attempt wrote bit 11, which is `LP_VACT_EN`.  So it enabled a low-power
    transition while believing it enabled an acknowledge.
  - *A wrong decision reversed.*  B4 had cleared every low-power transition to
    reduce moving parts while diagnosing.  The reference enables all of them
    for this panel, so that traded a working configuration for a guess.
  - *Half a register.*  The reference's "automatic" clock-lane state is two
    bits, `auto_clklane_ctrl` as well as `phy_txrequestclkhs`.  This port set
    only the second, pinning the lane in high speed permanently instead of
    letting the host manage it.  `lpclk` now reads `0x3`.

None of the three frees the host, and frame acknowledge was tried and then
switched back off: with it set the host waits for the panel to answer every
frame, and nothing this port has read from this panel has ever answered.

- Acceptance points passed: none of B5's.
- Acceptance points failed: no scanout.  The panel's backlight follows the
  video stream, so it stays dark as well.
- Remaining risk: the two open defects are now visibly the same shape - the
  panel never answers a read, and the host never drives the data lanes.  Both
  are the link failing to go high-speed in the direction it is asked to.
- Next safe step: read back the bridge's own DPI timing registers.  The host
  waits for VSYNC from the bridge before it transmits, the bridge generates it
  from `DPI_V_CFG0/1` and `DPI_H_CFG0/1`, and those are written by
  `krnP4DsiPatternOn` in pixel units and have never been verified.  A bridge
  that emits no sync is a host that never starts, which fits everything
  measured.

### 2026-08-23 - B5: every register verified, and the data lanes still will not go high-speed

- State change: none functionally.  The configuration is now verified end to
  end by read-back rather than by comparison, and it is correct.  That is worth
  recording because it removes a whole class of cause.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The bridge's DPI timing, read back and decoded:**

```text
[b5]     brg  v 0x05000540/0x0004001e h 0x03200370/0x00140014 en 0x00000001 pix 0x00000000
```

`VDISP` 1280, `VTOTAL` 1344, `VSYNC` 4, `VBANK` 30, `HDISP` 800, `HTOTAL` 880,
`HSYNC` 20, `HBANK` 20, `dsi_en` set, pixel type RGB565.  Every value is what
the reference computes from the same panel parameters, in the right field.

**What has now been verified rather than assumed, all of it correct:** the
host's mode, power, clock-lane control, video-mode flags, packet size,
horizontal and vertical timing and colour coding; the bridge's DPI timing,
enable, pixel type, flow controller, raw count, discard count, burst length and
empty threshold; the DMA's channel configuration, descriptor and running source
address; two data lanes at 1000 Mbit/s, `PHY_ENABLECLK`, `PHY_FORCEPLL`,
`auto_clklane_ctrl` with `txrequestclkhs`.  The PLL locks and the clock lane
leaves stop state.

**And `PHY_STATUS` still reads `0x15b9`:** both data lanes in stop state, a
host that never transmits, a payload FIFO that overflows.

So the fault is not a register value.  Comparing this port's configuration
against the reference has been exhausted twice now - once for PSRAM, where the
answer turned out to be one bit outside the sequence being compared, and now
here.

**The two open defects have the same shape.**  No read from this panel has ever
answered, and the host will not drive the data lanes.  Both are the link
refusing to enter high-speed in the direction asked for.  Treating them as one
problem rather than two is a change of view, not a finding, but it is the first
framing that accounts for both.

- Acceptance points passed: none of B5's.
- Remaining risk: unchanged, and now without a configuration candidate.
- Next safe step: the four PHY lane-transition times in `PHY_TMR_CFG` and
  `PHY_TMR_LPCLK_CFG` - 50, 104, 46 and 128.  They were carried over from
  ESP-IDF with no derivation available and are recorded as unresolved in the
  display contract.  They are exactly what decides whether a lane can leave
  low-power for high-speed, they are the last values in the data path this port
  cannot justify, and a wrong one would present as a lane that stays in stop
  state and a read that never answers - which is both symptoms at once.

### 2026-08-23 - the PHY PLL reference is 20 MHz, not the crystal, and B3's evidence was not evidence

- State change: a wrong assumption carried since B3 is identified and corrected.
  It does not fix the stall, and it changes the PHY status, so it is recorded
  with both halves.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The finding.**  On ESP32-P4 before revision 3.0 the PHY PLL reference has no
source select - it is fixed - and ESP-IDF's own name says what it is fixed to:
`MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY` is `PLL_F20M`.  Selecting the
crystal became possible only on revision 3.0.  So the reference is **20 MHz**,
not the 40 MHz crystal.

**And B3's reasoning was circular.**  B3 assumed 40 MHz, derived N=2 and M=50
for 1000 Mbit/s from it, saw the PLL lock, and recorded the lock as evidence
that the assumption held.  It is not evidence: 20 MHz with N=2 and M=50 is 500
Mbit/s, which locks just as well.  A PLL that locks says the loop closed, not
that it closed on the intended frequency.  The roadmap entry for B3 stage one
should be read with that in mind.

**What was therefore wrong since B3.**  The lanes ran at half the intended
rate, while the PHY was told its range was 1000 to 1049 Mbit/s
(`hs_freq_sel 0x2A`) and the host's horizontal timing was scaled for a 125 MHz
byte clock that was really 62.5.  That is one mistake accounting for both open
defects - lanes that will not enter high speed and reads that never answer -
where no single register value did.

**The correction, and what it changed.**  For 1000 Mbit/s from 20 MHz the
reference's own search gives N=1, M=50.  With that:

  - `PHY_STATUS` after the video-mode setup moves from `0x15b9` to `0x15bd`, so
    the change reaches the hardware.
  - But bit 2 is now set, which is the **clock** lane in stop state, where it
    was out of stop state before.  That is worse in one respect and the reason
    this is not being called a fix.
  - The DCS read still does not answer, and the DMA still stalls at the same
    `0x2200`.

- Acceptance points passed: none.  One wrong assumption removed.
- Remaining risk: the corrected divider leaves the clock lane in stop state,
  which suggests something else in the PHY sequence assumed the old rate.  The
  four lane-transition times are the obvious candidate - they came from
  ESP-IDF, where they accompany a correctly configured PLL, and they are in
  byte-clock units, so a rate that doubled changes what they mean.
- Next safe step: work the rate through consistently rather than one register
  at a time.  Everything derived from the lane rate needs re-deriving: the
  escape and timeout clock dividers, the horizontal timing scale factor, and
  whether `hs_freq_sel` and the transition times still match.  This port has
  been mixing values from two different rates, and correcting one of them in
  isolation is what produced the new symptom.

### 2026-08-23 - the lane rate is derived from one number, and 40 MHz is now measured

- State change: the whole DSI link is computed from the lane rate instead of
  being written out value by value, and the reference frequency is settled by
  measurement rather than by assumption.  The stall is unchanged.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The reference is 40 MHz, and this is how it was established.**  ESP-IDF's
name for the source it selects on pre-3.0 silicon is `PLL_F20M`, so 20 MHz was
taken seriously: with N=2 and M=50 that is 500 Mbit/s, and the whole link was
re-derived for it - dividers, range selector `0x07`, horizontal scale 25/16,
escape and timeout dividers.  Measured consistently at 500:

```text
pll n 2 m 50, range 0x00000007
host pkt 800 hsa 31 hbp 31 hline 1375
vid_mode 0x0000bf02 phy 0x000015bd
```

Everything agrees with itself, and the clock lane stops leaving stop state -
`phy` bit 2 set, where at 1000 it is clear.  The range selector has to match
the rate the lanes physically run at, so the lanes run at 1000, so the
reference is 40 MHz.  Put back, the clock lane leaves stop state again.

That is a measurement: a configuration change produced a lane-state change.
B3's values were right all along and B3's reasoning was still wrong - it read a
locking PLL as evidence for the reference frequency, and a loop that closes
says nothing about the frequency it closed on.  Both halves are worth recording
because the wrong reasoning survived four phases.

**And the link is now derived rather than transcribed.**  `P4_DSI_LANE_MBPS` is
the one number; the PLL dividers, the range selector, the pixel-to-byte-clock
scale factor and the escape and timeout dividers all follow from it, and a rate
with no range-table entry fails the build instead of silently keeping an old
selector.  This port had been carrying values for three different rates at
once - dividers for one, a selector for another, a scale factor for a third -
which is why correcting any single one of them changed which symptom appeared.

**What has not changed.**  `phy 0x15b9` after the video-mode setup: both data
lanes in stop state, the read path silent, the DMA stalled at `0x2200`.

- Acceptance points passed: none of B5's.  One assumption converted into a
  measurement and one class of inconsistency removed.
- Remaining risk: unchanged.
- Next safe step: the four lane-transition times are now the only values in the
  data path still unjustified, and they are in byte-clock units so they at
  least belong to the rate that is now confirmed.  Beyond them, what has never
  been tried is sending anything in command mode with the clock lane in high
  speed - every DCS write so far went out in low-power escape mode, and if the
  panel's controller expects high-speed commands, a silent read and lanes that
  never carry data are the same fact seen twice.

### 2026-08-23 - the panel answers: JD9365 identifies itself, and the read path is solved

- State change: the silent read path, open since B3 stage two, is fixed.  The
  panel returns its identity.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**The measurement.**

```text
[dsi]    dcs 0x04 identity read: 3 bytes, 0x00000093 0x00000065 0x00000004
```

`0x93 0x65` is JD9365 - the controller this panel carries, reading back its own
part number.  Five reads across four sessions had returned nothing at all
before this.

**The cause: the DCS command types were never set to low power.**
`CMD_MODE_CFG` has two groups of speed bits, GEN_ for generic packets and DCS_
for display-command-set packets, and each bit clear means "send this type in
high speed".  This port set the seven GEN_ bits and left bits 16 to 19, the
DCS_ group, at zero.

Everything a panel is actually spoken to in is DCS: the JD9365 initialisation
sequence, sleep-out, display-on, and the identity read.  So every one of those
packets was being asked for in high speed, on data lanes that never left stop
state - which is why the sequence produced no panel-side evidence of any kind
and every read was silent.  `ACK_RQST_EN` was missing as well, and it is what
makes a lost command visible rather than silent; the reference sets both.

`PHY_STATUS` after the read confirms it from the other side: `0x15ab`, where it
had been `0x15b9`.  Bit 4, `STOPSTATE0LANE`, is now clear - data lane 0 has left
stop state - and bit 1, `PHY_DIRECTION`, is set, so the host is in receive.  The
link carries traffic in both directions for the first time.

**How it was found, which is worth recording.**  The next thing to try was
high-speed commands, on the theory that the panel might require them.  Reading
the reference's command path first showed it sets every type to *low* power,
which refuted that theory before a build - and the same twenty lines showed the
DCS group being set at all, which this port did not do.  The hypothesis was
wrong and reading the reference to check it was what found the real defect.

- Acceptance points passed: B3's read path answers, which was its open defect.
- Acceptance points failed: B5 still has no image.  The second read, DCS 0x0A,
  does not answer and the host stays busy on the first, so the read path works
  once rather than repeatedly.  The DMA still stalls at `0x2200`.
- Remaining risk: the panel is now known to be initialised, or at least
  addressable, which changes the reading of everything downstream.  The
  JD9365 sequence was going out in the wrong speed mode too, so it has never
  actually reached the panel - the display may need it re-run now that it can
  be delivered.
- Next safe step: re-run the initialisation sequence with the corrected command
  mode and check the panel's own registers - the identity read proves reads
  work, so `0x0A` power mode and `0x0C` pixel format can now be asked and
  believed.  A panel that reports itself powered and in RGB565 is a much
  stronger starting point for the scanout than one that has never spoken.

### 2026-08-23 - two read-path defects fixed, and the panel goes quiet after its own init sequence

- State change: the read FIFO is drained completely and the bus turnaround is
  switched off again after every read, both of which were real defects.  The
  panel still answers only its identity.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.

**Two defects, both found by reading the reference's own read path.**

  - *The FIFO was left partly full.*  The drain loop stopped as soon as the
    caller's buffer was satisfied, and the FIFO hands out whole thirty-two bit
    words - so a three-byte identity read left one byte behind, and the next
    read found a non-empty FIFO and would have returned that leftover instead
    of waiting for its own reply.  The reference drains unconditionally and
    discards the excess.
  - *The bus turnaround was never switched off.*  `BTA_EN` is what lets the
    peripheral answer, and this port set it for a read and left it set.
    `PHY_STATUS` then reads `0x15ab` with `PHY_DIRECTION` set - the host sitting
    in receive - and a host in receive does not transmit.  That is one missing
    clear tying the silent read path and the stalled video scanout together, so
    it is now cleared on every exit path including the failures.

**What is still wrong, and the shape of it has changed.**  The identity read
answers; every read after it does not:

```text
dcs 0x04 identity read: 3 bytes, 0x93 0x65 0x04
dcs 0x0a power mode: no reply, pkt 0x00060054
```

`0x60054` is `GEN_RD_CMD_BUSY` set with `GEN_PLD_R_EMPTY` set: the host has sent
the read and is waiting for a reply that does not arrive.

And the ordering rules out the obvious explanation.  The identity read is the
first thing `krnP4DsiPanelInit` does, before any write; `0x0A` is asked after
the whole sequence has gone out.  So it is not that an uninitialised panel
answers less - it is that the panel answers before its initialisation sequence
and not after it.  Something in that sequence stops it responding.

That sequence has never actually reached the panel before today, since it went
out in the wrong speed mode, so its contents have never been tested against
hardware.  It is a page-unlock, `MADCTL`, `COLMOD`, a lane-count command and
Espressif's eight-entry table - and any one of them landing wrong on a panel
that is now listening would do this.

- Acceptance points passed: the read path works, which was B3's open defect.
- Acceptance points failed: B5 has no image, and `PHY_DIRECTION` is still set
  after the reads, so the host is still in receive when the scanout starts.
- Remaining risk: the initialisation sequence is now suspect in a way it could
  not be before.  It was written against a panel that could not hear it.
- Next safe step: send the sequence one command at a time and read `0x0A` after
  each, which localises the command that silences the panel.  The read path is
  reliable enough for that now, and it is the first time this port can ask the
  panel what a command did to it.

### 2026-08-24 - the initialisation sequence was eight commands out of 174

- State change: the JD9365 initialisation table is complete.  The panel
  identifies itself and the whole sequence goes out without error.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Artifact: core 177,792 bytes.

**The finding.**  `krnP4JD9365Init[]` had eight entries.  The vendor table in
the reference has **174**.

The eight this port had are the page-unlock magic - `0xE1 0x93`, `0xE2 0x65`,
`0xE3 0xF8`, `0x80 0x01` - plus sleep-out and display-on.  Everything between
them is the actual configuration: gamma curves, power settings, panel timing,
register pages one through four.  A JD9365 given the unlock and the display-on
and nothing else has been told to show a picture without being told how.

They came from an abbreviated example in a driver rather than from that
driver's actual initialisation path, and the mistake was invisible for four
sessions because the sequence was going out in the wrong speed mode anyway and
reaching nothing.  Fixing the speed mode is what made the sequence's contents
matter.

**The order around it was already right.**  Checked against the reference
command for command: identity read, `0xE0` page-user, `0x36` MADCTL, `0x3A`
COLMOD `0x55`, `0x80` with the two-lane code `0x01`, then the vendor table.
All five match, including the constants.

**What the run shows now.**

```text
dcs 0x04 identity read: 3 bytes, 0x93 0x65 0x04
jd9365 sequence completed, display on, backlight still dark
[b5]     sar 0x49e0e200 then 0x49e0e200  stalled
phy 0x000015ab   int1 0x00000080  DPI_PLD_WR_ERR
```

**And one more self-inflicted problem removed.**  The four diagnostic reads
after the sequence were stalling the scanout they exist to observe: a read
turns the link around, an unanswered read leaves the host in receive with
`PHY_DIRECTION` set, and a host in receive does not transmit video.  They are
now compiled out of the scanout path.  The identity read inside the init is
unaffected because it answers, so it completes.

`PHY_DIRECTION` is still set afterwards, so something still leaves the link
turned around, and the payload write error persists.

- Acceptance points passed: the panel is identified and fully initialised for
  the first time, which is B3's remaining substance.
- Acceptance points failed: B5 has no image.
- Remaining risk: the eight-command table is the second case this session of a
  value taken from an example rather than from the working path - the first was
  the DCS speed bits.  Anything else in this port copied from a snippet rather
  than from the reference's own code deserves the same suspicion.
- Next safe step: find what leaves `PHY_DIRECTION` set.  It is the last thing
  standing between a fully initialised panel and a scanout that transmits, and
  the identity read is now the only read on the path - so either it does not
  complete as cleanly as its answer suggests, or the bit means something other
  than a read in progress.

### 2026-08-24 - four defects in the video path, and PHY_DIRECTION explained

- State change: the link is no longer turned around, the bridge no longer
  underruns, and the bridge reads the frame in the format it is written in.
  B5 still produces no image.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_SCANOUT_TEST=1
  P4_LDSCRIPT=ldscript-xip.lds`.  Core 177,472 bytes, sha256
  `410dbc529fd90aec071b2b1a6bb98d0694daacb5ef81eea099ab6159dfa4b08f`,
  written to `ota_0` at `0x20000`.
- New: `krnP4DsiVideoOn`, a `PHY_STATUS` trace at eight points, and
  `P4_DSI_ACK_REQUEST` as a switch rather than a fixed choice.

**The measurement that made the difference.**  `PHY_STATUS` was being read once,
after the fact, and one reading is consistent with any step having set
`phy_direction`.  Recording it at each point where the link's direction can
change turned a four-session question into two runs.

**1. B5 never returned, so the backlight went out again.**  The B5 block ended
with a message saying it was left running and then fell through to
`krnP4PanelSafe()`, which darkens the backlight and re-asserts panel reset.
B4 carries a `return` for exactly this reason; B5 did not.  Reported from the
board as a flash of under half a second, and the same run printed "panel
returned to safe, backlight never on" - a message that was false about the run
it appeared in.  The backlight path was never broken.

**2. ACK_RQST_EN turned the link around, non-deterministically.**  With the bit
set, `PHY_STATUS` after the initialisation sequence read `0x15af` in one run
and `0x15bd` in another, and where it read `0x15af` that value survived the
whole handover to video mode unchanged.  `0x15af` is `phy_direction` set, lane
0 out of stop state and lane 1 in it, which is a bus turnaround in progress and
not a dead second lane - all three bits have one cause.  With the bit clear,
eight readings in one run and `phy_direction` set in none of them.

The reference does set `ack_rqst_en`, which is why this port did.  The two are
not in the same position: the reference reads and reports the acknowledgements,
this port has no handler for them, and the timeout counters are disabled on
both sides, so an acknowledgement that never arrives is a wait that never ends.
The bit is now `P4_DSI_ACK_REQUEST`, off by default.

**3. The bridge was reading RGB888 out of an RGB565 frame.**
`DSI_BRG_PIXEL_TYPE` read `0x00000000`, and `raw_type` 0 is RGB888.  The
register had been printed as a diagnostic since B5's first run without anyone
asking what zero meant.  The bridge therefore fetched three bytes per pixel
from a two-byte-per-pixel frame and offered the host twenty-four bits per pixel
where `DPI_COLOR_CODING` says sixteen.  Set to `raw_type` 2 for this revision,
which is what hw_ver1's reference path sets for both the input and the output
format; the bridge underrun disappeared with it.

Note for future comparisons: hw_ver3 splits this into `raw_type` and
`dpi_type`, and the two branches are `#if CHIP_SUPPORT_MIN_REV >= 300` in
`mipi_dsi_brg_ll.h`.  Reading the wrong branch gives `0x22` instead of `0x02`.

**4. The host entered video mode before anything could feed it.**  The
reference arms the DMA channel first, then enables the host's video mode, then
the bridge's DPI output.  This port did all of it inside the staging call, so
the host was in video mode against an empty DPI input.  Split into
`krnP4DsiVideoOn`, called between `krnP4ScanoutDmaUp` and
`krnP4ScanoutFeedOn`.

**Where it stands.**

```text
[b5]     brg  en 0x00000001 pix 0x00000002
[b5]     brg  flow 0x00000010 rawnum 0x0003e800 misc 0x00003201 int 0x00000000
[b5]     host pkt 0x00050015 int0 0x00000000 int1 0x00000080  DPI_PLD_WR_ERR
[b5]     sar 0x49e0c000 then 0x49e0c000  stalled
[b5]     phy trace 15bd 15bd 15ad 15bd 15bd 15bd 15bd 15b9
[b5]     turned around after step none, it is not turned around
```

`int 0x00000000` is the bridge reporting no underrun, which is new.  The DMA
now does not move at all, where before the pixel-format fix it moved 8,704
bytes: the bridge is not requesting data.  `0x15b9` is a host with the clock
lane in high speed, the direction outbound and both data lanes in stop state,
so it is not transmitting.

**What was checked and found correct.**  The whole of the reference's bus and
DPI configuration, value by value: escape and timeout clock divisions (7 from
/18 and 13 from /10), all six timeout counters at zero, `PHY_TMR_CFG` and
`PHY_TMR_LPCLK_CFG`, `stop_wait_time` 0x3F, `PHY_TMR_RD_CFG` 6000, EOTP
transmit, receive CRC and ECC, lane count as `n_lanes = lanes - 1`,
`enableclk` and `forcepll`, burst type 2, `raw_num_total` 256,000, discard
count 800, empty threshold 768, multi-block 1, burst length 256, DPI clock
source 1 with divider `div - 1`, the bridge configuration-update trigger, and
the whole of `CH_CFG1` including `src_osr_lmt`/`dst_osr_lmt` as `limit - 1`.
Two candidates were raised and eliminated from the reference itself:
`DSI_CFG_REF_CLK_EN` defaults to 1 and nothing in ESP-IDF ever writes it, and
the host's `pwr_up` is set once and never cycled for configuration.

- Acceptance points passed: none of B5's; this is diagnosis, not function.
- Acceptance points failed: B5 has no image.
- Remaining risk: `DPI_PLD_WR_ERR` has carried a lot of interpretation without
  its origin being established.  It is a latched interrupt status and this port
  never clears it, so it may date from the command phase rather than from the
  scanout.  Until it is read before the handover and after a clear, no argument
  should rest on it.
- Next safe step: measured in the same session, see below.

### 2026-08-24 - the payload error is continuous, and the DMA never starts

- State change: none.  A measurement that removes an ambiguity.
- Test defines: as above.  Core 177,552 bytes, sha256
  `9d1084aec039dce1a6cb675313539804767ea6464165ab415069e512a2c9e909`.

`INT_ST0` and `INT_ST1` are cleared by reading them, so every previous reading
said only that something had happened since this probe last looked - and it
looks in B3 as well.  Read three times, the second immediately and the third
after 50 ms:

```text
[b5]     host pkt 0x00050015 int0 0x00000000 int1 0x00000080 then 0x00000000
         then 0x00000080  still failing
```

The second reading is zero, which confirms clear-on-read; the third is set
again, so the host is producing payload write errors continuously rather than
having latched one during the handover.  It is receiving pixels it cannot send.

At the same time `sar` does not move at all, so the DMA delivers nothing, and
the bridge reports no underrun.  Before the pixel-format fix the DMA moved
8,704 bytes; after it, nothing.  Those two facts do not yet compose into one
account, and that is the open question rather than a conclusion.

**The comparison against the reference is now exhaustive** for the bus, the DPI
configuration, the channel configuration and the link-list item, including
`LLI_VALID` at bit 31 of `CTL1`, source burst 512 against destination burst
256, `burst_len` 16 on both sides and a transfer size of 256,000 64-bit items.
Every value matches.  What has not been instrumented is the bridge's own
progress: there is no reading in this port that says whether the bridge ever
starts a frame, and both remaining symptoms are consistent with it never
starting one.

- Acceptance points passed: none.
- Remaining risk: the DMA moving 8,704 bytes with the wrong pixel format and
  nothing with the right one is unexplained.  It may mean the bridge's frame
  start depends on something the format changed, or that the earlier movement
  was not the bridge requesting data at all.
- Next safe step: instrument the bridge rather than the host.  Read whatever
  the bridge exposes about its own progress - its interrupt status repeatedly
  rather than once, and any counter that advances - because a bridge that never
  starts a frame explains a DMA that never starts and a host that receives
  nothing it can send, and no host-side register can distinguish that from the
  alternatives.

### 2026-08-24 - the bridge instrumented, and the DMA runs continuously

- State change: pixels move from PSRAM through the DMA into the DSI bridge,
  continuously and without software intervention.  The host still does not
  transmit them.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_SCANOUT_TEST=1
  P4_LDSCRIPT=ldscript-xip.lds`.  Core 179,136 bytes, sha256
  `2ace318756139fba11a57f9635afe3c67b3707780ae52392b20cfb4c7f8de16d`,
  written to `ota_0` at `0x20000`.
- New: `CH_INTSTATUS0/1`, `DSI_BRG_FIFO_STATUS` sampled six times, and the ten
  bridge registers this port never writes.  The link-list descriptor is gone.

**The instrumentation answered immediately.**  Two readings this port had never
taken:

```text
[b5]     dma  int0 0x00002010 int1 0x00000000  lli_invalid
[b5]     brg  depth 0 0 0 0 0 0
[b5]     brg  raw   0x0 0x0 0x0 0x0 0x0 0x0
```

Bit 13 of `CH_INTSTATUS0` is `SHADOWREG_OR_LLI_INVALID_ERR`: the channel
fetched the descriptor and rejected it.  The bridge fifo sat at zero across
six samples 10 ms apart and never raised an underrun, so it had never started a
frame.  Neither fact is visible in the source address, which is all this port
had been reading.

**And a third register explained the contradiction.**  `RSV_DPI_DATA` reads
`0x3fff`: on fifo underflow the bridge does not stop, it substitutes this
reserved pixel value and keeps feeding the host.  That is how the host could
report a continuous payload error while nothing at all came out of memory, and
it retires the reasoning that treated `DPI_PLD_WR_ERR` as evidence about the
DMA.

**Why the descriptor was rejected, measured rather than assumed.**  Read back
with the cache invalidated first:

```text
[b5]     lli  sar 0x49e0c000 dar 0x50105000 ts 0x0003e7ff llp 0x4ff02680
[b5]     lli  ctl 0x001e1b40/0x000f87c0  VALID CLEARED by the engine
```

Source, destination, block size and the self-pointer are all intact.  Bit 31 of
the control word - `LLI_VALID`, which this code writes before arming the
channel - is clear.  The engine clears it when it consumes an item, so an item
whose next-pointer addresses itself is valid exactly once and invalid every
time after.  That is also why the reference marks its single item last and
re-arms from a transfer-done callback: with a link list there is no other way,
and this port has no interrupt handler to do it from.

**Automatic reload instead.**  `CFG0.SRC_MULTBLK_TYPE` and `DST_MULTBLK_TYPE`
set to 1 rather than 3, the transfer parameters written into the channel's own
registers, `LLP` zeroed.  The channel restores its configuration at the end of
every block and starts the next one indefinitely.  No descriptor, no cache
maintenance for one, no interrupt handler.

```text
[b5]     dma  int0 0x00000010 int1 0x00000000  nothing reported
[b5]     brg  depth 911 905 908 868 848 808
[b5]     sar 0x49ef8200 then 0x49e44e80  moving
```

The fifo holds around nine hundred of its 1024 entries and drains slowly, the
source address walks the frame and wraps to the start on its own.  This is the
first continuous pixel path in the port.

**What is left.**  The host takes the pixels and does not send them:
`PHY_STATUS` `0x15b9` is the PLL locked, the direction outbound, the clock lane
in high speed and both data lanes in stop state, with `DPI_PLD_WR_ERR` standing
on every read.  A host with a full payload fifo whose data lanes never leave
stop state is not a starved host; it is one that will not begin a transmission.

Checked and correct in the same pass: the switch times against
`mipi_dsi_phy_ll_set_switch_time(50, 104, 46, 128)` including the parameter
order, `phy_lp2hs_time` at bits 9:0 and `phy_hs2lp_time` at 25:16 and the same
split for the clock lane, and every channel register offset.  `DPISHUTDN`,
`DPICOLORM` and `DPIUPDATECFG` all read zero and ESP-IDF never writes any of
them, so their reset values are the working ones.

- Acceptance points passed: none of B5's formally; the data path is the phase's
  substance and it now runs.
- Acceptance points failed: B5 has no image.
- Remaining risk: `int0` bit 4, `DST_TRANSCOMP`, is set in both the failing and
  the working configuration, so it is not diagnostic of anything and should not
  be read as progress.
- Next safe step: the host, and specifically why a video-mode transmission
  never begins.  The bridge is no longer a candidate: it holds pixels and hands
  them over.  Worth reading before changing anything are `VID_MODE_CFG`'s burst
  type against the packet size the host is asked to buffer, and whether the
  host's own DPI input sees the bridge's vertical and horizontal sync at all -
  there is no reading yet that says a frame ever starts on the DPI interface.

### 2026-08-25 - B5 puts an image on the panel, and four of this session's findings were withdrawn

- State change: the panel displays the framebuffer.  The grid measures correct
  in both axes; a displaced second copy of every line remains, and its cause is
  open.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_SCANOUT_TEST=1 P4_DSI_NONBURST=1
  P4_SCANOUT_GRID=1 P4_BL_PERCENT=100 P4_LDSCRIPT=ldscript-xip.lds`.
- New switches: `P4_PANEL_565`, `P4_DSI_NONBURST`, `P4_DSI_CHUNKS`,
  `P4_DSI_FRAME_ACK`, `P4_DSI_PANEL_QUERY`, `P4_SCANOUT_TESTCARD`,
  `P4_SCANOUT_CROSS`, `P4_SCANOUT_GRID`, `P4_BRG_RAW_DIV`, `P4_BRG_VDIV`,
  `P4_DPI_MHZ`, `P4_VFP`, `P4_BL_PERCENT`.

**What was fixed, in order of how much it mattered.**

  - *The backlight defaulted to 20 per cent.*  A faint image on a dim panel and
    no image at all look identical from across a desk, and this masked
    everything else for two rounds.  Now overridable.
  - *B5 fell through to `krnP4PanelSafe()`.*  It printed "left running" and
    then darkened the backlight and re-asserted panel reset, so the panel lit
    for well under a second.  B4 carries a `return` for exactly this reason.
  - *`ACK_RQST_EN` turned the link around.*  A bus turnaround per command,
    unanswered, leaving `phy_direction` set through the handover to video.
    Measured at eight points: with the bit set the link is turned around after
    the command sequence in some runs and not others; clear, in none.
  - *The bridge pixel format was RGB888 on an RGB565 frame.*  `PIXEL_TYPE` read
    zero and `raw_type` zero is RGB888.  The register had been printed as a
    diagnostic since B5's first run without anyone asking what zero meant.
  - *The host entered video mode before the DMA was armed.*  The reference arms
    the channel first.  Split out as `krnP4DsiVideoOn`.
  - *Two entries were believed missing from the initialisation table.*  This
    finding is withdrawn by the 2026-08-26 effective-source audit below.  The
    earlier extraction counted a commented-out `0x4A 0x35` BIST write as live
    and overlooked that the wrapper already sends the first of the two
    effective `0xE0 0x00` page selections.  Adding both made AROS's table 176
    entries against the reference's active 174.
  - *Display-on was only sent from the table.*  The vendor driver sends `0x29`
    a second time as `tx_param(io, LCD_CMD_DISPON, NULL, 0)` - no parameter -
    and the firmware that drives this board calls it explicitly after panel
    init.  `0x29` has no one-parameter form, so the table's version is a packet
    the controller may discard.

**The measurement that made the pixel format decidable.**  DCS `0x0A` asked
*after* the video handover instead of before it:

```text
24-bit, 80 MHz, 1500 Mbit/s    0x1C   display on, normal mode, awake
RGB565, 40 MHz, 1000 Mbit/s    0x18   display off, normal mode, awake
```

Every earlier reading was taken before the handover, where a panel that has
seen no pixels reports its output disabled for the obvious reason.  That made
the answer look constant and uninformative for several rounds.  It is the only
instrument in this phase that reports on the panel without a photograph, and it
is worth reaching for first next time.

**Four findings withdrawn.**  Each was a measurement taken with a known
disturbance still in the path.

  - *"The panel is 24-bit because it says so and the vendor test app agrees."*
    The firmware that runs this board configures RGB565 at 40 MHz over 1000
    Mbit/s.  A component's test application is not a board configuration.  The
    24-bit profile is nonetheless the only one that produces an image here, so
    both are kept as switchable profiles and neither is called correct.
  - *"The 40 MHz DPI clock is why burst mode will not start."*  Burst does not
    start at the vendor's clocks either.
  - *"Frame acknowledge stops the host transmitting."*  Measured while
    `ACK_RQST_EN` had the link turned around.  Measured properly, the host
    transmits and the panel goes black, because it does not answer the
    turnaround.
  - *"The bridge reads through vertical blanking."*  From a 5 ms rate window
    inside a 14.6 ms frame, which measures the active rate by construction.
    Over 100 ms the bridge draws 213 MB/s against the 210 the timing calls for.

**What the panel measures.**  Peaks located per row and per column in a
photograph rather than counted by eye: 13 row lines at 87 px spacing where 86
is expected, and 8 column lines.  The geometry is correct and the earlier
reading of "twice the image" does not hold for this configuration.  Every line
carries a second, fainter copy 34 panel lines away.

**Also established.**  Burst mode is unusable on this path: with the line split
into four packets the bridge draws 58.2 frames per second against the 33 the
timing calls for and the host does not transmit at all; non-burst draws 35.5
and transmits.  Halving only the bridge's active-line count stops the transfer
entirely rather than halving its rate, so the bridge honours that count and
refuses to run when it disagrees with `raw_num_total`.

- Acceptance points passed: pixels cross from PSRAM to the panel and are
  displayed.
- Acceptance points failed: a clean single frame, and the 100 MB/s bandwidth
  gate reassigned here from B1.
- Remaining risk: the 24-bit profile is not the vendor's, and the reason RGB565
  transmits without error while the panel refuses to enable its output is not
  understood.  Building on the 24-bit profile means building on a configuration
  whose only justification is that it works here.
- Next safe step: look at the panel directly rather than at a photograph, and
  establish whether the second copy of each line is on the panel or in the
  exposure.  A 1.4 per cent rate difference across an exposure spanning several
  frames is enough to place a displaced copy in an image, and that is the
  cheaper explanation.  If it is on the panel, the next measurement is
  frame-phase coupling between the DMA and the bridge.

### 2026-08-25 - B5 geometry solved: the panel takes two transmitted lines per row

- State change: the scanout puts a dimensionally correct image on the panel.
  Grid pitch measures 98 panel rows against the 100 drawn, within the error of
  reading the active area's edges off a photograph.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Test defines: as the previous entry plus `P4_PANEL_VMUL=2`.
- Artifact: core 183,648 bytes, sha256
  `2fa1cd4e2c3ee0b157f4cbdfb97dfe518dac39a16f092856b8a8ad36590cc1c5`.

**The measurement.**  Three isolated white rows, not a grid.  Framebuffer rows
100, 500 and 900 arrived on panel rows 50, 256 and 462, and again 640 rows
lower.  So framebuffer row `y` lands on panel row `y/2` and the frame repeats.

A grid could not have shown this.  At a pitch of 100 one line's second copy
lands where the next line's first copy goes, so the halving and the repeat alias
into a single plausible count - which is what four rounds of this phase were
spent interpreting.  Isolated features separated by more than the defect are
what made it readable.

**The correction.**  `P4_PANEL_VMUL` transmits twice the panel's line count.
Confirmed on hardware: three rows at even spacing, the first 1.5 cm from the
edge, which is where row 100 of 1280 falls on a 173 mm panel axis, and two equal
gaps of 400 rows.  Grid then measures 13 row lines at 92 px where 1200 px covers
1280 rows.

Everything on the transmit side derives from `P4_TX_V_RES`: framebuffer size,
the bridge's active and total line counts, `VID_VACTIVE_LINES` and
`raw_num_total`.  Five values that have to agree, from one definition.

**The mechanism is not explained.**  The obvious reading - the panel expects
1600 pixels per line and joins two transmitted rows side by side - is ruled out.
A pattern with a bar in the left half of even rows, the right half of odd rows,
and a deliberate gap between them arrives as one continuous line with no gap.
So `P4_PANEL_VMUL=2` is a correction with a measurement behind it and no theory,
and is recorded as such.

- Acceptance points passed: a dimensionally correct `800 x 1280` frame from
  PSRAM on the panel.
- Acceptance points failed: the frame is not clean.  Row lines arrive in
  changing colours although they are drawn in one channel, and column lines
  arrive dotted - a byte offset that walks from row to row, which at three
  bytes per pixel rotates the channel assignment.  The 100 MB/s bandwidth gate
  reassigned here from B1 is also unmeasured.
- Remaining risk: two configuration choices now rest on measurement without
  explanation - `P4_PANEL_VMUL=2`, and the 24-bit profile that the vendor
  firmware does not use.  Neither is understood, both are needed for an image.
- Next safe step: characterise the byte offset.  Its period in rows and whether
  it accumulates or resets per frame both follow from a pattern that puts a
  known byte value at a known column, and it is the last defect between this
  and a clean frame.

### 2026-08-25 - B5 transmits a clean frame; the byte order was the last defect

- State change: the panel shows a dimensionally correct grid with every line in
  one continuous colour.  The transmit side reports no payload error across
  thirty seconds.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Working configuration, now the default in `hardware.h`: RGB565, 40 MHz pixel
  clock, 1500 Mbit/s lanes, `P4_PANEL_VMUL=2`, non-burst, `P4_PANEL_VBP` 30.
  `P4_PANEL_24BIT` selects the superseded 24-bit profile.

**The last defect was a byte swap, and it faked a different one.**  Three bands
with `0xFF` in one pixel byte each: byte 0 arrived yellow, byte 1 blue.  In
RGB565 the high byte carries red and the top green bits - yellow - so the panel
reads the first byte written as the high one, and `px()` was writing the low
byte first.  Every pixel was swapped, identically.

What was recorded across four rounds as "a byte offset that walks from row to
row" was that uniform swap seen through one-pixel grid lines: on a thin line a
uniform swap changes which colour survives recognisably, and reading a run of
those as a period was the error.  A flat single-channel field shows it at once
and was reached for late.

**The lane rate is what the panel judges.**  Crossed one at a time against DCS
`0x0A` read *after* the video handover:

```text
RGB565  40 MHz  1000 Mbit/s   rejects    transmit side clean
RGB565  80 MHz  1500 Mbit/s   accepts    DPI_PLD_WR_ERR every second
RGB565  40 MHz  1500 Mbit/s   accepts    clean
```

The move to 24 bits per pixel was a wrong turn: it changed colour depth, pixel
clock, lane rate and back porch together, and the result was credited to the
depth, the one thing that did not matter.  Note that the vendor firmware runs
1000 with 40 and works; why this port cannot get the panel to accept that
pairing is **unexplained**.

**The boot hang is located and contained, not fixed.**  With SRAM-resident stage
markers a hung boot prints `123456789ab789ab789ab` and stops.  Marker `b` sits
immediately before `p4_psram_probe_latency`; the first two calls into the sweep
return, the third does not.  Each ask inside it is an unbounded read through the
ROM SPI helpers, and the file's own comment - written before this was measured -
says a read at the wrong width does not fail, it does not return.

The sweep is now off by default, so the same board prints its diagnosis and
carries on headless instead of stopping.  A bound would be better and needs the
MSPI transaction started and polled by `psram_init.c` rather than by
`rom_cmd_start`; the P4 headers do not carry the register names the earlier
chips use and this port has not verified them.

**What puts the chip in that state.**  A scanout running across a reset.  The
GDMA survives a CPU reset, and between the reset and this port's code sit the
ROM and ESP-IDF bootloaders - a few hundred milliseconds in which the channel
keeps reading PSRAM while nothing has yet reconfigured it.  `krnP4ScanoutQuiesce`
therefore arrives too late to prevent it and only helps when the reset falls
after the scanout has stopped.  `P4_SCANOUT_SECS` bounds the scanout to 60 s by
default for that reason, and the run prints `scanout stopped, panel safe` when a
reset becomes safe.

Recovery, used three times: flash `~/Source/Vellum/firmware/build/vellum-d1001.bin`
to `0x20000`, boot it once - it reports `vendor id 0x0d`, X16 mode - then flash
the AROS core back.  Two and a half minutes.  The vendor bring-up leaves the
chip in the width this port assumes; it never addressed the hang.

- Acceptance points passed: a dimensionally correct, cleanly transmitted
  `800 x 1280` frame from PSRAM on the panel.
- Acceptance points failed: the 100 MB/s bandwidth gate reassigned here from B1
  is still unmeasured.
- Remaining risk: two settings carry measurement without explanation -
  `P4_PANEL_VMUL=2`, and a lane rate the vendor firmware does not need.  The
  first was measured while every pixel was byte-swapped, so it deserves a
  retest now that the swap is gone; that test was built and the board went into
  the ROM download stub before it could be read.
- Next safe step: run the grid with `P4_PANEL_VMUL=1` and look.  A full correct
  grid means the doubling can be deleted.  Then the channel mapping in `px()`:
  rows are drawn in this code's green and arrive blue while red arrives red.

### 2026-08-26 - B5 line doubling withdrawn after clean VMUL=1 hardware run

- State change: B5 remains `hardware partial`, but its geometry is now the
  native `800 x 1280`; `P4_PANEL_VMUL=2` is deleted as the default and retained
  only as a diagnostic override.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, restarted through
  USB-Serial/JTAG rather than a battery disconnect.
- Artifact and configuration: core 184,656 bytes, SHA-256
  `3a42a5d8dd0c340837456947097272f4702c402de5b844d77d03b9c2d6bc1aea`;
  RGB565, 40 MHz pixel clock, 1500 Mbit/s lanes, non-burst,
  `P4_PANEL_VMUL=1`, grid pattern, 200 MHz calibrated PSRAM and 360 MHz CPU.
- Default-source rebuild: after changing `hardware.h`, all generated kernel
  objects were deleted and the same diagnostic configuration was rebuilt
  without any `P4_PANEL_VMUL` command-line override.  The first invocation was
  correctly rejected because `esptool` was absent from `PATH` and left the old
  image untouched.  With the local Espressif environment in `PATH`, the log
  contains both `Creating .../aros-esp32p4.bin` and `Successfully created
  ESP32-P4 image`, no `error:`, and produced a new 184,656-byte core, SHA-256
  `234831639428cd55c920f4fab7bcc43fb6cce0a0ed059efc7b698aaa7aa70fe0`.
  It was written only to `ota_0` at `0x20000` and passed both write-time hash
  verification and a separate `esptool verify-flash`.
- Procedure and observed UART: `tools/reset-and-log.py` reset the board out of
  the ROM download stub and captured the boot from its first byte.  PSRAM
  calibrated in one attempt, the bridge and host both reported 1280 active
  lines, the framebuffer was exactly 2,048,000 bytes, DSI reported no payload
  error and scanout measured 71 MB/s.  The scanout remained bounded to 30
  seconds and then stops before another reset.
- Panel observation: the complete red/blue grid was clean and regular.  There
  was no half-height image, repeated lower half, displaced copy or line
  corruption.  This directly falsifies the earlier VMUL=2 conclusion, which
  had been measured while every pixel byte pair was swapped.
- Default-source boot: a subsequent USB reset entered normal SPI boot, PSRAM
  calibrated at 200 MHz in one attempt, framebuffer size was again exactly
  2,048,000 bytes, bridge and host each reported 1280 active lines, DSI stayed
  free of payload errors and scanout measured 71 MB/s.  This build uses the
  60-second default scanout bound before panel-safe shutdown.
- Acceptance points passed: native geometry, one-pixel/grid-line continuity,
  PSRAM-backed scanout and a clean transport-side observation for this run.
- Acceptance points still open: the grid requests red columns and green rows,
  but the rows appear blue; full solids/bars/checkerboards, dirty-rectangle
  coherency, 30-minute concurrent stress, the 100 MB/s bandwidth gate and ten
  cold plus ten warm boots have not passed.
- Safety impact: the rebuilt core was written only to the authorized `ota_0`
  range beginning at `0x20000`; no BSP, partition metadata or media was
  written.  Reset used the USB control lines, scanout is time-bounded, and
  panel-safe shutdown remains enabled.
- Next safe step: isolate the green/blue mapping with the existing primary
  colour test card or flat fields, changing only the channel-format variable.

### 2026-08-26 - B5 RGB565 colour mapping hardware verified

- State change: B5 remains `hardware partial`, but its RGB565 byte and channel
  mapping is now hardware verified.  The first run changed only the diagnostic
  framebuffer pattern from the proven grid to the existing primary/pair-colour
  card; the correction run changed only the central RGB565 packer's byte order.
  Geometry, clocks, lane rate and video mode stayed unchanged.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3.
- Artifact and build: all generated kernel objects were deleted before the
  rebuild.  The build log contains `Creating .../aros-esp32p4.bin` and
  `Successfully created ESP32-P4 image`, contains no `error:`, and produced a
  184,656-byte core, SHA-256
  `138d9a25357a59bf99e263dff9d8d79f14e9c30a29a0bf54f83b0424f9192035`.
- Flash verification: only `ota_0` at `0x20000..0x4dfff` was erased and
  written.  The write-time digest matched and a separate `verify-flash`
  invocation reported `Verification successful (digest matched)`.
- UART observation from a first-byte USB reset: normal SPI boot; PSRAM
  calibrated at 200 MHz in one attempt; framebuffer exactly 2,048,000 bytes;
  bridge and host each report 1280 active lines; no DSI payload error; DMA SAR
  moves; scanout measured 69 MB/s against the calculated 69 MB/s requirement;
  scanout is bounded to 60 seconds.
- Panel observation: `IMG_0922.HEIC` shows a clean, dimensionally regular card.
  Accounting for the D1001's physical landscape rotation, the requested
  top-to-bottom red, green, blue, yellow, cyan, magenta, white and gray bars
  arrive as blue, red, green, magenta, yellow, cyan, white and gray.  This
  initially looks like a cyclic primary mapping, but the actual 16-bit values
  identify the cause exactly: high-byte-first makes requested red/green/blue
  `0x00f8`, `0xe007` and `0x1f00` when consumed as native little-endian
  RGB565, producing those three observed colours.  The pair colours undergo
  the corresponding swaps, while white remains white.  This is a byte-order
  fault, not a stride or DSI channel-routing fault.
- Safety impact: only the authorized core range was written.  No BSP,
  partition metadata or media was changed, and panel-safe shutdown remains
  enabled.
- Local reference audit: ESP-IDF v6.0.1 configures the revision-one bridge as
  raw RGB565 and host 16-bit configuration 1, with no cyclic RGB routing
  control; the D1001 BSP declares `BSP_LCD_BIGENDIAN=0`.  Its native
  little-endian framebuffer convention agrees with the bit-level panel result.
- Next gate: write RGB565 low byte first in the central packer and repeat the
  same card unchanged.  Correct red, green, blue, yellow, cyan and magenta in
  the requested order will close the colour mapping.
- Correction re-test armed: `px()` now writes the low byte first and no other
  scanout variable changed.  After deleting all generated kernel objects, the
  clean build contains both image-creation success markers and no `error:`;
  the 184,656-byte core has SHA-256
  `688d3c9843cf743d76efc6599ed7e1732fa670dffff0c7e9913c53820228f3f7`.
  Only `ota_0` at `0x20000..0x4dfff` was written, with both write-time digest
  verification and a separate successful `verify-flash`.
- Correction re-test UART: normal SPI boot from the first byte; PSRAM at
  200 MHz calibrated in one attempt; framebuffer exactly 2,048,000 bytes;
  bridge and host each report 1280 active lines; no DSI payload error; DMA SAR
  moves; scanout measured the required 69 MB/s and remains bounded to 60
  seconds.
- Correction re-test panel observation: confirmed correct.  Accounting for the
  D1001's physical landscape rotation, the visible bars are red, green, blue,
  yellow, cyan, magenta, white and gray in the requested order, with the other
  half black.  The border and ruler remain straight and continuous.  This
  closes RGB565 byte order and primary-channel mapping on D1001 hardware.
- Acceptance points passed here: all three primary colours, all three pair
  colours, white, gray and black; distinct top/bottom halves; continuous
  border/ruler; unchanged native geometry and error-free transport.
- Remaining B5 gates: full solid/checkerboard/corner coverage, dirty-rectangle
  coherency, 30-minute concurrent stress, the 100 MB/s bandwidth gate and ten
  cold plus ten warm boots.  At the time of this run, reset-surviving PSRAM
  recovery also remained an implementation defect; the next entry supersedes
  that last point with hardware evidence.

### 2026-08-26 - B5 bounded MSPI command recovery hardware verified

- State change: B5 remains `hardware partial`.  The reset-surviving **PSRAM**
  blocker is closed; a distinct DSI/DMA restart defect remains.  This entry
  supersedes the previous entry's statement that PSRAM recovery was still an
  implementation defect and the older containment-only handoff at the end of
  the 25 August investigation.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`.
- Implementation: `esp_rom_spi_set_op_mode` and `esp_rom_spi_cmd_config` still
  program each transaction.  `rom_cmd_start` is replaced by an SRAM-resident
  local start/poll/copy operation with a 3,600,000-cycle bound.  On expiry it
  restores the ROM's chip-select state and pulses `SYNC_RESET` on both PSRAM
  MSPI FSMs in Espressif's clear/assert/clear order.  Every command returns a
  status; mode-register access, identification, round trips and tuning stop
  consuming receive buffers after a timeout.  Boot output reports separate
  timeout and FSM-recovery counters.  The previously disabled eight-latency
  recovery sweep is therefore enabled by default.
- Local-source evidence: disassembly of `esp32p4_rev0_rom.elf` at
  `esp_rom_spi_cmd_start` gives MISC offset `0x34`, buffer offset `0x58`, the
  CS restore and the unbounded `CMD != 0` loop.  Both ESP-IDF v6.0.1 P4
  `spi1_mem_s_reg.h` hardware-version headers give `USR` at bit 18 and
  `SYNC_RESET` at `CTRL2 + 0x10`, bit 31.  Espressif's low-level reset helper
  supplies the clear/assert/clear ordering.
- Correction made during hardware work: the ROM instruction `lui 0x40` was
  first misread as `0x00400000`; on RV32 it produces `0x00040000`, bit 18.
  The resulting 185,520-byte diagnostic image did not talk to PSRAM, but the
  new bound caught all 33 commands, performed 33 FSM resets and let AROS stay
  alive headless.  That is valid negative evidence for the bound, not a valid
  PSRAM transaction.  The IDF headers and hardware correction agree on bit 18.
- Normal-path proof: the 185,856-byte corrected image brought up 32 MB at
  200 MHz, found read latency 4, chose a 23-of-31 tuning window, verified one
  word per MiB across the mapped window and reported `command timeouts 0, FSM
  recoveries 0`; B5 then measured the required 69 MB/s with no payload error.
- Deterministic recovery proof: `P4_PSRAM_TIMEOUT_TEST=1` makes exactly one
  command use the already measured non-self-clearing bit 22, then resumes with
  bit 18.  The 185,952-byte image, SHA-256
  `c8ff471a33e7a8ce1b24cc3eba51bb41e599e7a800f2c7316fead474c34f0d9f`,
  reported exactly `command timeouts 1, FSM recoveries 1`, then completed the
  same 32 MB / 200 MHz identification, tuning and full-window check and fed B5
  at 69 MB/s.  This proves a transaction after the FSM reset, not merely that
  the timeout returns.
- Non-injected artifact used for the active-reset test: clean rebuild with all
  generated kernel objects deleted and the last proven display configuration:
  `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360 P4_PANEL_PROBE=1
  P4_DSI_PROBE=1 P4_SCANOUT_TEST=1 P4_DSI_NONBURST=1
  P4_SCANOUT_TESTCARD=1 P4_BL_PERCENT=100
  P4_LDSCRIPT=ldscript-xip.lds`.  Both image-creation success markers are
  present and no `error:` occurs.  The 185,872-byte core has SHA-256
  `a61d165b11e5f3c859499a5380720cf70ac81f822d4b48b478a208ba1d63469a`.
- Active-reset proof: immediately before reset that image reported a
  moving GDMA source address and measured 69 MB/s from PSRAM with no payload
  error.  A USB reset was then deliberately issued while that scanout was
  active.  The next boot again identified 32 MB at 200 MHz in one attempt,
  found latency 4, chose a 23-of-31 tuning window, verified the full mapped
  window and reported zero command timeouts/recoveries.  No Vellum/vendor
  firmware and no power cycle was needed.
- Post-review delivery artifact: the mode-initialisation return was then made
  authoritative, so failed mode writes cannot be masked by a later data
  transaction.  A final rebuild with the same flags contains both image
  success markers and no `error:`.  The 185,872-byte core has SHA-256
  `5d6676c2fa28d428085922066e5eb5e35bdf9a27ee57052f2f2f25bcc7d141bc`.
  Only `ota_0` at `0x20000..0x4dfff` was written; write-time hashing and an
  independent `verify-flash` passed.  Its D1001 boot again brought up 32 MB at
  200 MHz in one attempt, selected a 24-of-31 window, verified the mapped
  window and reported zero timeouts/recoveries.  The already-created DSI
  retained-state fault remained visible as a stalled new DMA, independently
  confirming that PSRAM is available while scanout restart is not.
- Newly separated defect: after that active reset the panel power-mode read got
  no reply and the newly configured GDMA source address did not move; the B5
  sampler called it `stalled`.  PSRAM was already fully verified at that point,
  so this is DSI/bridge/GDMA reset continuity, not PSRAM recovery.  Ordinary
  work must still avoid resetting a running scanout until this is fixed.
- Remaining acceptance: this is one deliberately evidenced active warm reset,
  not the B5 requirement of ten warm plus ten cold boots.  Full solids,
  checkerboard/corners, dirty-rectangle coherency, 30-minute concurrent stress,
  the 100 MB/s gate and the full boot-count gate remain open.

### 2026-08-26 - B5 active-scanout DSI reset continuity hardware verified

- State change: B5 remains `hardware partial`, but the DSI/bridge/GDMA restart
  defect separated in the preceding entry is closed for its reproduced active
  warm-reset case.  Together with the bounded MSPI work, a running scanout no
  longer requires vendor firmware or a battery disconnect after USB reset.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`.
- Local-source finding: ESP-IDF v6.0.1's revision-one
  `mipi_dsi_brg_ll_reset()` is explicitly unsupported.  Its DPI-panel teardown
  disables the DPI clock and bridge before the next bus creation pulses the
  chip-level `RST_EN_DSI_BRG`.  The old early quiesce reset GDMA alone, leaving
  an active producer on the other side; the next boot could fill the bridge
  FIFO while the host remained unable to consume it.
- Implementation: after disabling and resetting GDMA, early quiesce now enables
  the DSI system/register clocks, releases a possibly inherited reset, disables
  the DPI clock and bridge in the Espressif teardown order, then pulses the
  chip-level DSI reset.  The reset is released before return because the same
  function serves B5's bounded shutdown and the following operation still
  accesses host registers.  No timing, framebuffer, panel-init, lane-rate or
  PSRAM variable changed.
- Source identity and build: dirty tree based on `83a78c86c5`, with only
  `dsi_scanout.c` plus this evidence/status update changed.  All generated
  kernel objects were deleted.  The build used
  `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360 P4_PANEL_PROBE=1
  P4_DSI_PROBE=1 P4_SCANOUT_TEST=1 P4_DSI_NONBURST=1
  P4_SCANOUT_TESTCARD=1 P4_BL_PERCENT=100
  P4_LDSCRIPT=ldscript-xip.lds`, contained both image-creation success markers,
  passed the SRAM-residency check and contained no `error:`.
- Artifact and flash safety: core 186,192 bytes, SHA-256
  `51c91a127a5860ba93a293645efc6c8a6e2cdd42bc37a9c407e11199aa19e1e3`.
  Only `ota_0` at `0x20000..0x4dfff` was erased and written; write-time hashing
  and an independent `verify-flash` both passed.  Bootloader, partition table,
  OTA metadata, BSP, storage and SD media were not written.
- Existing-fault recovery: the first boot of the candidate started from the
  persistent retained DSI state recorded in the preceding entry.  Without a
  power cycle or Vellum it identified 32 MB PSRAM at 200 MHz, selected a
  24-of-31 window, verified the mapped window, reported zero command
  timeouts/FSM recoveries, read DSI identity `93 65 04` and power mode `0x1c`,
  then moved the framebuffer at the required 69 MB/s with no payload error.
  FIFO depth varied throughout the complete 60-second run, which distinguishes
  a consuming bridge from the formerly pinned-full failure.
- Active-reset procedure and observation: a fresh first-byte capture reached a
  measured 69 MB/s scanout with moving SAR and sampled it through `t11`.  The
  capture ended without invoking bounded shutdown and a second
  `reset-and-log.py` immediately issued USB reset while that scanout was still
  active.  The second boot again found 32 MB at 200 MHz in one attempt,
  reported zero command timeouts/recoveries, verified the mapped window, read
  identity `93 65 04` and power mode `0x1c`, measured 69 MB/s, and showed a
  changing SAR and FIFO depth for all 60 samples before `scanout stopped,
  panel safe`.
- Acceptance passed here: recovery from the already-created retained DSI
  fault; one deliberately timed active warm reset; independent PSRAM, command
  path, panel-state, bridge-consumption and GDMA-motion evidence on the boot
  after reset; bounded safe shutdown afterwards.
- Remaining acceptance: this is one active warm-reset pair, not the full ten
  warm plus ten cold boots.  Full solid/checkerboard/corner coverage,
  dirty-rectangle coherency, 30-minute concurrent stress and the 100 MB/s gate
  remain open.
- Next safe step: run the remaining deterministic framebuffer-pattern and
  coherency cases before expanding this diagnostic path into the B6 buffered
  VSYNC handoff.

### 2026-08-26 - B5 deterministic framebuffer/coherency instrument transport passed

- State change: B5 remains `hardware partial`.  The missing deterministic
  solids/checker/corners/one-pixel/dirty-rectangle instrument now exists and
  completed one D1001 run without a transport fault.  The UART cannot establish
  that the panel showed no stale pixels, so the visual half of the
  dirty-rectangle gate remains explicitly open.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`.
- Implementation: `P4_SCANOUT_COHERENCY=1` keeps the proven single native
  RGB565 framebuffer and video configuration, but changes its contents on a
  fixed schedule: full red, green, blue, white and black fields; a 17-by-19
  checkerboard; four differently coloured and sized corner marks with four
  true one-pixel lines; then 23 moving 127-by-73 rectangles at changing odd
  positions, followed by the corner/line card again.  Checker cells, line
  positions and rectangle extents deliberately do not share cache-line or
  scanline boundaries.  Every changed phase completes the existing L1/L2
  writeback plus memory fence before its presentation marker is printed.
- Source identity and build: dirty tree based on `b98d673428`; all generated
  kernel objects were deleted before building with
  `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360 P4_PANEL_PROBE=1
  P4_DSI_PROBE=1 P4_SCANOUT_TEST=1 P4_DSI_NONBURST=1
  P4_SCANOUT_COHERENCY=1 P4_BL_PERCENT=100
  P4_LDSCRIPT=ldscript-xip.lds`.  The build passed the SRAM-residency check,
  contains both image-creation success markers and contains no `error:`.
- Artifact and flash safety: core 187,600 bytes, SHA-256
  `effe8c0884cf7e9608f19d802e4dce4637529dde1b3e467c99e38f9401b65e7c`.
  Only `ota_0` at `0x20000..0x4dfff` was erased and written.  Write-time hash
  verification and an independent `verify-flash` passed; bootloader, partition
  table, OTA metadata, BSP, flash storage and SD media were not written.
- Observed UART: first-byte normal SPI boot; 32 MB PSRAM at 200 MHz in one
  attempt; zero command timeouts/FSM recoveries; complete mapped-window check;
  DSI identity `93 65 04`; power mode `0x1c`; moving DMA and measured 69 MB/s.
  Phases 1 through 7 were presented at their scheduled seconds, phase 8 was
  presented once for the black reset plus 23 successive dirty rectangles, and
  phase 9 restored the corner/line card.  Across all 60 one-second samples the
  SAR moved, bridge FIFO occupancy varied, bridge raw status stayed zero and
  host `int1` stayed zero.  The run ended with `scanout stopped, panel safe`.
- Acceptance passed here: the instrument and its cache-writeback path execute
  on hardware; all requested framebuffer shapes are generated from PSRAM; no
  DMA stall, bridge underrun or DSI payload fault accompanies any update.
- Acceptance still open here: UART register evidence cannot see the panel and
  therefore cannot prove that the five solids were uniform, checker/corners
  were pixel-correct, one-pixel lines stayed one pixel wide, or old dirty
  rectangles disappeared completely.  Those points require a direct panel
  observation of this exact artifact.
- Remaining B5 gates after that observation: 30-minute concurrent SD/PSRAM/
  graphics stress, the 100 MB/s gate and ten cold plus ten warm boots.
- Next safe step: repeat this exact artifact for an observer, changing no
  variable, and record the panel result.  If clean, keep the final corner card
  as the static handoff image and move to the concurrent stress gate.

### 2026-08-26 - B5 post-video read hazard isolated; checker instrument corrected

- State change: B5 remains `hardware partial`.  This entry corrects two
  interpretations in the preceding B5 evidence: the active-reset repeat was
  intermittently destroyed by an optional diagnostic read rather than by the
  ordered teardown, and the first checkerboard did not tile the active area.
  The corrected path has now passed both its transport and direct-observation
  gates.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`.
- DSI correction evidence: an exact repeat of the 187,600-byte artifact from
  the preceding entry first passed, then failed after panel initialization.
  The failed run's post-video DCS 0x0A read got no reply and changed
  `CMD_PKT_STATUS` from the working `0x00050015` to `0x00050055`: exactly
  `GEN_RD_CMD_BUSY`.  The bridge FIFO then stayed at 1021 and GDMA SAR at
  `0x49e0e200`.  Clearing BTA and restarting video did not abort that read.
  Consequently `P4_DSI_POST_VIDEO_QUERY=1` now guards the disruptive query and
  is off by default.  Phase messages say `written back`, not `presented`,
  because UART establishes cache completion but cannot observe panel pixels.
- Reset-continuity retest: with only that query removed, core 187,360 bytes,
  SHA-256 `f1919f88d5eaae0f1772bfbb2b32741df8fafad6b1c0601fd36560c434af7707`,
  recovered directly from the retained busy state and completed at 71 MB/s.
  One immediate reset of the identical artifact also completed at 71 MB/s;
  both runs had moving SAR/FIFO through all phases, zero bridge raw status and
  zero host `int1`.  This supports the ordered quiesce, but does not replace the
  still-open ten-warm/ten-cold gate.
- Checker correction: the observer found one place where the 17-by-19 pattern
  appeared not to alternate.  That was a valid instrument defect:
  `800 % 17 == 1` and `1280 % 19 == 7`, leaving partial edge cells.  The checker
  now uses 25-by-20 cells, exactly 32 by 64 cells across 800 by 1280.  Each
  RGB565 cell is still 50 bytes wide and therefore crosses 64-byte cache-line
  boundaries without manufacturing a false edge seam.
- Corrected source/build identity: dirty tree based on `34ec315445`; all
  generated kernel objects were deleted before one complete build with
  `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360 P4_PANEL_PROBE=1
  P4_DSI_PROBE=1 P4_SCANOUT_TEST=1 P4_DSI_NONBURST=1
  P4_SCANOUT_COHERENCY=1 P4_BL_PERCENT=100
  P4_LDSCRIPT=ldscript-xip.lds`.  The log contains no `error:`, passed the SRAM
  residency check and contains both image-creation success markers.  Core
  187,360 bytes, SHA-256
  `cbea8f0811b9f208359443bb196dc0d7597b5d069bf212d2fe6e3a9bd7aa826f`.
- Flash safety: only `ota_0` at `0x20000..0x4dfff` was erased and written.
  Write-time hash verification and independent `verify-flash` passed;
  bootloader, partition table, OTA metadata, BSP, flash storage and SD media
  were not written.
- Corrected-run UART: first-byte normal SPI boot; 32 MB PSRAM at 200 MHz in one
  attempt; zero command timeouts/FSM recoveries; DSI identity `93 65 04`; no
  post-video DCS read; measured 71 MB/s.  All scheduled phases wrote back,
  every `t0..t59` sample had changing SAR and FIFO depth, bridge raw status and
  host `int1` stayed zero, and the run ended `scanout stopped, panel safe`.
- Direct panel observation: the exact same flashed artifact was reset and run
  again without a build or flash write.  The observer reported the corrected
  checkerboard `perfekt` and, while the dirty-rectangle sequence ran, `keine
  Artefakte`.  The repeat independently completed all `t0..t59` samples with
  moving SAR/FIFO, zero bridge raw status, zero host `int1` and the same bounded
  safe stop.
- Acceptance passed here: the exactly tiled checker and repeated dirty
  rectangles are visually clean from PSRAM; no stale rectangle or cache-line
  artefact was seen.  Transport remained free of stalls, underruns and payload
  errors, and a reset-retained read-busy state no longer contaminates the
  default B5 path.
- Acceptance still open here: explicit observation that the five solids are
  uniform, the four corner marks are geometrically correct and each one-pixel
  line remains one pixel wide; the 30-minute concurrent SD/PSRAM/graphics
  stress, 100 MB/s gate and ten cold plus ten warm boots.
- Next safe step: commit this validation correction, then run the concurrent
  stress gate without enabling `P4_DSI_POST_VIDEO_QUERY`.

### 2026-08-26 - B5 30-minute concurrent SD/PSRAM/scanout stress passed

- State change: B5 remains `hardware partial`, but its sustained concurrent
  stress acceptance point is closed on D1001 hardware.  The remaining B5 gates
  are explicit direct observation of the five solids, four corner marks and
  one-pixel lines, the DMA-based 100 MB/s decision and ten warm plus ten cold
  boots.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`; 121,942 MB SD card present and used read-only.
- Implementation: `P4_B5_CONCURRENT_STRESS=1` runs as an AFTERDOS resident so
  it reuses the hardware-verified `sdcard.device` after boot.  It excludes the
  fixed framebuffer at `0x49e0c000` from both the BSP package loader and Exec's
  PSRAM memory header, allocates separate PSRAM buffers, and each second reads
  128 sectors through `NSCMD_TD_READ64`, alternating LBA 8,388,608 and
  10,000,000 and checking their card-referenced FNV hashes.  It also writes and
  verifies every word of a changing 1 MB PSRAM pattern after L1D/L2 writeback
  and invalidation, while GDMA scans the framebuffer and moving odd-sized dirty
  rectangles continue for the entire run.  Any SD, hash or PSRAM mismatch
  aborts; a non-smoke build shorter than 1,800 seconds is rejected.
- Source/build identity: dirty tree based on `24f88bf523`; all generated kernel
  objects were deleted before building with
  `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_B5_CONCURRENT_STRESS=1 P4_BL_PERCENT=100
  P4_LDSCRIPT=ldscript-xip.lds`.  The flag supplied the proven panel, DSI,
  non-burst scanout and coherency defines and defaulted `P4_SCANOUT_SECS` to
  1,800.  The complete build contained no `error:`, passed the SRAM-residency
  check and printed both ESP32-P4 image-creation success markers.
- Artifact/flash safety: core 199,536 bytes, SHA-256
  `162609fcb86ed105653a02e22edd9b3036cf931bd2688b0480379a81664e3884`.
  Only `ota_0` at `0x20000..0x50fff` was erased and written; the write-time hash
  and an independent `verify-flash` passed.  Bootloader, partition table, OTA
  metadata, BSP, flash storage and SD media were not written.
- Harness validation: a separately built 60-second smoke image passed 60
  referenced SD reads, 60 cache-forced 1 MB PSRAM passes and 60 moving-scanout
  samples with zero failure.  Its `SMOKE PASSED` wording deliberately did not
  count toward the 30-minute gate.
- Recovery and discarded runs: one otherwise clean capture reached `t1786`
  before its 2,200-second host capture window expired; attempting to reopen the
  USB serial endpoint reset the board, so that run was rejected.  An identical
  repeat reached `t1511` before the host lost power and was also rejected.  A
  later boot exposed the known retained-state PSRAM failure (`no answer`, 30
  bounded timeouts/recoveries).  That boot is boot-cycle failure evidence, not
  a stress result.  One verified Vellum 1.12.0 boot restored vendor `0x0d`, X16
  32 MB PSRAM at 200 MHz and passed its memory test; the exact AROS core was
  then rewritten and independently verified.  The vendor recovery does not
  count as AROS recovery or toward any B5 acceptance point.
- Accepted D1001 run: normal first-byte SPI boot after recovery; 32 MB PSRAM at
  200 MHz in one attempt, zero command timeouts/FSM recoveries; SD boot from
  `SDCARD0P0`; framebuffer excluded from Exec at `0x49e0c000`; measured
  scanout 71 MB/s.  The log contains exactly sequential `t0..t1799`.  All
  1,800 samples have moving/wrapping SAR, `braw 0`, host `int1 0` and channel
  enable set; phase 8 was written back 1,782 times through the moving-update
  interval.  The final counters are `SD reads 1800 x 128 sectors` (112.5 MiB),
  `PSRAM 1 MB passes 1800`, `failures 0`, followed by `scanout stopped, panel
  safe` and a return to the Shell.
- Acceptance passed here: at least 30 minutes of simultaneous display DMA,
  real read-only SD traffic, forced external-PSRAM writes/reads and moving
  framebuffer updates with no detected underrun, payload, storage or memory
  error, followed by bounded safe teardown.
- Remaining risk: reset retention can still leave PSRAM unreachable and is not
  repaired merely by this stress pass.  The ten-warm/ten-cold boot matrix must
  include and resolve that failure rather than hiding it behind vendor
  recovery.  The explicit visual solids/corners/one-pixel and 100 MB/s gates
  are also still open.
- Next safe step: commit this functional stress increment, then close the
  explicit visual pattern observations or begin the controlled boot matrix;
  keep the SD read-only through graphical boot.

### 2026-08-26 - B5 first explicit corner observation failed; visual gate separated

- State change: B5 remains `hardware partial`.  The five solid and checker
  phases transported cleanly, but the first explicit corner observation is a
  failed gate: the observer reported that the supposed corner marks appeared
  paired together at the middle of the display's short edges rather than in
  the four physical corners.  This result is not accepted as correct geometry.
- Hardware / revision: Seeed reTerminal D1001, ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`; SD present and untouched by the visual harness.
- Source/artifact: clean commit `b32b2dbd0e`; core 187,456 bytes, SHA-256
  `393fcb66b91f13f8331a4660555ff66992ca6f4918775f64423a56fb983269a0`,
  built with `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_PANEL_PROBE=1 P4_DSI_PROBE=1 P4_SCANOUT_TEST=1
  P4_DSI_NONBURST=1 P4_SCANOUT_COHERENCY=1 P4_SCANOUT_SECS=60
  P4_BL_PERCENT=100 P4_LDSCRIPT=ldscript-xip.lds`.  The targeted port build
  exited zero, contained no `error:`, passed the SRAM-residency check and
  printed both ESP32-P4 image success markers.  Only `ota_0` at
  `0x20000..0x4dfff` was written and independently verified.
- UART: normal first-byte boot; 32 MB PSRAM at 200 MHz in one attempt with zero
  command timeouts/recoveries; DSI identity `93 65 04`; measured 71 MB/s;
  moving SAR/FIFO and zero bridge raw/host `int1` through all `t0..t59`; bounded
  panel-safe stop and Shell return.  Transport therefore does not explain the
  visual placement.
- Code audit: `scanout_corners_and_lines()` really writes its four coloured
  rectangles at framebuffer coordinates `(0,0)`, `(799,0)`, `(0,1279)` and
  `(799,1279)` with asymmetric extents.  `P4_PANEL_VMUL` is 1.  The observation
  therefore either exposes a physical-coordinate mapping not visible in the
  periodic checker or confusion between corner blocks and the four full-screen
  line intersections; it does not justify silently moving the coordinates.
- Instrument correction: `P4_B5_VISUAL_GATE=1` now provides one reproducible
  60-second observer schedule: long separate RGB/white/black solids, exact
  checker, four large asymmetric coloured quadrants, corner blocks alone for
  ten seconds, one-pixel lines alone for ten seconds, then both together.  The
  existing sustained-stress schedule is unchanged.
- Acceptance passed/failed: transport and safe teardown passed; physical
  corner placement failed; the solids, corner geometry and one-pixel width are
  not closed until the separated exact artifact is directly observed.
- Next safe step: build and run only `P4_B5_VISUAL_GATE=1`, record the physical
  quadrant order, then assess corner-only and line-only phases independently.

- Separated-run artifact and direct evidence: core 187,632 bytes, SHA-256
  `0b47ded30527e5fc6551e15dcd6495bf5b8a2fa1ff857e79de665cd2e630b51a`,
  built with `P4_B5_VISUAL_GATE=1` plus the 200-MHz PSRAM and 360-MHz CPU
  settings above.  The targeted build exited zero with no `error:`, passed the
  SRAM check and printed both image success markers; only `ota_0` at
  `0x20000..0x4dfff` was written and independently verified.  UART again
  covered every `t0..t59`, all with zero bridge/host error, followed by the
  bounded safe stop.
- Photographic observation: `IMG_0925.JPG` shows the requested framebuffer
  quadrants not as four physical quadrants but as top `green/red/green` and
  bottom `yellow/blue/yellow`.  `IMG_0926.JPG` shows the upper green/right
  framebuffer corner directly left of the upper red/left corner, and the lower
  yellow/right corner directly left of the lower blue/left corner, all near
  the middle of the physical short edges.  `IMG_0927.JPG` shows the isolated
  x=599 and x=173 vertical lines at the corresponding wrapped physical
  positions; `IMG_0928.JPG` confirms the same mapping when lines and corners
  are combined.  This is a failed coordinate gate, not an observer mix-up.
- Mapping: all four photographs agree with one linear cyclic displacement of
  approximately 550 pixels in an 800-pixel row.  If physical pixel zero reads
  framebuffer x=550, framebuffer x=739..799 (green) appears at physical
  x=189..249 and x=0..36 (red) immediately follows at x=250..286; the x=599
  and x=173 lines then appear at physical x=49 and x=423.  That predicts every
  photographed feature without scaling, reflection or a changed stride.
- Reference comparison and correction under test: the automatic channel-register
  reload used by AROS has no frame boundary.  Espressif's
  `esp_lcd_panel_dpi.c` instead marks its one full-frame list item last; the DMA
  clears VALID after consuming it, and the full-transfer ISR restores VALID,
  selects the item and re-enables the channel.  AROS now follows that exact
  mechanism through the existing CLIC path, reports completed ISR-rearmed
  frames and latched DMA faults, and disables the source before safe teardown.
  A fixed 550-pixel source bias was deliberately rejected because it would
  hide rather than repair a start phase that can differ on another boot.
- Next safe step: build and run the same separated visual artifact.  First
  require a steadily increasing ISR frame count with zero latched faults and
  clean bridge/host status; then directly observe the quadrant, corner-only and
  line-only phases again before accepting the coordinate gate.
- First re-arm artifact attempt: core 188,224 bytes, SHA-256
  `deec3ddfc8258e499c26fe70949ae3c709bf6b7e19123703db2bcc179dfe3f1b`.
  Its clean first-byte boot reached video with no bridge or host error, but the
  new diagnostic reported `frames 0 faults 0x00000010`, a stopped channel and
  zero subsequent scanout rate.  Bit 4 is `DST_TRANSCOMP`, a normal companion
  to full-transfer completion rather than a DMA fault; the deliberately
  fail-closed ISR therefore suppressed its first re-arm.  Source and destination
  transfer-complete bits are now classified with block/dma-done and disabled as
  normal terminal status.  This run proves the interrupt and fail-closed path,
  not coordinate correctness, and is excluded from visual acceptance.
- Second re-arm artifact attempt: core 188,224 bytes, SHA-256
  `619e8059ff92c651fe2b0a9507d648fc52b0a8cc5fac50a9cbdcdcc6c47c62d4`.
  The first frame completed and reached the ISR (`frames 1 faults 0`), which
  restored VALID and re-enabled the channel, but the DMA then reported
  `LLI_INVALID` and stopped before a second frame.  Espressif does not write its
  internal-SRAM list item through the normal pointer: on P4 it first writes
  back and invalidates the cached address, then accesses the item only through
  `CACHE_LL_L2MEM_NON_CACHE_ADDR`, the normal address plus `0x40000000`; the
  normal address is retained only in the DMA's LLP.  AROS had incorrectly
  described internal SRAM as coherent and rewrote VALID through its cached
  address.  The item now follows IDF's alias rule exactly, and LLI_INVALID is
  propagated to the handler as well as retained in the diagnostic.  This run
  is excluded from visual acceptance.
- Third re-arm artifact and UART acceptance: dirty tree based on clean commit
  `b32b2dbd0e`; core 188,288 bytes, SHA-256
  `7e1484a08dbed07366495e535ce64ab80a5db1b21c6d904480a7f4c91d5272ad`,
  built with `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_B5_VISUAL_GATE=1 P4_BL_PERCENT=100 P4_LDSCRIPT=ldscript-xip.lds`.
  The targeted build exited zero, contained no `error:`, passed the SRAM
  residency check and printed both ESP32-P4 image success markers.  Only
  `ota_0` at `0x20000..0x4dfff` was erased and written; write-time hashing and
  independent `verify-flash` passed.
- Accepted transport evidence for that artifact: normal first-byte SPI boot;
  32 MB PSRAM at 200 MHz in one attempt with zero command timeouts/recoveries;
  DSI identity `93 65 04`; 71 MB/s measured scanout.  The completion interrupt
  had already re-armed five frames at the initial report.  Every `t0..t59`
  sample kept the channel enabled, SAR and bridge depth moving, bridge raw and
  host `int1` zero, and DMA faults zero while the completed-frame count rose
  monotonically from 18 to 2,078.  The run ended `scanout stopped, panel safe`,
  then found the 121,942 MB SD card read-only and reached the Shell.
- Acceptance passed/open: explicit frame-boundary re-arm through the
  non-cacheable descriptor alias and the complete UART transport/safe-teardown
  gate pass.  Direct observation of the quadrants, separated corner marks and
  one-pixel lines for this exact artifact remains open; the earlier photos do
  not transfer to a changed artifact.  Because this replaces the transport
  mechanism, the earlier 30-minute stress result must also be repeated before
  it can validate the final implementation.
- Next safe step: record the observer's result for the exact third artifact.
  If its geometry passes, rebuild and repeat the 30-minute concurrent
  SD/PSRAM/scanout stress with the ISR-rearmed transport before changing B5's
  overall state.
- Third-artifact direct observation failed: the exact artifact above was reset
  and run again without a build or flash write.  It independently completed
  `t0..t59`, rose from 18 to 2,078 completed frames with zero DMA faults,
  retained zero bridge raw/host `int1` and stopped safe.  Nevertheless,
  `IMG_0929.JPG` again shows the requested quadrants physically as top
  `green/red/green` and bottom `yellow/blue/yellow`; `IMG_0930.JPG` again shows
  the two upper and two lower corner marks paired near the middle of the short
  edges; `IMG_0931.JPG` and `IMG_0932 2.JPG` show the same wrapped line mapping
  alone and combined.  The explicit coordinate gate therefore remains failed.
- Refined cause under test: a full-transfer interrupt establishes DMA frame
  boundaries but does not by itself prove the correct GDMA route into the
  bridge.  Comparing every descriptor field against `dw_gdma_lli_config_transfer()`
  found one real mismatch: ESP-IDF selects GDMA master 1 for a PSRAM source and
  master 0 for the MIPI-DSI destination.  AROS left both SMS and DMS at reset
  zero.  SMS now selects the memory master while DMS remains the MIPI master;
  this is an exact reference correction, not a compensating 550-pixel bias.
- Next safe step: rebuild and rerun the same visual artifact.  Require the
  existing rising-frame/zero-fault transport evidence again, then compare the
  isolated quadrant, corner and line phases before accepting or rejecting the
  master-port correction.
- Memory-master artifact and transport evidence: core 188,288 bytes, SHA-256
  `8d46e744921cdb5fdb2ef785e9942aac5c1f490fea6cd2fd9207ef0df3ae74fc`,
  built under the same visual-gate flags.  The build exited zero, contained no
  `error:`, passed the SRAM-residency check and printed both image-creation
  success markers.  Only `ota_0` at `0x20000..0x4dfff` was erased and written;
  write-time hashing and independent `verify-flash` passed.  Normal first-byte
  boot found PSRAM at 200 MHz in one attempt with zero timeouts/recoveries.
  All `t0..t59` samples again kept the channel enabled, DMA faults and
  bridge/host error status zero, with completed frames increasing from 18 to
  2,078; safe stop, read-only SD discovery and Shell return followed.  Direct
  observation of this exact artifact is pending and remains the deciding
  result for the master-port hypothesis.
- Memory-master direct observation failed: `IMG_0933.JPG` through
  `IMG_0936.JPG` show no geometric change.  The quadrants remain physically
  `green/red/green` over `yellow/blue/yellow`; the corner marks remain paired
  near the short-edge centres, and the isolated/combined lines retain the same
  wrapped positions.  The corrected master selection remains because it is
  required by the reference, but it did not cause the coordinate fault.
- Working-firmware register control: verified Vellum 1.12.0, core SHA-256
  `dd37407b98204fd6d236031d841275424a8b353d90db6d290aff77dcc43945fa`,
  was written and independently verified only in `ota_0`.  It identified the
  same panel as `93 65 04`, initialized 800x1280 RGB565 at 40 MHz and displayed
  normally.  A read-only USB-JTAG snapshot briefly halted and then resumed both
  cores.  Every configured bridge register relevant to scanout matches AROS:
  timing `05000540/0004001e` and `03200370/00140014`, frame words `0003e800`,
  pixel type 2, flow `10`, frame interval `20002409`, threshold `300`, and the
  same credit, request, auxiliary and control values.  The remaining measured
  host difference is Vellum's 1000-Mbit/s lanes and `VID_MODE_CFG 0000ff02`
  versus AROS's 1500-Mbit/s lanes and `0000bf00`: Vellum uses burst video with
  sync pulses and frame acknowledge while the current visual gate forces
  non-burst without acknowledge.  The GDMA registers could not be read by
  OpenOCD on this revision, so no unsupported equality is claimed there.
- Exact-reference host test under construction: stop forcing non-burst in the
  separated visual gate, build it at 1000 Mbit/s with frame acknowledge, and
  rerun the same patterns on the corrected descriptor path.  This specifically
  tests whether the host's per-line video packet boundary, not the bridge's
  framebuffer counter, is carrying the 550-pixel phase error.
- Exact-reference host artifact and technical run: dirty tree based on clean
  commit `b32b2dbd0e`; core 188,288 bytes, SHA-256
  `43f8bad4e28dabaab060964935b4cdb49afa60d444865ccf7fa7a551bec003df`,
  built with `P4_HEADLESS_BOOT=1 P4_PSRAM_MHZ=200 P4_CPU_MHZ=360
  P4_B5_VISUAL_GATE=1 P4_BL_PERCENT=100 P4_LANE_MBPS=1000
  P4_DSI_FRAME_ACK=1 P4_LDSCRIPT=ldscript-xip.lds`.  The valid targeted build
  exited zero, contained no `error:`, passed the SRAM-residency check and
  printed both ESP32-P4 image success markers.  An earlier invocation without
  Espressif's Python environment reached the link but failed image creation
  because `esptool` was absent; it is explicitly excluded.  Only `ota_0` at
  `0x20000..0x4dfff` was erased and written, restoring AROS after the read-only
  Vellum register comparison; write-time hashing and independent
  `verify-flash` both passed.
- D1001 evidence for that artifact: normal first-byte boot on ESP32-P4 v1.3,
  32 MB PSRAM at 200 MHz in one attempt with zero command timeouts/recoveries,
  panel identity `93 65 04`, and the requested exact host state was read back:
  1000 Mbit/s per lane, `VID_MODE_CFG 0x0000ff02`, packet 800, HSA/HBP 63 and
  HLINE 2750.  The bridge timing remained the reference-matching 40-MHz
  800x1280 state.  Through every `t0..t59` sample the channel stayed enabled,
  SAR and FIFO depth moved, bridge raw status and DMA faults remained zero,
  and ISR-rearmed frames rose monotonically from 18 to 2,078 before the bounded
  `scanout stopped, panel safe` teardown.  The read-only 121,942 MB SD card was
  subsequently discovered and the system reached the Shell.
- Host-status qualification: `INT_ST1` bit 7 (`DPI_PLD_WR_ERR`) remained
  `0x00000080` and continued to reassert after clear.  Therefore this is a
  successful exact-configuration and bounded-transport run, but not a
  zero-host-error acceptance.  It must not be described as a completely clean
  B5 transport result merely because Vellum configures the same video mode.
- Acceptance/open: the exact Vellum host configuration is now proven active on
  AROS; DMA frame continuity, bridge status, PSRAM, safe teardown and read-only
  SD discovery passed.  The direct coordinate observation of this exact
  artifact is pending.  It decides whether host video packet mode affects the
  550-pixel horizontal phase; no coordinate correction or B5 state change is
  accepted before that observer result is recorded.
- Direct observation failed: the exact-reference artifact was reset and shown
  again with the timed solids/checker, quadrant, corner-only, line-only and
  combined schedule.  The observer reported that the display remained black
  throughout.  Frame counts and moving DMA state therefore do not establish
  visible panel acceptance in this mode.  The exact Vellum host register value
  is not a usable AROS result as a unit, and the persistent
  `DPI_PLD_WR_ERR` remains relevant to the failure.
- Next safe step: remove only frame acknowledge while retaining 1000-Mbit/s
  burst video; this
  separates the ACK/BTA failure from line-packet mode.  If visible output
  returns but the displacement persists, reject burst/line timing as the
  coordinate cause and compare the working firmware's DMA start/restart
  sequence or panel-side horizontal addressing without introducing a fixed
  source bias.  Any passing mode still requires the 30-minute final stress.
- Burst-without-ACK isolation artifact: core 188,288 bytes, SHA-256
  `a397c5994cad90aca38d8d7624816ec7019a8e9c2a2a251338f41907537fa7b5`,
  built from the same dirty tree with the exact-reference flags except
  `P4_DSI_FRAME_ACK` omitted.  All 198 generated kernel `.o`/`.d` files were
  removed before the flag change.  The targeted build exited zero, contained
  no `error:`, passed SRAM residency and printed both image-success markers.
  Only `ota_0` at `0x20000..0x4dfff` was written; write-time hashing and the
  subsequent independent verification passed.
- D1001 technical result: the requested isolated mode read back as 1000
  Mbit/s, `VID_MODE_CFG 0x0000bf02`, packet 800, HSA/HBP 63, HLINE 2750 and no
  frame acknowledge.  Every `t0..t59` sample retained moving SAR/FIFO, zero
  bridge raw status and zero DMA faults while ISR frames increased from 18 to
  2,078.  `DPI_PLD_WR_ERR` still reasserted as `INT_ST1 0x00000080`, proving
  that frame acknowledge alone did not create that host error.  The run ended
  with the bounded panel-safe stop and reached the Shell through the read-only
  SD discovery path.
- Acceptance/open: build, exact-mode readback, DMA continuity, safe teardown
  and read-only SD discovery passed.  Direct observation of whether visible
  output returned, and if so whether its coordinate phase changed, remains
  pending; no visual or B5 acceptance is inferred from the counters.
- Direct observation failed: the unchanged burst-without-ACK artifact was reset
  and shown again; the observer reported that every phase remained completely
  black.  Removing frame acknowledge therefore did not restore visible output,
  and ACK/BTA is not the discriminator between Vellum and AROS.  The next
  single-variable split retains 1000 Mbit/s but restores the previously visible
  non-burst mode; this distinguishes lane rate from burst packetization.
- 1000-Mbit/s non-burst isolation artifact: core 188,288 bytes, SHA-256
  `93e114fa2acba49e9569fec4e2b744692ce385368d067787fa14c0af117adcee`,
  built from the same dirty tree with `P4_DSI_NONBURST=1`, 1000-Mbit/s lanes
  and no frame acknowledge after all 198 generated kernel `.o`/`.d` files were
  removed.  The targeted build exited zero, contained no `error:`, passed SRAM
  residency and printed both image-success markers.  Only `ota_0` at
  `0x20000..0x4dfff` was written, and both write-time hashing and independent
  verification passed.
- D1001 mode evidence: the visual rerun read back 1000 Mbit/s and
  `VID_MODE_CFG 0x0000bf00` with packet 800, HSA/HBP 63 and HLINE 2750.  In
  contrast to both burst artifacts, host `INT_ST1`, bridge raw status and DMA
  faults remained zero through the captured visual interval while frame count,
  SAR and FIFO depth advanced.  Direct visibility and coordinate placement are
  pending the observer result; the counters alone do not close that gate.
- Direct observation failed: the observer saw no image in any phase of the
  1000-Mbit/s non-burst rerun.  Together with the two black 1000-Mbit/s burst
  artifacts, this isolates lane-rate-dependent AROS initialization or runtime
  state as sufficient to suppress visible output; frame acknowledge and burst
  mode are not required for the black result.  Vellum still proves that the
  panel and board can operate at 1000 Mbit/s, so this is an AROS sequencing or
  unmeasured-state difference rather than a panel capability limit.
- Next safe step: run the orthogonal 1500-Mbit/s burst/no-ACK control.  A
  visible result isolates 1000-Mbit/s state as the black-screen cause; a black
  result shows burst packetization is independently unsupported by the current
  AROS sequence.  Neither result by itself repairs the still-open 550-pixel
  coordinate phase in the known-visible 1500-Mbit/s non-burst mode.
- 1500-Mbit/s burst control artifact: core 188,288 bytes, SHA-256
  `e3b3f0af3797f3fbaa895fc63c2f28e855eb34804443ee6d453e89b76f6eea6e`,
  built from the same dirty tree at 1500 Mbit/s with burst video and no frame
  acknowledge after all 198 generated kernel `.o`/`.d` files were removed.
  The targeted build exited zero, contained no `error:`, passed SRAM residency
  and printed both image-success markers.  Only `ota_0` at
  `0x20000..0x4dfff` was written; write-time hashing and independent
  verification passed.
- D1001 mode evidence: the visual rerun read back 1500 Mbit/s and
  `VID_MODE_CFG 0x0000bf02`, packet 800, HSA/HBP 94 and HLINE 4125.  Frame
  count, SAR and FIFO depth advanced with zero bridge raw status and zero DMA
  faults through the captured interval, while burst mode again continuously
  reasserted `DPI_PLD_WR_ERR`.  Direct visibility remains pending the observer
  result and is the purpose of this control.
- Direct observation failed: the unchanged 1500-Mbit/s burst artifact was run
  again and the observer reported that every phase remained completely black.
  The controlled matrix is therefore conclusive for current AROS: both tested
  burst modes are black at 1000 and 1500 Mbit/s; 1000-Mbit/s non-burst is also
  black; only 1500-Mbit/s non-burst without frame acknowledge has produced
  visible output.  Static equivalence to Vellum's host registers is insufficient
  because an unmeasured or sequential driver state differs.
- Consequence: restore 1500-Mbit/s non-burst/no-ACK as the visual baseline and
  do not use the black exact-reference modes to assess coordinate geometry.
  The Vellum comparison remains useful evidence that 1000-Mbit/s burst is
  physically supported, but the 550-pixel phase must now be pursued in the
  known-visible AROS path, especially DMA/bridge start ordering and state that
  was not readable through OpenOCD.
- Visible baseline restored and reconfirmed: the visual gate now selects
  non-burst by default unless an explicit `P4_DSI_BURST=1` test override is
  supplied.  After removing all 198 generated kernel `.o`/`.d` files, the
  rebuilt 1500-Mbit/s non-burst/no-ACK image was byte-for-byte the already
  documented visible artifact, 188,288 bytes with SHA-256
  `8d46e744921cdb5fdb2ef785e9942aac5c1f490fea6cd2fd9207ef0df3ae74fc`.
  The build exited zero with no `error:`, passed SRAM residency and printed both
  image-success markers; only `ota_0` was written and independently verified.
  Direct observation confirmed visible colour phases again, but also confirmed
  the same horizontal coordinate displacement.  The host-mode/lane matrix did
  not change the phase.
- Refined blocker: lane rate controls current AROS visibility and burst mode is
  independently black, but neither is the coordinate cause.  Static bridge
  equality, host-mode substitutions, GDMA PSRAM-master selection, and explicit
  full-frame LLI rearm have all failed to move the approximately 550-pixel
  displacement.  The next comparison must cover temporal enable/reset order
  and DMA state not readable through OpenOCD, not another static timing value.
- Exact GDMA audit found one further reference mismatch now isolated for test:
  `dw_gdma_new_link_list()` writes LMS=memory into every descriptor LLP even
  when the one last item has a null next address, and
  `dw_gdma_channel_use_link_list()` also writes LMS=memory into the channel LLP.
  AROS supplied the correct aligned L2MEM address but left LLP bit 0 clear,
  selecting the MIPI master for link-list fetches.  Both LLP locations now set
  LMS=memory exactly as ESP-IDF does; the framebuffer source and bridge
  destination routing are otherwise unchanged.  This is a single exact-driver
  correction under test on the known-visible 1500-Mbit/s non-burst/no-ACK
  configuration, not a fixed coordinate offset.
- Next safe step: build, write and independently verify only `ota_0`, then
  require rising ISR frame counts and zero DMA/bridge/host errors before the
  observer compares the same quadrant, corner and line phases.  If the phase
  is unchanged, retain the reference-correct LMS setting and next isolate the
  earlier ESP-IDF LPCLK-auto initialization before panel commands.
- LLP-memory-master artifact and technical evidence: dirty tree based on clean
  commit `b32b2dbd0e`; core 188,288 bytes, SHA-256
  `86f6607c605f44a04740d6729fbc853464db9aec057774913c590206631a0648`,
  built with the known-visible 200-MHz PSRAM, 360-MHz CPU, 1500-Mbit/s lane and
  visual-gate settings.  The targeted build exited zero, contained no
  `error:`, passed the SRAM-residency check and printed both ESP32-P4 image
  success markers.  Only `ota_0` at `0x20000..0x4dfff` was erased and written;
  write-time hashing and the independent `verify-flash` digest both passed.
- D1001 transport result: normal first-byte boot on ESP32-P4 v1.3, 32 MB PSRAM
  at 200 MHz in one attempt with zero command timeouts/recoveries, and panel
  identity `93 65 04`.  The consumed one-item list reported LLP `0x00000001`,
  directly confirming LMS=memory while its null next address remained zero.
  All `t0..t59` samples kept the channel enabled, SAR and bridge FIFO moving,
  bridge raw status, host `int1` and DMA faults zero; ISR-rearmed frames rose
  monotonically from 18 to 2,078.  The bounded panel-safe stop, read-only
  121,942 MB SD discovery and Shell return followed.  Direct coordinate
  observation of this exact artifact remains pending; transport counters alone
  do not close the visual gate.
- LLP-memory-master visual result: the unchanged artifact was reset for a
  second observer run.  It repeated the complete `t0..t59` sequence, again
  reached 2,078 ISR-rearmed frames with zero DMA/bridge/host errors, stopped
  the panel safely and discovered the SD read-only.  The observer reported the
  same horizontal displacement.  Correct LLP master selection is retained as
  an exact reference requirement but is rejected as the coordinate cause.
- Historical test, later source-audit correction: this test was made after an
  incorrect reading of ESP-IDF's clock-lane helper.  The artifact placed the
  clock lane in AUTO state before any panel command (`LPCLK_CTRL=3`), whereas
  `esp_lcd_new_dsi_bus()` actually requests LP (`LPCLK_CTRL=0`) and the DPI
  start path requests AUTO only after DMA is armed and video mode is enabled.
  The run remains valid evidence that *early AUTO* did not alter the visible
  displacement, but it is not an exact-reference correction.
- Early-LPCLK artifact and D1001 transport evidence: core 188,288 bytes,
  SHA-256
  `69c94efe633bad3da4cb2f9b9f6cee91a43ebc61d201a42bece4d406bf41fe71`.
  The targeted build exited zero, contained no `error:`, passed the SRAM
  residency check and printed both ESP32-P4 image success markers.  Only
  `ota_0` at `0x20000..0x4dfff` was erased and written; write-time hashing and
  independent `verify-flash` passed.  Normal first-byte boot found 32 MB PSRAM
  at 200 MHz in one attempt with zero timeouts/recoveries and panel identity
  `93 65 04`.  The final host readback remained the requested
  `LPCLK_CTRL=3`, 1500-Mbit/s non-burst/no-ACK state.  All `t0..t59` samples
  retained a moving DMA/bridge pipeline with zero bridge raw status, host
  `int1` and DMA faults while ISR-rearmed frames increased from 18 to 2,078.
  Bounded panel-safe stop, read-only SD discovery and Shell return passed.
  Direct observation remains the deciding evidence; no coordinate result is
  inferred from the technically clean run.
- Early-AUTO direct observation failed: the unchanged artifact was reset for
  a second observer run and again completed `t0..t59`, 2,078 frames, zero
  DMA/bridge/host errors, safe teardown and read-only SD discovery.  The
  observer reported that the horizontal displacement was unchanged.  Early
  AUTO clock-lane control was therefore not the coordinate cause.  A later
  source audit established that it does not match ESP-IDF and it is removed by
  the first-frame host-acceptance test below.  The next audit at that point was
  restricted to the exact bridge/DMA enable, configuration-update and
  FIFO-reset sequence.
- Reference call-order audit found a more specific difference now isolated for
  test.  The working JD9365 wrapper first sends its vendor command table, then
  calls the underlying DPI panel's `init`, which arms GDMA, enables host video
  and enables bridge DPI output; only after those three steps return does its
  separate `disp_on_off(true)` send the valid parameterless DCS `0x29`.  AROS
  sent that valid display-on command before it configured or started any pixel
  producer.  The vendor table still contains its earlier one-parameter `0x29`,
  but DCS defines no such form and both implementations later send the valid
  parameterless transaction.
- Correction under test: for B5 only, panel initialization now defers the
  parameterless `0x29` until immediately after GDMA, video mode and bridge feed
  are active, exactly matching the working wrapper's call order.  A failure
  stops scanout and returns the panel safe before the backlight is enabled.
  No timing, host-mode, bridge or DMA register value changes in this test.  A
  changed physical phase would identify panel enable relative to the existing
  video frame as the missing synchronisation event.
- Display-on-after-video artifact and result: core 188,432 bytes, SHA-256
  `d655c2d41255191cc35814af36957e12ead14ee6a236f0d90903d00238ba19b7`.
  The targeted build exited zero, contained no `error:`, passed SRAM residency
  and printed both image-success markers.  Only `ota_0` at
  `0x20000..0x4efff` was erased and written; write-time hashing and independent
  verification passed.  D1001 boot confirmed `display on after video start`,
  then completed `t0..t59` with moving SAR/FIFO, zero bridge raw/host `int1` and
  DMA faults, and 2,079 ISR-rearmed frames before safe stop and read-only SD
  discovery.  The observer reported the same displacement.  The corrected
  order remains because it exactly matches the working wrapper, but it is not
  sufficient to repair the 1500-Mbit/s non-burst phase.
- Next controlled retry: the earlier 1000-Mbit/s burst/frame-ACK artifact was
  black before three exact-reference corrections landed: LLP LMS=memory,
  AUTO clock-lane state before panel commands, and parameterless display-on
  after the running DPI stream.  Rebuild the exact Vellum host configuration
  with all three corrections.  Visible output would move B5 back onto the
  known-good host mode and make its geometry decisive; another black result
  keeps the non-burst coordinate blocker open without weakening the new
  reference-correct state.
- Corrected exact-Vellum artifact and technical result: after deleting all 198
  generated kernel `.o`/`.d` files for the flag change, core 188,432 bytes,
  SHA-256
  `bde29610160a9935bcc0454011ebd63ab8a7f8357c576b2ec5cd49d3e3ef77ce`,
  was built at 1000 Mbit/s with burst-with-sync-pulses and frame acknowledge.
  The targeted build exited zero, contained no `error:`, passed SRAM residency
  and printed both image-success markers.  Only `ota_0` at
  `0x20000..0x4efff` was written; write-time hashing and independent
  verification passed.
- D1001 readback confirmed the requested exact host state:
  `VID_MODE_CFG=0x0000ff02`, packet 800, HSA/HBP 63, HLINE 2750, early
  `LPCLK_CTRL=3`, LLP LMS=memory and display-on after video start.  DMA frames
  rose from 18 to 2,079 with zero DMA faults and bridge raw status, but host
  `DPI_PLD_WR_ERR` continuously reasserted as `INT_ST1=0x00000080` and the
  data lanes remained in stop state as PHY `0x15b9`.  Safe stop and read-only
  SD discovery passed.  Direct observation confirmed that the display stayed
  completely black.  The corrected exact-reference setup therefore still
  reproduces the earlier 1000-Mbit/s burst/frame-ACK failure; it does not
  provide a usable geometry baseline on AROS.
- Next controlled test: restore the technically clean, visible 1500-Mbit/s
  non-burst/no-ACK setup, but build the asymmetric quadrant image completely
  before DMA, host video or bridge output starts and never modify the
  framebuffer afterwards.  If that immutable image is displaced, the defect
  is already present in initial DMA/bridge/panel framing; if it is correctly
  placed, later writes or their visibility to the scanout path are moving the
  apparent origin.  `P4_B5_STATIC_PRELOAD=1` makes this distinction
  reproducible without adding a compensating source offset.
- Immutable-preload artifact built: after deleting all 198 generated kernel
  `.o`/`.d` files, the 1500-Mbit/s non-burst/no-ACK build produced a
  188,112-byte core with SHA-256
  `0e9773ebcea0dcd276683cda922a9cfca5d51cfa04b488970be365d69f70bb0e`.
  The targeted kernel build exited zero, contained no `error:`, passed SRAM
  residency and printed both ESP32-P4 image-success markers.  Flashing has not
  occurred: every esptool connection attempt failed before erase or write.
  A subsequent host-side reset of the unresponsive USB-JTAG/serial device
  timed out and detached it from macOS, so the test is blocked only until the
  USB cable is re-enumerated; neither flash nor SD media was changed.
- Immutable-preload D1001 result: after USB re-enumeration, only `ota_0` at
  `0x20000..0x4dfff` was written and both the write-time hash and independent
  `verify-flash` passed.  The 60-second run retained moving SAR and bridge FIFO
  depth, zero host payload errors, zero bridge raw status and zero DMA faults;
  ISR-rearmed frames rose from 11 to 2,006 before panel-safe stop.  Read-only
  SD discovery and Shell return passed.  Direct photo `IMG_0937.JPG` shows the
  immutable quadrant image still cyclically wrapped within every physical
  row: each expected half-width colour appears as a central span plus the
  opposite colour at both short edges.  The horizontal midpoint remains
  coherent, so this is the same constant horizontal frame phase, not tearing.
  Cache writeback and any modification of a live framebuffer are therefore
  excluded as causes.
- Exact start-order mismatch isolated: Espressif configures the bridge, writes
  global `DSI_EN`, and only then commits `DPI_CFG_UPD`.  AROS committed the
  staged DPI values before enabling the bridge.  Its later live update can
  change values without necessarily restarting the internal line phase.  The
  order is now corrected to enable-then-update; the same immutable pattern is
  the acceptance instrument, with no source-coordinate compensation.
- Enable-before-update artifact and technical result: after deleting all 198
  generated kernel `.o`/`.d` files, the 1500-Mbit/s non-burst/no-ACK static
  build produced a 188,112-byte core with SHA-256
  `41be66573c8bd3004b07d61b38448ae3e6207965a2fd6567a840aca8d0c5b294`.
  The build exited zero, contained no `error:`, passed SRAM residency and
  printed both image-success markers.  Only `ota_0` at
  `0x20000..0x4dfff` was written; write-time and independent verification
  passed.  D1001 then completed `t0..t59` with zero host payload errors,
  bridge raw status and DMA faults, while frames rose from 11 to 2,006 before
  panel-safe stop.  Read-only SD discovery and Shell return passed.  Direct
  geometry observation is pending and remains the deciding evidence.
- Enable-before-update direct observation failed: the unchanged artifact was
  reset for a second observer run and again completed `t0..t59`, reached 2,006
  ISR-rearmed frames with zero host payload errors, bridge raw status and DMA
  faults, then stopped safely and discovered the SD card read-only.  The
  observer reported the same horizontal displacement (`wie gehabt`).  The
  corrected enable-before-update order is retained because it matches
  ESP-IDF, but it is rejected as the coordinate cause.  The immutable-source
  result now restricts the blocker to producer/consumer start phase or another
  unmeasured GDMA/bridge state; the next diagnostic must measure those state
  transitions rather than alter a static timing or add a coordinate offset.
- Start-transition diagnostic prepared.  Three attempted builds produced no
  image and were not flashed because the invocation accidentally omitted the
  mandatory XIP linker-script selection and therefore hit the smaller
  all-SRAM ceiling; they do not measure the trace's actual size.  The retained
  form nonetheless reuses an existing three-value report line for
  the live GDMA source address immediately after DMA enable, host video enable
  and DPI feed enable.  This changes no pixel data, timing, transfer parameters
  or start order; its purpose is to identify the exact edge that first consumes
  framebuffer data without increasing the already ceiling-bound report.  The
  immutable-preload build now defines `P4_B5_START_TRACE` and omits only the
  initial dump of stable, previously recorded bridge registers to make room;
  the sustained per-second host, bridge and DMA fault checks remain enabled.
- Start-trace artifact built: core 187,696 bytes, SHA-256
  `450de64ca46539f4d68e8a495c2c930f42fa9ab0496ea791371fb7e1566c6926`.
  The final reproducible build used `P4_B5_VISUAL_GATE=1`,
  `P4_B5_STATIC_PRELOAD=1`, 200-MHz PSRAM, 360-MHz CPU and
  `P4_LDSCRIPT=ldscript-xip.lds`; it exited zero, contained no `error:`, passed
  the SRAM-residency check and printed both ESP32-P4 image-success markers.
  The connected target identified as ESP32-P4 v1.3 with MAC
  `e8:f6:0a:e0:46:4c`.  Flashing and hardware evidence remain pending.
- Start-trace D1001 result: only `ota_0` at `0x20000..0x4dfff` was written;
  write-time hashing and independent `verify-flash` both passed.  The source
  address was already `FB+0x0f00` immediately after DMA enable and
  `FB+0x2200` immediately after host-video enable; enabling DPI output did not
  move it again in the immediate sample.  Thus GDMA fills almost exactly the
  8-KiB bridge FIFO before DPI output starts.  The 60-second run remained
  technically clean: frames rose from 11 to 2,006, source and FIFO depth kept
  moving, and host payload, bridge raw and DMA fault status stayed zero before
  panel-safe stop and read-only SD discovery.  Pre-video consumption is real,
  although by itself it does not prove a wrong FIFO head because ESP-IDF uses
  the same high-level enable order.
- Next isolated bridge-state test: revision-one `BLK_RAW_NUM_CFG` resets to
  230,400 64-bit words, exactly a 720x1280 RGB565 frame, while this framebuffer
  is 256,000 words for 800x1280.  ESP-IDF does not rewrite the field when its
  one-item link keeps multi-block mode disabled, so it may be ignored; AROS
  must measure rather than assume that.  `P4_B5_BLK_RAW_FRAME=1` reloads it to
  the same full-frame count as `RAW_NUM_CFG`, without changing source address,
  timing or any panel coordinate.  A changed wrap phase would prove that the
  nominally inactive block counter still delimits data on revision one.
- Block-counter artifact built: core 187,760 bytes, SHA-256
  `2ac7392d5aa2b797b5ee0eec526f5dcac4a19ea61a386b1ac11d0991393eceed`.
  The reproducible build adds `P4_B5_BLK_RAW_FRAME=1` to the start-trace
  configuration, exited zero, contained no `error:`, passed SRAM-residency
  checking and printed both ESP32-P4 image-success markers.  Target identity,
  restricted `ota_0` write, independent flash verification and direct D1001
  geometry observation remain pending.
- Block-counter D1001 technical result: the connected ESP32-P4 v1.3 identified
  as MAC `e8:f6:0a:e0:46:4c`; only `ota_0` at `0x20000..0x4dfff` was written,
  and both write-time hashing and independent `verify-flash` passed.  The
  60-second run completed `t0..t59`, frames rose from 11 to 2,006, source and
  FIFO depth kept moving, and host payload, bridge raw and DMA fault status
  remained zero before panel-safe stop.  The programmed block-raw register
  read back as zero throughout, consistent with the field being inactive while
  multi-block mode is disabled.  Read-only SD discovery still passed.  Direct
  geometry observation is pending; if unchanged, this test rejects the reset
  default as the horizontal phase cause.
- Block-counter direct observation failed: the same verified image was reset
  for a second observer run, again reached 2,006 frames with zero host payload,
  bridge-raw or DMA faults, and stopped the panel safely.  The observer reported
  `immer noch verschoben`; correcting the reset-default 720x1280 block count to
  the actual 800x1280 framebuffer therefore has no visible effect.  Together
  with its zero readback while multi-block mode is disabled, this rejects
  `BLK_RAW_NUM_CFG` as the horizontal phase cause; the diagnostic switch must
  remain off by default.  The next test should target non-burst packetization or
  the GDMA-to-bridge FIFO burst boundary, not another source-coordinate offset.
- Effective JD9365 sequence audit found two extra AROS transactions.  A
  mechanical extraction that ignores commented source lines gives 174 active
  entries in Vellum's exact managed component and 176 in AROS.  The diff is
  limited to an extra initial `E0 00` and `4A 35`; the latter is explicitly a
  commented-out BIST command in the reference.  Vellum's wrapper sends one
  page-zero command before the table and the table sends one more, while AROS's
  equivalent wrapper plus duplicated table entry sent three.  The AROS table
  now contains exactly the reference's 174 active command/value pairs.  This
  corrects the contrary 2026-08-25 claim rather than silently replacing it.
  Hardware verification is pending; the first remote-decidable check is the
  exact 1000-Mbit/s burst/frame-ACK host mode, where disappearance of persistent
  `DPI_PLD_WR_ERR` and data-lane stop state would prove a functional change even
  without a display observer.
- Corrected-sequence exact-reference artifact built: core 187,680 bytes,
  SHA-256
  `eb6e64f6366a2b9362f4923f10737c9dd452abf19a3ae3541d239b42e09adebc`.
  It was rebuilt from all 198 fresh kernel `.o`/`.d` files with
  `P4_B5_VISUAL_GATE=1`, immutable preload, 200-MHz PSRAM, 360-MHz CPU,
  1000-Mbit/s lanes, burst-with-sync-pulses, frame acknowledge and the XIP
  linker script.  The build exited zero, contained no `error:`, passed SRAM
  residency and printed both ESP32-P4 image-success markers.  Target identity,
  restricted `ota_0` write, flash verification and D1001 counters remain
  pending.
- Exact-reference run did not improve host state.  The D1001 identified as
  ESP32-P4 v1.3, MAC `e8:f6:0a:e0:46:4c`; only `ota_0` was written and both
  write-time and independent verification passed.  The corrected 174-command
  sequence retained stable panel identity `93 65 04`, but the requested
  1000-Mbit/s burst/frame-ACK mode still held `INT_ST1=0x80`
  (`DPI_PLD_WR_ERR`) and `PHY_STATUS=0x15b9` with both data lanes stopped.
  Frames nevertheless rose from 11 to 2,006 with moving SAR/FIFO, zero bridge
  raw status and zero DMA faults before panel-safe stop and read-only SD
  discovery.  The table correction is retained as exact-reference hygiene,
  but it is rejected as sufficient cause of the 1000-Mbit/s failure.
- Next remote-decidable control: temporarily run the already verified Vellum
  image and read the live LDO3 control/analogue registers through USB-JTAG.
  Vellum uses Espressif's eFuse-calibrated regulator path while AROS currently
  writes the nominal untrimmed `dref=9, mul=6` solution.  A difference would
  provide a concrete PHY-supply experiment; equality rejects regulator trim
  without depending on display observation.  Restore the verified AROS image
  to `ota_0` immediately after the read-only snapshot.
- LDO3 reference snapshot found a real mismatch.  The clean Vellum 1.12.0
  image (2,381,664 bytes, SHA-256
  `25666ec02dae7ef1af66a03700b95ed5b019da0fb5ab54cdd88352a505928e73`)
  was temporarily written only to `ota_0`; write-time and independent
  verification passed.  During its active display interval USB-JTAG read
  `PMU+0x1c0/+0x1c4` as `0x40200180 / 0xc6000000`, while AROS had reported
  `0x40200180 / 0x97000000`.  Read-only eFuse words were
  `MAC_SYS_2=0x9b054313` and `MAC_SYS_3=0x26780122` (block v0.3).  Applying
  ESP-IDF's local `ldo_ll_voltage_to_dref_mul()` arithmetic decodes K=0.979,
  Vos=-0.003 and C=0.983 and selects `dref=12,mul=4`; the remaining analogue
  bit is `EN_VDET`, which IDF sets for ripple suppression.  This is a measured
  per-die difference, not an assumed voltage tweak.
- AROS now implements the same bounded 16-by-8 integer search from the
  read-only eFuse fields, retains nominal 9/6 only when no calibrated block is
  present, and explicitly enables ripple suppression.  The exact 1000-Mbit/s
  burst/frame-ACK artifact was rebuilt from all 198 fresh kernel `.o`/`.d`
  files: 188,320 bytes, SHA-256
  `a00f2b2f07079f773a7451cd093f1739659ae6e80a5a40e731d76e0d0b387c4d`.
  The build exited zero, passed SRAM residency and printed both ESP32-P4
  image-success markers.  D1001 v1.3 MAC `e8:f6:0a:e0:46:4c` was identified;
  only `ota_0` at `0x20000..0x4dfff` was written, and write-time plus
  independent verification passed.
- Calibrated-LDO D1001 result: AROS now reads back the exact Vellum LDO pair
  `0x40200180 / 0xc6000000` and reports `dref 12, mul 4`; panel identity remains
  stable at `93 65 04`.  The exact-reference link nevertheless still holds
  `INT_ST1=0x80` (`DPI_PLD_WR_ERR`) and `PHY_STATUS=0x15b9` with both data
  lanes stopped.  The 60-second run reached 2,006 ISR-rearmed frames with
  moving SAR/FIFO, zero bridge raw status and zero DMA faults, then stopped
  safely and discovered the SD card read-only.  The calibration fix is retained
  because it is an exact IDF parity correction and removes cold-boot dependence,
  but it is rejected as sufficient cause of the 1000-Mbit/s failure.  The next
  remote-decidable comparison is a live GDMA register snapshot under Vellum
  versus this exact AROS mode; display geometry observation is deferred until
  the user is physically present.
- USB-JTAG cannot read the DesignWare GDMA window on a halted P4 v1.3 core:
  both program-buffer and system-bus access failed at `0x50081010`.  The core
  was explicitly resumed after the failed read.  A temporary Vellum UART
  instrument was therefore built from the otherwise clean repository and
  flashed only to `ota_0` (2,382,176 bytes, SHA-256
  `50d851de1590377295c063f06259b054819d8b74d1975cfad838ca4abc47fd14`;
  write-time and independent verification passed).  While its known-good
  scanout was active it measured global CFG/CHEN/reset as
  `00000003/00000001/00000000`, and channel SAR/DAR/BLOCK_TS as
  `48314480/50105000/0003e7ff`.
- The decisive Vellum GDMA configuration is bit-identical to AROS:
  `CTL0=001e1b41`, `CTL1=400f87c0`, `CFG0=0000000f`,
  `CFG1=0a020001`, running `LLP=00000001` and `INT0/INT1=10/0`.
  Those are exactly the AROS descriptor/control values, including memory
  master 1, MIPI master 0, 64-bit widths, 512/256-beat source/destination
  bursts, 256,000-word block, list/list multiblock selection, M2P DMA flow
  control, DSI handshake and 5/2 outstanding request depths.  The framebuffer
  addresses differ only because each allocator chose a different PSRAM span;
  both target the same bridge FIFO `0x50105000`.  This closes the previously
  unmeasured GDMA-configuration branch.  The temporary Vellum source changes
  were removed and its repository is clean again.
- The calibrated AROS artifact hash was rechecked as
  `a00f2b2f07079f773a7451cd093f1739659ae6e80a5a40e731d76e0d0b387c4d`,
  the D1001 identity was rechecked, and only `ota_0` was restored; write-time
  and independent verification passed.  With panel table, LDO, bridge and
  GDMA values now matched, the next remote test must compare a dynamic host or
  bridge condition (interrupt/packet state or producer start phase), not alter
  another static GDMA field.
- A second read-only Vellum snapshot closes the host/bridge comparison before
  changing another producer parameter.  During known-good scanout the host
  read `VID_MODE_CFG=0x0000ff02`, packet size 800 and `LPCLK_CTRL=3`, while
  `INT_ST0/INT_ST1=0/0` and `PHY_STATUS=0x15bd`; the sampled blank therefore
  had clock and both data lanes in stop state.  Its 36-word bridge snapshot
  matched the staged AROS timing, raw count, pixel type and flow values, but
  exposed two final-state differences: Vellum had register-clock force-on
  `CLK_EN=0` and underrun interrupt enable `INT_ENA=1`; AROS had `CLK_EN=1`
  and `INT_ENA=0`.  The latter only reports bridge underflow and cannot remove
  the host's payload-write error, so the first isolated remote-decidable test
  is to leave `CLK_EN` at zero exactly as ESP-IDF's DPI path does.  Teardown
  retains force-on while accessing a potentially gated block.  Acceptance is
  disappearance of persistent `INT_ST1=0x80` with a corresponding lane-state
  change; visual geometry is deliberately deferred while the user is remote.
- Register-clock parity artifact and D1001 result: after deleting all generated
  kernel `.o`/`.d` files, the exact 1000-Mbit/s burst/frame-ACK build produced
  a 188,320-byte core with SHA-256
  `5a4620da14a4bdcc970cccfb303da6560a95b2f942d933bfa15f3cb02a3d5fd6`.
  It exited zero, contained no `error:`, passed SRAM residency and printed both
  ESP32-P4 image-success markers.  D1001 v1.3 MAC `e8:f6:0a:e0:46:4c` was
  identified; only `ota_0` at `0x20000..0x4dfff` was written, and write-time
  plus independent verification passed.  AROS now leaves bridge `CLK_EN=0`
  like Vellum and ESP-IDF, but `INT_ST1=0x80` still reasserted and the sampled
  video state remained `PHY_STATUS=0x15b9`.  GDMA SAR/FIFO moved, the run
  reached 2,006 rearmed frames with zero DMA faults and bridge raw status,
  then stopped safely and discovered the SD card read-only.  The parity fix is
  retained because force-on is unnecessary in the normal path, but rejected
  as sufficient cause.  With no local observer, the next step is an identical
  complete live host/bridge register snapshot under AROS and Vellum rather
  than another guessed configuration change.
- Complete live register parity was captured through USB-JTAG while each image
  was actively scanning.  AROS and Vellum are bit-identical across all staged
  bridge configuration words from `0x500a0800..0x500a088c` except dynamic FIFO
  depth and Vellum's underrun interrupt enable.  Their host video mode, packet
  size, HSA/HBP/HLINE, VACT, LP-clock control and PHY timing registers also
  match.  The AROS-only `0x80` at host offset `0xc0` is the already reported
  payload-write error, not a configuration bit.  Apart from dynamic status and
  stale last-command/test-interface data, the only final host-state difference
  is the completed command-read state: AROS reads
  `PCKHDL_CFG/CMD_MODE_CFG=0x19/0x010f7f00`; Vellum reads
  `0x1d/0x010f7f02`, meaning `BTA_EN` and `ACK_RQST_EN` remain set.  This
  corrects the initial interpretation of bit 2 as EoT; EoT transmit was already
  enabled in both images.  The instrumented Vellum image remained the same
  2,382,176-byte artifact with SHA-256
  `50d851de1590377295c063f06259b054819d8b74d1975cfad838ca4abc47fd14`;
  only `ota_0` was temporarily written and independently verified, its sources
  remained clean, and the verified AROS artifact was restored immediately.
- Next isolated remote test: `P4_B5_REF_READ_STATE=1` sets those two reference
  bits only after the last panel command, so it reproduces Vellum's live final
  state without asking AROS to service acknowledgements while transmitting the
  initialization table.  It changes no video, bridge, GDMA, PHY or framebuffer
  parameter.  Persistent `INT_ST1=0x80` rejects the state as causal; clearing
  it with a non-stopped PHY is the acceptance condition.  The switch remains
  diagnostic-only until hardware decides it.
- Final command-read-state parity was rejected on hardware.  After deleting
  all generated kernel objects, the exact 1000-Mbit/s build with
  `P4_B5_REF_READ_STATE=1` produced a 188,384-byte core with SHA-256
  `22ee773bac0cff6a7eeeaf79ed7129f8a3258caec043ac60d1d503f8df277c93`.
  The build exited zero, had no `error:`, passed SRAM residency and printed
  both image-success markers; only `ota_0` was written and write-time plus
  independent verification passed.  Despite matching Vellum's live
  `BTA_EN/ACK_RQST_EN` end state, AROS again reasserted `INT_ST1=0x80` and held
  `PHY_STATUS=0x15b9`.  SAR/FIFO remained active, 2,006 frames completed with
  zero DMA faults and bridge raw status, then panel-safe stop and read-only SD
  discovery passed.  The diagnostic switch stays off by default and is
  rejected as causal.  Static host, bridge and GDMA configuration parity is
  now closed; the next reference measurement is the three producer-start
  transitions, not another retained register value.
- The producer-start reference measurement is complete and rejects FIFO
  prefill/start ordering as the cause.  A first temporary Vellum instrument
  used the GDMA global window instead of channel 1 for SAR and is explicitly
  invalid for that field (artifact 2,381,920 bytes, SHA-256
  `257bc2a36dcf60cc6c6b2eba1c7e9542bbe4a624848aa9a3632efd9ff92a77b6`);
  it was corrected before drawing a hardware conclusion.  The corrected,
  otherwise clean Vellum artifact was 2,381,920 bytes with SHA-256
  `543082f772434bb224dd026a8b84be568955cb325f462c4320242e2d51a4edc9`.
  Only `ota_0` was written, and write-time plus independent verification
  passed.  Its three UART samples decoded to `FB=0x48230a80, SAR=0,
  FIFO=0` immediately after the DMA-start call, then `SAR=FB+0x2200,
  FIFO=0x3fd` immediately after video enable, unchanged immediately after
  bridge feed enable.  AROS's existing samples reach the identical
  `SAR=FB+0x2200, FIFO=0x3fd` state by video enable and likewise do not move
  at feed enable; its earlier `FB+0xa80` sample after the DMA call is only a
  few instructions later.  Both producers therefore enter feed with the same
  8,704-byte source advance and 1,021-word FIFO fill.  The temporary ESP-IDF
  instrumentation was removed and both the local ESP-IDF and Vellum source
  repositories were clean after the measurement.  With public host, bridge,
  GDMA configuration and first-frame producer phase now matched, the next
  remote-decidable comparison is the ordered internal D-PHY test-interface
  programming, followed by frame-to-frame DMA rearm latency if that sequence
  is also equal.  Visual geometry remains deferred until a local observer is
  present.
- Source-level D-PHY comparison found that AROS writes the same five internal
  PLL register/value pairs as ESP-IDF and uses the same test-interface edges,
  but not in the same reset state.  ESP-IDF's `mipi_dsi_hal_init()` pulses
  digital PHY reset, enables the clock lane and forces the PLL before
  `mipi_dsi_hal_configure_phy_pll()` performs the internal writes.  AROS kept
  digital reset asserted for all five writes and released it afterward.  The
  next exact-reference artifact moves only that reset/enable sequence to the
  ESP-IDF order; PLL divisors, range selector, clocks, LDO, host, bridge and
  GDMA settings remain unchanged.  Remote acceptance is disappearance of
  persistent `INT_ST1=0x80` together with a non-stopped data-lane state;
  otherwise the corrected reference order is retained but rejected as the
  sufficient cause.
- The ESP-IDF D-PHY reset-order artifact was rebuilt from all 198 fresh kernel
  `.o`/`.d` files.  It is 188,320 bytes with SHA-256
  `d621a86b55db920f25654790cdc7de741e56053448edca9ebe5082591cbd2ce4`;
  the build exited zero, passed SRAM residency and printed both ESP32-P4 image
  success markers.  D1001 v1.3 MAC `e8:f6:0a:e0:46:4c` was identified; only
  `ota_0` at `0x20000..0x4dfff` was written, and write-time plus independent
  verification passed.  The PHY locked with all lanes stopped before panel
  setup and stable identity `93 65 04`, but video again reasserted
  `INT_ST1=0x80` and settled at `PHY_STATUS=0x15b9`.  SAR and bridge FIFO moved
  through 2,006 ISR-rearmed frames with zero DMA faults and bridge raw status,
  followed by panel-safe stop and read-only SD discovery.  The reset-order
  correction is retained as exact ESP-IDF parity but rejected as sufficient
  cause.  The next remote-decidable branch is frame-to-frame DMA rearm timing,
  specifically whether AROS leaves the bridge without a producer between DMA
  completion and channel restart while ESP-IDF uses a different continuous or
  callback path.
- The frame-rearm source audit found AROS equivalent to ESP-IDF at the DMA
  completion boundary: both clear the level interrupt, restore the one LLI's
  valid/last markers, reload the list head and enable the channel.  A more
  fundamental ordered-state difference precedes the first frame.  AROS's B5
  path enabled and committed the bridge once with the earlier pattern-stage
  bridge-flow configuration, then changed flow controller, raw count and FIFO
  thresholds live and committed again.  ESP-IDF stages the complete DMA-facing
  bridge configuration before its first enable/commit, and only commits a
  second time when enabling pixel feed.  Because the bridge has internal
  counters and FIFO state not represented by the final register snapshot, the
  next exact-reference artifact defers B5's first bridge enable/commit until
  all DMA-facing fields are staged.  B4 keeps its existing ownership sequence.
  Remote acceptance remains disappearance of `INT_ST1=0x80` and transition of
  the data lanes out of their persistent stopped state.
- The single initial bridge-commit artifact was rebuilt from all 198 fresh
  kernel objects: 188,320 bytes, SHA-256
  `fb54ef529de3cb42349107dbd881a5b6883fca77130c965a96d2842dc368087b`.
  The build exited zero, passed SRAM residency and printed both ESP32-P4 image
  success markers.  D1001 v1.3 MAC `e8:f6:0a:e0:46:4c` was identified; only
  `ota_0` at `0x20000..0x4dfff` was written, and write-time plus independent
  verification passed.  Deferring B5's first bridge enable/commit did not
  change the failure: stable panel identity remained `93 65 04`, video held
  `INT_ST1=0x80` and `PHY_STATUS=0x15b9`, and SAR/FIFO moved for 2,006 frames
  with zero DMA faults and bridge raw status before panel-safe stop and
  read-only SD discovery.  The ordered-state correction is retained because
  it removes an unnecessary live flow-controller transition, but rejected as
  sufficient cause.  Before changing rearm timing, the next remote UART
  instrument must identify the first transition at which `INT_ST1=0x80`
  appears: staged host/bridge, DMA enable, video enable or pixel-feed enable.
- The transition-local UART instrument now samples command packet status,
  both host interrupt-status words and PHY status after five boundaries:
  staged host timing, complete bridge commit, DMA enable, video enable and
  pixel-feed enable.  It does not delay or change those transitions.  The
  first sample containing `INT_ST1=0x80` will identify which subsystem action
  creates the condition; if it predates feed, frame rearm and bridge
  underflow cannot be causal.
- The five transition samples remained clean through pixel-feed enable:
  staged/bridge/DMA all read `INT_ST1=0, PHY=0x15bd`, and video/feed both read
  `INT_ST1=0, PHY=0x15b9`.  The familiar `INT_ST1=0x80` appeared only in the
  later 100-ms sample.  Thus the condition is created either by the
  parameterless DCS `0x29` injected immediately after feed or by ordinary
  first-frame traffic during its following 20-ms wait; it is not created by
  any configuration/start write itself.  `P4_B5_SKIP_FINAL_PANEL_ON=1` omits
  only that final live-stream command for the next exact-reference run.  The
  vendor table has already issued its own 0x29 transaction, and the remote
  acceptance signal is host status rather than visible panel state.  If
  `0x80` still appears, command injection is closed and first-frame/rearm
  timing becomes the remaining branch.
- Omitting the live parameterless `0x29` did not suppress the failure.  The
  diagnostic artifact was 188,768 bytes with SHA-256
  `2e08d652e707fa57b7cc042aba5e6cbbda3a98eee1a76ff12d10e34e4c0444f1`;
  only `ota_0` was written and both write-time and independent verification
  passed.  All five immediate edge samples were again clean, then ordinary
  video traffic reached `INT_ST1=0x80` by the 100-ms sample with four completed
  DMA frames, moving SAR/FIFO and no DMA or bridge fault.  Live-command
  injection is therefore closed.  The same no-command diagnostic now polls
  for the first assertion and records elapsed timer ticks, completed-frame
  count, SAR and FIFO depth.  Zero completed frames proves a first-frame host
  acceptance fault; one or more isolates the frame-rearm boundary.
- First-assertion tracing closes frame rearm completely.  The 189,088-byte
  diagnostic artifact had SHA-256
  `e17c2940f978105a77f283d43b8a0b688932610e007527d8a6c5f5e4e8e90284`;
  only `ota_0` was written and write-time plus independent verification
  passed.  `INT_ST1=0x80` first appeared after `0x1129` system-timer ticks with
  `dma_frames=0`, `SAR=FB+0x3200` and bridge FIFO depth `0x302` (770 words).
  The channel had consumed only 12,800 bytes of the first 2,048,000-byte frame
  and had not crossed a completion/restart boundary.  This is a first-frame
  host-acceptance failure: ordinary DPI data starts filling the path while the
  host data lanes remain stopped.  DMA rearm latency, ISR priority and every
  post-frame action are now outside the causal tree.  The next comparison must
  cover host reset/start and clock handshakes that are not represented by the
  matched final register image.
- Exact source re-audit found that the earlier early-AUTO experiment was based
  on a reversed reading of ESP-IDF.  `esp_lcd_new_dsi_bus()` calls
  `mipi_dsi_host_ll_set_clock_lane_state(...LP)` before every panel command;
  the helper clears both `auto_clklane_ctrl` and `phy_txrequestclkhs`.
  `esp_lcd_panel_dpi.c` selects AUTO (`LPCLK_CTRL=3`) only after enabling DMA
  and video mode.  AROS currently selects AUTO before the first panel command
  and repeats it at video start.  The next exact-reference artifact therefore
  clears both bits for command mode and retains the existing AUTO write in
  `krnP4DsiVideoOn()`.  It restores the valid final parameterless DCS `0x29`;
  the no-command first-assertion switch is not part of this build.  Because the
  observer is remote, acceptance is entirely UART-decidable: the five edge
  samples must remain clean and ordinary first-frame traffic must no longer
  assert `INT_ST1=0x80`; otherwise the corrected sequencing is retained as
  reference parity but rejected as the sufficient cause.
- Command-phase LP parity is hardware-negative.  A fresh 198-object exact
  1000-Mbit/s burst/frame-ACK build, with the valid final parameterless DCS
  `0x29` restored, produced a 188,848-byte core with SHA-256
  `13dec0c2fca6374924c1d0182fda90a157ca38d5006f31a1e1bc627fc79681d8`.
  The build exited zero, passed SRAM residency and printed both image-success
  markers.  D1001 v1.3 MAC `e8:f6:0a:e0:46:4c` was identified; only `ota_0`
  at `0x20000..0x4efff` was erased and written, and write-time plus independent
  verification passed.  UART confirmed command mode with the clock lane in LP,
  stable panel identity `93 65 04`, and clean staged/bridge/DMA/video/feed edge
  samples.  Ordinary first-frame traffic nevertheless asserted
  `INT_ST1=0x80` and held `PHY_STATUS=0x15b9`; the 60-second run completed
  2,006 ISR-rearmed frames with moving SAR/FIFO, zero DMA faults and zero
  bridge raw status.  Panel-safe stop, 121,942-MB SD discovery and two explicit
  write-protected-medium reports passed.  The corrected LP-to-AUTO ordering is
  retained as source parity but rejected as sufficient cause.  With the host
  bus-clock enable and global DSI reset already bit-for-bit identical to
  `mipi_dsi_ll_enable_bus_clock()` / `mipi_dsi_ll_reset_register()`, the next
  remote-decidable audit is the remaining host/bridge reset and start sequence,
  including revision-conditional bridge reset behaviour and any status that a
  final register snapshot cannot expose.
- The reset continuation closes that branch without another flash.  Vellum's
  `CONFIG_ESP32P4_REV_MIN_FULL=100` selects the legacy bridge helper, whose
  `mipi_dsi_brg_ll_reset()` is a documented no-op; AROS likewise performs no
  local bridge reset.  The APB clock enable, global `reg_rst_en_dsi_brg` pulse,
  lane-count write, host/PHY power-on, PHY digital-reset pulse, clock-lane
  enable and force-PLL order are now source-identical.  The remaining ordered
  mismatch is the command transaction state, which the previous final-state
  test did not reproduce.  Vellum enables `ACK_RQST_EN` before the first read;
  IDF's read path then sets `BTA_EN` and never clears it, so both remain active
  through the complete JD9365 table and video handover.  AROS normally clears
  BTA after the identity and previously set both bits only after all commands.
  `P4_B5_IDF_CMD_STATE=1` now isolates the exact ordered reference state while
  retaining AROS's bounded waits and UART edges.  Clean video/feed edges with
  no subsequent `INT_ST1=0x80` accept it; a receive-direction edge or the same
  first-frame assertion rejects it without requiring a display observer.
- The first ordered ACK/BTA artifact is negative and identifies the missing
  temporal part precisely.  A fresh 198-object build produced a 188,848-byte
  core with SHA-256
  `577ebc61f184db7aa1120cbf608f80826c3388f2d43e2b2f7beaeb5bbc8ab06a`;
  only `ota_0` was written and write-time plus independent verification passed.
  Identity still read `93 65 04`, but the staged/bridge/DMA edges already read
  `PHY_STATUS=0x15af`, video/feed read `0x15ab`, and UART localized the first
  persistent turnaround to trace step 3, the end of the vendor table.  The
  host also asserted `INT_ST1=0x80`; 2,006 DMA frames completed with moving
  SAR/FIFO and no DMA or bridge fault before panel-safe stop and read-only SD
  discovery.  IDF calls `vTaskDelay(pdMS_TO_TICKS(delay_ms))` after *every*
  vendor-table entry, including zero-delay entries; AROS queued the next header
  as soon as the command FIFO was merely not full.  Under ordered ACK/BTA this
  removes the scheduling point in which an acknowledgement turnaround can
  finish.  The next refinement keeps the same state but, after every DCS write,
  waits boundedly for command and payload FIFOs empty plus transmit direction.
  It is the direct bare-metal equivalent of the reference yield and remains
  UART-decidable before any frame is launched.
- That first settle condition was deliberately bounded and failed safely on
  hardware: the 189,040-byte core (SHA-256
  `d9a08ba6f97f9abb143e2d894eddbb680b636d8accf2374314b5e4f58048c992`)
  was written and independently verified only in `ota_0`; identity succeeded,
  then the first acknowledged framing write did not return PHY_DIRECTION to
  transmit within 20 ms.  B3 reported `a command fifo never drained`, returned
  the panel safe with the backlight never enabled, and the later SD path again
  reported the medium write protected.  This proves that waiting for transmit
  direction is *not* equivalent to IDF's `vTaskDelay(0)`: IDF imposes no such
  predicate.  Vellum is configured at `CONFIG_FREERTOS_HZ=1000`.  The refined
  diagnostic therefore waits boundedly only for the two TX FIFOs to empty,
  then supplies one 1-ms scheduling window before the next header.  Whether
  the aggregate command sequence returns to transmit state is still judged at
  the existing step-3 UART edge.
- The refined 1-ms scheduling-window artifact was 189,104 bytes with SHA-256
  `2ed47bfe28a8232e4176b292d165e8c0016f75f1d4e4d18c6ef861f09a3c59f8`;
  only `ota_0` was written and write-time plus independent verification
  passed.  It completed identity and the full vendor table, but did not alter
  any decisive edge: staged/bridge/DMA remained `PHY_STATUS=0x15af`,
  video/feed `0x15ab`, and first traffic asserted `INT_ST1=0x80`.  The
  60-second run completed 2,006 frames with moving SAR/FIFO, zero DMA faults
  and zero bridge raw status before panel-safe stop.  The SD was discovered as
  121,942 MB and reported write protected twice.  Artificial per-command
  scheduling is rejected and removed from the diagnostic; ordered ACK/BTA is
  retained as a separately reproducible negative state.
- A temporary UART oracle was then built into the known-good Vellum reference,
  flashed only to `ota_0`, and its source tree restored clean immediately
  afterward.  The build metadata proves that Vellum uses the pinned local IDF
  at `/Volumes/Dev/esp-idf/v6.0/esp-idf` commit
  `662a3be354759d9487bf4b1a629fadb766cb1800`, not the newer v6.0.1 tree.  Five
  samples at 20-ms intervals after panel-on all read `INT_ST1=0`,
  `PHY_STATUS=0x15bd`, bridge raw status zero and moving FIFO depths
  `0x301,0x3bc,0x380,0x344,0x308`.  Its live video-packet status remained
  `0x00020001` while command status moved `0x40015 -> 0x50015`.  Therefore
  AROS's `DPI_PLD_WR_ERR` is not a harmless startup sticky bit: the working
  driver does not set it under the same 1000-Mbit/s burst/frame-ACK profile.
  AROS had never sampled `VID_PKT_STATUS`; the next normal-state artifact adds
  that read to first-assertion evidence so the overflowing DPI input can be
  distinguished from the host's internal payload buffer without a display
  observer.
- The normal transmit-state first-assertion artifact was 189,152 bytes,
  SHA-256
  `7d845e25252e24bb94c4e31cc33633ce485b99ff252421212ce85b56a408d0ad`.
  Only `ota_0` was written and independently verified.  Without the final live
  `0x29`, the first pure-video assertion occurred before any complete DMA frame
  after `0xf3d` timer ticks, at `SAR=FB+0x3380` and bridge depth `0x372`.
  Crucially, `VID_PKT_STATUS=0x00020009`: both the host's internal payload
  buffer and its DPI input payload FIFO were full.  Vellum's corresponding
  `0x00020001` has the internal buffer full but the DPI input not full.  This
  localizes the divergent behavior to host consumption of bridge DPI input,
  not PSRAM, GDMA delivery or bridge starvation.
- Two 400-pixel chunks per 800-pixel line did not relieve that pressure.  The
  189,152-byte artifact had SHA-256
  `b96aa01b957ea57b2d4be526237df095b139711fd252b7773930a265e1b5a994`;
  only `ota_0` was written and independently verified.  It asserted the same
  `INT_ST1=0x80` with `VID_PKT_STATUS=0x00020009` before frame zero completed,
  after `0x1271` ticks at `SAR=FB+0x4a00`, and sustained the same 69 MB/s with
  no DMA or bridge fault.  Packet capacity is rejected; chunking is not a fix.
- A second temporary Vellum oracle found every video `*_ACT` mirror and
  `VID_SHADOW_CTRL` at zero while the reference displayed correctly.  The
  pinned driver does not enable shadow-register latching, so those read-only
  mirrors do not expose a hidden timing difference.  Vellum source was again
  restored clean.  Source inspection then found a measurement-induced timing
  violation in AROS itself: after enabling video mode it performs two PHY
  trace reads, then reads DMA state, command status and the complete host state
  before enabling bridge DPI output.  The IDF reference performs video-mode,
  clock-AUTO and bridge-feed writes consecutively.  `P4_B5_ATOMIC_START=1`
  removes all reads from that critical window and samples only after feed; a
  clean first 100 ms accepts the ordering fix entirely by UART.
- Atomic host/feed start is hardware-negative.  The 189,152-byte artifact had
  SHA-256
  `d64b65e064735b424733e554bed1d4e517c52b4a35498e3586b7ea00070ccf87`;
  only `ota_0` was written and independently verified.  Removing every status
  read between video-mode/clock-AUTO and bridge-feed commit did not change the
  first-frame failure: `INT_ST1=0x80` first asserted after `0xb76` timer ticks
  with zero completed frames, `SAR=FB+0x3200`, bridge depth `0x360` and
  `VID_PKT_STATUS=0x00020009`.  A repeat reset of the exact artifact measured
  the same class of failure after `0xdec` ticks at `SAR=FB+0x3200`, depth
  `0x307`; all five pre-start command states were already `0x00050015`, the
  same external and buffered FIFO-empty state observed under Vellum.  Atomic
  start and undrained command buffers are therefore both rejected.
- The public host, bridge and producer state is now exhausted, but a host FSM
  can retain command-phase state that no register snapshot exposes.  The next
  bounded diagnostic pulses only host-core `SHUTDOWNZ` after the complete
  vendor table reports `CMD_PKT_STATUS=0x50015` and before video timing is
  staged.  The PHY remains powered and locked, the panel remains awake, and
  bridge/DMA are still off.  Disappearance of `INT_ST1=0x80`, together with
  first-frame data-lane activity, accepts hidden command-to-video state as the
  cause; otherwise the pulse is removed and that branch is closed.
- The isolated host-core restart is hardware-negative.  The 189,152-byte core
  had SHA-256
  `b1910b16387a4a30106b9f4e8089847ee86472fb97abd70bddad3fe79f898867`;
  only `ota_0` was written, and write-time plus independent verification
  passed.  The panel ID remained `93 65 04` and every pre-start command state
  was the reference-empty `0x00050015`, but the first assertion still occurred
  in frame zero after `0xb1c` timer ticks at `SAR=FB+0x3200`, bridge depth
  `0x31d`, `INT_ST1=0x80` and `VID_PKT_STATUS=0x00020009`.  Throughput then
  remained about 69 MB/s with moving producer state and no DMA or bridge
  fault.  A stale command-phase host FSM which a `SHUTDOWNZ` pulse can clear is
  rejected.  The next UART-only isolation removes *all* diagnostic reads from
  the end of the vendor command table through host staging, bridge setup, DMA
  arming, video-mode entry and feed commit.  Status is captured only after the
  complete reference write sequence, so a clean first 100 ms accepts observer
  perturbation and another frame-zero `0x20009` rejects it.
- The full command-table-to-feed atomic start is hardware-negative.  Its
  189,088-byte core had SHA-256
  `c76d82ebaa156ae9c7fc7043a5d9329f0a7ce1aacdf595e74ecb7b4e8db95ea9`;
  only `ota_0` was written and both write-time and independent digest
  verification passed.  The first post-feed snapshot still showed the
  reference-empty command state `0x00050015` and PHY stop state `0x15bd`, but
  traffic asserted `INT_ST1=0x80` in frame zero after `0xd86` timer ticks at
  `SAR=FB+0x3200`, bridge depth `0x307` and
  `VID_PKT_STATUS=0x00020009`.  The clock lane then entered HS (`0x15b9`),
  producer SAR and FIFO depth continued moving at 69 MB/s, and DMA/bridge
  faults remained zero.  Diagnostic reads anywhere in the handover are
  rejected as the cause.  The remaining known static bridge-register
  difference from the live Vellum oracle is its enabled bridge-underrun
  interrupt; AROS leaves all bridge interrupts masked.  Although an interrupt
  mask should not change flow control, matching it is the next bounded parity
  test before deliberately changing producer/consumer start order.
- Reference bridge-underrun interrupt enable is hardware-negative.  The
  189,088-byte core had SHA-256
  `d419c6813e75a685ab0f6ee6f77ee8045ded1aeae7ba25a44114bffbdda119b5`;
  only `ota_0` was written and both digest verifications passed.  Enabling
  `INT_ENA.bit0` at the exact post-feed point used by IDF left bridge RAW zero,
  yet the host again asserted `INT_ST1=0x80` and
  `VID_PKT_STATUS=0x00020009` in frame zero, this time after `0x846` ticks at
  `SAR=FB+0x3200` and bridge depth `0x346`.  The last measured static bridge
  difference is rejected.  The next bounded test retains bridge-before-DMA
  and an already-running producer, but commits the bridge DPI feed immediately
  before enabling host video mode.  A clean first 100 ms accepts a host
  start-order race; another frame-zero `0x20009` rejects prefeeding as a fix.
- Feed-before-video changes the failure edge materially but does not clear it.
  The 189,088-byte core had SHA-256
  `5a3c0a1f7692b7793bbdd93fd12b2b89253142f11e9a78724490e8e6059b696e`;
  only `ota_0` was written and independently verified.  Instead of asserting
  within roughly `0x800..0xd00` ticks before the first producer wrap, the host
  first asserted `INT_ST1=0x80` after `0x74178` ticks with one DMA frame
  complete, `SAR=FB+0x3200`, bridge depth `0x30d` and the same
  `VID_PKT_STATUS=0x00020009`.  This does not prove that a complete DSI frame
  reached the panel--the frame counter belongs to DMA--but it is the first
  ordering change to postpone the failure to the producer's frame boundary.
  The next UART artifact records PHY stop-state/direction and video-FIFO state
  across that tight first-assertion poll.  Data-lane activity before the error
  accepts real host transmission and focuses the next test on frame-ACK/BTA;
  no sampled data-lane activity keeps the issue at the initial host start.
- The tight PHY/FIFO trace proves that feed-first starts real host traffic.
  Its 189,472-byte core had SHA-256
  `26bdb4696e4a1902893af14c7b500227014436664f01ea55eaf91dbad15c4ff0`;
  only `ota_0` was written and independently verified.  The result reproduced
  after `0x74281` ticks with one DMA frame complete, `SAR=FB+0x3380`, bridge
  depth `0x36b` and final `VID_PKT_STATUS=0x00020009`.  Across the poll,
  `PHY_STATUS` ranged from AND/OR `0x1529/0x15bd` with four state changes and
  at least one sample in which the two data-lane stop bits were clear.  Video
  FIFO status ranged through AND/OR `0x00000000/0x0003000d`; receive direction
  was never sampled.  Therefore the host did transmit before stalling, rather
  than merely accepting bridge input forever in stop state.  The conjunction
  of first producer-boundary failure and frame-ACK enabled now warrants the
  same feed-first run with only frame-ACK removed.  Zero `INT_ST1` through the
  first 100 ms accepts ACK/BTA as the post-first-frame blocker; an unchanged
  boundary failure rejects it.
- Feed-first without frame-ACK is hardware-identical at the failure edge.  The
  189,472-byte core had SHA-256
  `6a030fe33bbe2baecfd12af273724051c9312c0b7e0f22e754c1c8cad69bfa5c`;
  only `ota_0` was written and independently verified.  With read-back
  `VID_MODE_CFG=0x0000bf02`, the error again appeared after `0x741fe` ticks
  and one DMA frame at `SAR=FB+0x3400`, bridge depth `0x378` and
  `VID_PKT_STATUS=0x00020009`.  PHY/FIFO ranges were unchanged:
  `0x1529/0x15bd`, four transitions, one sampled data-lane-active state, no
  sampled receive direction, and video-status OR `0x0003000d`.  Frame ACK/BTA
  is rejected as the first-boundary blocker.  The next diagnostic temporarily
  uses the already-characterized GDMA register-reload producer, which has no
  channel stop/rearm boundary.  It is not a geometry fix--that producer caused
  the earlier cyclic coordinate phase--but zero host error with reload would
  isolate the newly exposed failure to the list-item completion/rearm edge.
- Continuous GDMA reload is hardware-negative and rules out the producer
  boundary.  The 189,344-byte core had SHA-256
  `170beaf1185645fa15f0b0f1127460ffd33b83a414649a4b85f05851183ba9b3`;
  only `ota_0` was written and independently verified.  With DMA interrupts
  disabled and channel registers automatically reloading, SAR crossed the
  framebuffer boundary continuously, but the host asserted after `0x741ac`
  ticks--the same first-video-frame interval--at `SAR=FB+0x3200`, bridge depth
  `0x346` and `VID_PKT_STATUS=0x00020009`.  PHY/FIFO ranges again matched the
  feed-first list run.  Descriptor completion, interrupt latency and rearm are
  rejected; the exposed boundary belongs to host video timing.  The next
  single-variable test keeps feed-first, burst and no frame ACK but disables
  all video low-power transitions.  Surviving the 100-ms oracle accepts the
  first frame's HS-to-LP-to-HS transition as the blocker; another `0x741xx`
  failure rejects it.
- Disabling every video low-power transition is hardware-negative.  The
  189,472-byte core had SHA-256
  `ca0dbf187222d9abcb683ce9247d378f119eb1319bdc2fab55f2ee6447a99f25`;
  only `ota_0` was written and independently verified.  With read-back
  `VID_MODE_CFG=0x00000002`, the same `INT_ST1=0x80` and
  `VID_PKT_STATUS=0x00020009` appeared after `0x7400f` ticks and one DMA frame
  at `SAR=FB+0x3400`, bridge depth `0x379`.  The tighter PHY trace changed as
  expected: AND/OR was `0x1529/0x15b9`, only two transitions occurred, and
  the data lanes were sampled active `0xc3` times with no receive direction.
  Subsequent one-second samples remained at `PHY_STATUS=0x1529` while the DMA
  completed roughly 34 frames/s with moving SAR and bridge FIFO, zero bridge
  raw status and zero DMA faults.  Thus the host remains in high speed after
  the sticky overflow; neither a first-frame LP exit/re-entry nor a host stop
  is its cause.  The first failure remains locked to one host video-frame
  interval despite removing frame ACK, LP transitions and the GDMA completion
  boundary.  The next bounded UART test changes only `VID_HLINE_TIME` by a
  small negative delta while bridge timing stays fixed.  A shifted or absent
  first-error edge establishes a producer/consumer rate mismatch; an unchanged
  `0x740xx` edge rejects line-period sensitivity before any visual gate.
- A 16-lane-byte-clock shorter host line is hardware-negative.  The 189,472-
  byte core had SHA-256
  `87a6f3d5c73f1b28c84fc474e245436ed6bb3823fbde9cba223495ccb59fcc29`;
  only `ota_0` was written and independently verified.  Source, disassembly
  and UART read-back independently agreed on `VID_HLINE_TIME=2734`, while the
  bridge retained its reference 880-pixel timing.  Nevertheless the first
  `INT_ST1=0x80` occurred after `0x741ad` ticks and one DMA frame at
  `SAR=FB+0x3200`, bridge depth `0x339`, with the unchanged
  `VID_PKT_STATUS=0x00020009`.  PHY AND/OR, active-lane count and video-status
  range also matched the no-LP baseline.  The run then completed its bounded
  two seconds at moving SAR, zero bridge status and zero DMA faults before
  quiescing safely; the SD card was only mounted through the enforced
  read-only path.  This small change is below run-to-run edge variation and
  neither fixes nor measurably moves the boundary.  One larger `-64` run is
  the bounded discriminator: it changes the nominal host frame by about
  0.58 ms while retaining positive line blanking.  An unchanged edge rejects
  host `VID_HLINE_TIME` as the rate-control cause; a proportional edge shift
  justifies narrowing the timing rather than guessing visually.
- A 64-lane-byte-clock shorter host line also leaves the failure edge
  unchanged.  The 189,472-byte core had SHA-256
  `2af316a0b96db8ae0b9dee2ca20bda0057f8bc7484ed55a156ca4e8541bb579f`;
  only `ota_0` was written and independently verified.  Disassembly and UART
  agreed on `VID_HLINE_TIME=2686`, a 2.3-percent change, but the first
  assertion remained at `0x740e7` ticks with one DMA frame,
  `SAR=FB+0x3200`, bridge depth `0x309`, and the same `0x80/0x00020009`
  host status.  PHY/FIFO ranges were identical to the no-LP baseline and the
  bounded run quiesced safely with zero DMA or bridge fault.  Host HLINE is
  therefore rejected as the clock controlling this failure edge.  The next
  discriminator restores the exact host timing and shortens only the bridge
  horizontal total from 880 to 860 pixels.  At 40 MHz over 1,344 lines that
  moves an incoming DPI-frame boundary earlier by about 0.672 ms, or roughly
  `0x2a00` system-timer ticks.  A corresponding move proves the assertion is
  keyed by the bridge frame; an unchanged `0x740xx` edge instead assigns it to
  an internal host period.  The altered bridge timing is diagnostic-only and
  cannot pass a geometry gate.
- Shortening only the bridge horizontal total by 20 pixels moves the failure
  by the predicted incoming-frame interval.  The 189,472-byte core had
  SHA-256
  `c353b72f629bdd2bc56a77474ce2cddb606ed060cbcc0d3a0dcb633d47e2361a`;
  only `ota_0` was written and independently verified.  Disassembly and UART
  read-back agreed on bridge `H_CFG0=0x0320035c` (860 total pixels) while the
  host was restored to `VID_HLINE_TIME=2750`.  The first
  `INT_ST1=0x80` moved from the 880-pixel baseline at `0x740e7` to
  `0x71737`, a difference of `0x29b0` timer ticks versus the calculated
  `0x2a00`.  It still occurred after one DMA frame at `SAR=FB+0x3200`, bridge
  depth `0x33a`, and the same `VID_PKT_STATUS=0x00020009`; PHY activity,
  active sampling and the bounded safe stop also remained healthy, with zero
  DMA or bridge faults.  Therefore the first assertion is locked to the
  incoming bridge DPI-frame boundary, not GDMA completion or the DSI host's
  programmed line period.  The next UART-only discriminator keeps the bridge
  frame total and rate unchanged but moves 20 vertical blanking lines from
  VFP to VBP.  A moved or eliminated assertion isolates the bridge/host
  active-window phase; an unchanged edge assigns the remaining defect to the
  frame-boundary transition itself.  Both timing changes are diagnostic-only
  and cannot pass a geometry gate.
- The first attempt to move 20 blanking lines from bridge VFP to VBP is not a
  single-variable result.  The XIP core was 189,248 bytes with
  SHA-256
  `436c356d6d2317495cd776e0982175e594f5b230b93c003f3d773453f4adcc5c`;
  the build exited zero, contained no `error:`, passed the SRAM-residency
  check and emitted the successful ESP32-P4 image marker.  Disassembly and
  UART read-back agreed on bridge `V_CFG1=0x00040032` (VSYNC 4, VBP 50),
  while `V_CFG0=0x05000540` retained 1,280 active of 1,344 total lines.  Only
  `ota_0` was written; write-time and
  independent verification passed on D1001 revision v1.3, MAC
  `e8:f6:0a:e0:46:4c`.  The first assertion remained at `0x744c1` ticks with
  `INT_ST1=0x80` and `VID_PKT_STATUS=0x00020009`.  The deliberate vertical
  partition mismatch additionally raised bridge raw status `0x1`, and the
  tight sample caught DMA just before its first wrap (`frames=0`,
  `SAR=0x49f6d900`, depth `9`), but it neither prevented nor shifted the host
  boundary failure.  The bounded two-second run stopped the scanout safely,
  then completed read-only SD discovery and continued to the already known
  missing graphical-console alert.  UART review then found that this build
  had omitted `P4_LANE_MBPS=1000`: it used the 1,500-Mbit/s default and read
  back host `VID_HLINE_TIME=4125`, not the comparison run's 2,750.  It is
  therefore retained as a safely executed two-variable diagnostic, not as
  evidence rejecting active-window phase.  The exact VBP test must be rebuilt
  at 1,000 Mbit/s before the ESP-IDF source audit can choose the next change.
- A same-source 1,000-Mbit/s VBP A/B pair shows active-window sensitivity but
  is not the healthy producer result.  Both XIP
  cores were 189,248 bytes, passed the no-`error:` build and SRAM-residency
  gates, and read back host `VID_HLINE_TIME=2750`.  The VBP-50 image had
  SHA-256
  `fa8c72c5314ee692de043382a93521940c80b52642925766124ad69200d2187b`;
  its first `0x80/0x00020009` assertion occurred at `0x748fa` ticks.  The
  immediately rebuilt VBP-30 control had SHA-256
  `0ad2435f17edf0010de4ac7806418a5f0abc06c550089429686e302b39dc2dfe`;
  its assertion occurred at `0x72d14`.  The measured delta is `0x1be6`
  ticks; moving 20 880-pixel lines at 40 MHz predicts `0x1b80`, only 102
  ticks less.  Both tight samples caught DMA just before its first wrap, with
  near-empty bridge FIFO, identical sticky host status, and bridge raw
  underrun `0x1`; both bounded runs then maintained moving SAR, zero DMA
  faults, stopped safely after two seconds, and completed read-only SD
  discovery.  Each image was written only to `ota_0` and independently
  verified.  This refines the earlier horizontal-total result: that test moved
  the same next active-start boundary by changing every line's duration.
  UART review also showed that both builds omitted the established
  `P4_PSRAM_MHZ=200 P4_CPU_MHZ=360` settings: PSRAM remained at 20 MHz,
  feed commit caught only `SAR=FB+0x400` rather than the reference-like
  `FB+0x2200`, and bridge underrun was already asserted.  The proportional
  edge movement is real for that low-bandwidth underrun path, but it cannot
  localize the healthy producer's host failure.  A VBP-50 run with the
  established 200/360-MHz producer is required; only zero bridge RAW status
  makes its edge comparable to the earlier `0x740xx` feed-first evidence.
- The healthy 200-MHz-PSRAM/360-MHz-CPU VBP-50 run confirms that the same
  active-window relationship is not an underrun artefact.  Its 189,472-byte
  XIP core had SHA-256
  `ec762d95d8a4ef4af247a94660c29cae104be35a8fa96dd2c2c2ed922fe9e99b`;
  the build exited zero, contained no `error:`, passed SRAM residency, and
  disassembly showed host HLINE 2,750 plus bridge
  `V_CFG1=0x00040032`.  Only `ota_0` was written and independently verified.
  With PSRAM read back at 200 MHz and CPU requested at 360 MHz, the first
  `0x80/0x00020009` assertion occurred at `0x76088` ticks, one DMA frame and
  exactly `SAR=FB+0x3200`.  The corresponding healthy VBP-30/no-LP baseline
  was `0x7400f`; adding the calculated 20-line interval predicts `0x75b8f`.
  The remaining `0x4f9` ticks are 80 microseconds against a deliberate
  440-microsecond displacement.  More importantly, bridge RAW stayed zero,
  throughput was 69 MB/s, FIFO depth remained live, and DMA faults stayed
  zero through the bounded safe stop and read-only SD discovery.  Together
  with the near-exact low-bandwidth A/B delta, this establishes that
  feed-before-video fails when the next active DPI window begins: the host was
  enabled after the bridge cycle's initial synchronization edge.  Feed-first
  is closed as a working-order candidate.  Geometry work must use the
  reference host-video-before-feed order and treat any attempt that loses its
  first synchronization edge as invalid even if it defers an error for one
  frame.
- The next bounded host-before-feed discriminator pre-stages `DPI_EN` in the
  bridge shadow state without committing it.  DMA is then armed and the host
  enters video mode in the reference order; one final `DPI_CFG_UPD` write
  starts the producer instead of a post-host bridge read/write/update trio.
  A clean 100-ms host oracle with zero bridge RAW status accepts a lost-first-
  sync-edge race.  An immediate underrun means revision-one does not shadow
  this field; an unchanged frame-zero `0x20009` rejects the shorter commit
  window.  Every outcome remains bounded by the two-second panel-safe stop.
- Pre-staging `DPI_EN` is hardware-negative.  The 189,472-byte XIP core had
  SHA-256
  `8a283309f5363237b52a0b97ff51115958136b0e0699f93c9cde53c877aec462`;
  it passed the build, image and SRAM-residency gates, and disassembly proved
  that the post-host feed function contained only one `DPI_CFG_UPD` write.
  Only `ota_0` was written and independently verified.  At 200-MHz PSRAM,
  360-MHz CPU and 1,000-Mbit/s lanes, the host still asserted
  `0x80/0x00020009` after only `0x922` ticks in DMA frame zero.  Bridge RAW
  remained zero, throughput was 69 MB/s and DMA faults remained zero through
  the bounded safe stop and read-only SD discovery.  The post-host bridge
  read/write/update latency is therefore rejected.  The next start primitive
  gates the shared DPI pixel clock, enables host video and bridge feed while
  no DPI edge can occur, then releases that clock with one write.  A clean
  first 100 ms accepts an edge-level synchronization fix; another immediate
  payload error rejects it.
- Pixel-clock-gated start is prepared as an additive discriminator on the
  rejected pre-staged path.  After DMA is armed, software clears only
  `DPICLK_EN`, enables host video, commits the already staged bridge feed, and
  sets `DPICLK_EN` again.  Command/PHY clocks, bridge memory traffic and the
  panel remain otherwise untouched.  The full-atomic trace samples only after
  clock release, so zero host error and zero bridge RAW status are the entire
  acceptance gate; the normal path remains unchanged unless the switch is
  explicitly selected.
- Pixel-clock-gated start is hardware-negative.  The 189,600-byte XIP core
  had SHA-256
  `48dc778a449a1fb82a965dffc2bc8f545d9bc78a6b14dc388bd7a35b6d1cf70c`;
  it passed the build/image/residency gates, and disassembly confirmed both
  the `DPICLK_EN` clear and later set around the pre-staged host/feed writes.
  Only `ota_0` was written and independently verified.  The host nevertheless
  asserted `0x80/0x00020009` after `0xcc3` ticks in frame zero.  Bridge RAW
  stayed zero, measured throughput was 69 MB/s and DMA faults stayed zero
  through panel-safe stop and read-only SD discovery.  A shared first pixel-
  clock edge is therefore insufficient, and the explicit clock gate is
  rejected.  The next audit returns to the Waveshare JD9365 wrapper's actual
  lifecycle order--DPI panel init versus vendor command table--because that
  can leave host FSM state different despite identical final registers.
- The wrapper audit rejects a DPI-init-versus-vendor-table reversal but finds
  an earlier lifecycle difference.  `esp_lcd_new_panel_jd9365_8()` calls
  `esp_lcd_new_panel_dpi()` while the panel is still held in reset; that IDF
  creation path enables the DPI pixel clock, stages every host and bridge
  timing register, globally enables the bridge and commits its configuration.
  Only afterwards does the board pulse hardware reset and call the wrapper
  init, which sends the complete JD9365 table before the underlying DPI init
  arms DMA, enters host video mode and commits the pixel feed.  AROS previously
  delayed DPI clock, timing and global bridge creation until after the vendor
  table.  Final-register parity cannot see that FSM history.
- `P4_B5_EARLY_DPI_CREATE` is the bounded discriminator for that difference.
  It performs the clock/timing/global-bridge portion immediately after command
  IO setup and before the hardware reset pulse, leaves DMA/video/feed off
  through the vendor table, and does not rewrite the early configuration at
  the later start.  The existing 100-ms host oracle, zero bridge RAW status,
  two-second safe stop and read-only SD gate apply unchanged.  A clean host
  accepts lifecycle history as the cause; the familiar immediate
  `0x80/0x00020009` rejects it without requiring a visual observation.
- Early DPI creation is hardware-negative.  The 189,520-byte XIP core had
  SHA-256
  `5cf0965e9b49a4a92eae76ac4a42875c36070665d60fe5e0057ff7906798bc11`;
  the build contained no exact `error:`, passed the SRAM-residency and image
  creation gates, and only `ota_0` at `0x20000` was written and independently
  verified on D1001 revision 1.3.  UART confirmed that DPI creation occurred
  before the panel reset and that the JD9365 identity still answered
  `93 65 04`.  The host then asserted `0x80/0x00020009` after `0xcb0` ticks in
  DMA frame zero, at `SAR=FB+0x3200`.  Bridge RAW status remained zero,
  throughput was the expected 69 MB/s and DMA faults remained zero through the
  two-second safe stop; the SD card was subsequently discovered through its
  read-only path.  The earlier clock/timing/global-bridge lifecycle is not the
  missing state and is rejected.  Because the operator is remote, no visual
  claim is made for this run.  The next audit compares the complete ESP-IDF
  host reset and register-write sequence rather than another final snapshot.
- An initial continuation audit inspected the wrong installed IDF tree and is
  corrected before commit.  `~/.espressif/v6.0.1` does select AUTO before DBI,
  but Vellum's `project_description.json`, compile database and object paths
  all name `/Volumes/Dev/esp-idf/v6.0/esp-idf`, clean tag `v6.0`, commit
  `662a3be354759d9487bf4b1a629fadb766cb1800`.  That exact source explicitly
  selects LP until DPI init.  Thus the older evidence entry above was right.
  The additive AUTO comparison was nevertheless completed safely: its
  189,520-byte image had SHA-256
  `5506b02ae0892d5bf28685e366f523235edc24bc1420fa9493b0d327e129529a`,
  passed build/image/verification gates, answered identity `93 65 04`, then
  asserted the unchanged `0x80/0x00020009` after `0xcf2` ticks in frame zero.
  Bridge RAW, 69-MB/s transfer, DMA faults, safe stop and read-only SD remained
  healthy.  AUTO command history is rejected and is not reference parity; no
  remote visual claim is made.
- The exact v6.0 object disassembly exposes the next still-unmatched start
  primitive.  After enabling video mode it performs two volatile RMW writes to
  `LPCLK_CTRL`: `AUTO_CLKLANE` first at object offset `0xd2`, then
  `TXREQUESTCLKHS` at `0xde`.  AROS set both bits in one write.  The public end
  state is `3` in either case, but the clock-lane FSM observes an intermediate
  AUTO-without-HS-request state only under Vellum.  `P4_B5_SPLIT_AUTO_START`
  reproduces those two writes additively to the now-tested early DPI creation.
  The same first-100-ms host oracle and two-second safe stop decide it without
  a display observer.
- Split AUTO start is hardware-negative.  The 189,520-byte image had SHA-256
  `023dce9469c9a4e8df1c9dd1279004f945298bb98407408b1102a6fcc3a7b9b5`;
  it passed the exact-error, SRAM-residency, image, write and independent
  `ota_0` verification gates.  AROS disassembly showed the intended video-mode
  write followed by separate `dsi_set(0x94,2)` and `dsi_set(0x94,1)` calls.
  D1001 again answered identity `93 65 04`, then asserted
  `0x80/0x00020009` after `0x995` ticks in DMA frame zero.  Bridge RAW stayed
  zero, DMA faults stayed zero and transfer measured 72 MB/s before the safe
  stop and read-only SD discovery.  The intermediate clock-lane FSM state is
  rejected; no remote visual claim is made.  The next source/assembly audit is
  the exact GDMA channel-enable primitive before host start.
- The exact v6.0 GDMA audit closes the enable primitive without another
  hardware variable.  `dw_gdma_channel_enable_ctrl()` is only one write of
  channel enable plus its write-enable bit; it has no read-back, poll, delay or
  hidden resume.  This is the same final write AROS already emits.  The audit
  instead found a register-invisible lifecycle difference: DPI object creation
  calls `dw_gdma_new_channel()` before the JD9365 reset and vendor table.  That
  path enables clocks, pulses both resets, enables the controller, configures
  the idle channel and installs its interrupt.  `dpi_panel_init()` much later
  fills/selects the link item and performs the single channel-enable write.
  AROS previously did both halves together after the command table.
- `P4_B5_EARLY_GDMA_CREATE` moves only the controller reset, idle channel
  configuration and existing AROS interrupt state into the already tested
  early-DPI creation window.  Descriptor contents, cache write-back and the
  channel-enable write remain at the normal host-before-feed start.  The
  later path explicitly retains the early controller instead of resetting it.
  Run it additively with `P4_B5_EARLY_DPI_CREATE` and the already measured
  split-AUTO baseline, at 1,000-Mbit/s lanes, 200-MHz PSRAM and 360-MHz CPU.
  A clean 100-ms UART host oracle with zero bridge RAW status accepts GDMA
  lifecycle history; the familiar frame-zero `0x80/0x00020009` rejects it.
  The two-second safe stop and read-only SD gate remain mandatory, and no
  visual acceptance is possible while the operator is remote.
- Early GDMA creation is hardware-negative.  The 189,616-byte XIP image had
  SHA-256
  `c4ea6bafc927271fc1f5247f74f51d284c1703c794d70adeb380dc2de9cf442b`;
  the build exited zero, contained no exact `error:`, passed SRAM residency
  and emitted a fresh successful ESP32-P4 image marker.  Disassembly proved
  the intended early `krnP4ScanoutDmaCreate()` call before bridge creation and
  panel reset, while the later `krnP4ScanoutDmaUp()` contained descriptor
  writes followed directly by the single `CHEN=0x101` start and no second
  controller reset.  Only `ota_0` was written and an independent flash digest
  matched on D1001 v1.3, MAC `e8:f6:0a:e0:46:4c`.  UART reported the early
  idle-channel marker, identity `93 65 04`, then the familiar first
  `INT_ST1=0x80 / VID_PKT_STATUS=0x00020009` after `0xd87` ticks in DMA frame
  zero at `SAR=FB+0x3200`.  Bridge RAW remained zero, measured transfer was
  71 MB/s, DMA faults remained zero, and the moving producer completed the
  bounded two-second panel-safe stop.  Boot then discovered the SD card through
  the read-only path and reached the already known missing-console alert.
  Therefore GDMA object lifetime before the command table is rejected; no
  visual claim is made for this remote run.  The next audit must compare an
  earlier stateful operation in the exact v6.0 DSI bus/PHY creation path, not
  another final public-register value or GDMA start delay.
- The exact linked Vellum objects reveal that the earlier retained PHY reset
  correction still matched final values, not the complete creation sequence.
  `mipi_dsi_hal_init()` writes `PHY_SHUTDOWNZ`, pulses `PHY_RSTZ`, then emits
  separate volatile RMW writes for `ENABLECLK` and `FORCEPLL`.  Its first
  internal-register transaction writes zero directly to `PHY_TST_CTRL0`; it
  never pulses `TESTCLR`.  The bus object also leaves `STOP_WAIT=0` throughout
  PHY creation, PLL programming and lock/stop-state waits, then writes `0x3f`
  only at the end of bus timing setup.  AROS combined ENABLECLK/FORCEPLL,
  added a TESTCLR pulse and set STOP_WAIT before bringing the PHY up, despite
  reaching the same final public registers.
- `P4_B5_EXACT_PHY_CREATE` reproduces those three ordered differences while
  retaining every PLL register/value pair, clock, lane count and timeout.
  The existing late STOP_WAIT write in command-mode setup becomes its only
  write.  It is additive to the exact early DPI/GDMA and split-AUTO lifecycle
  image, so the immediately preceding hardware run remains the one-variable
  control.  UART must still prove PLL lock, lanes stopped, identity, first
  host assertion, bridge RAW, DMA health, safe stop and read-only SD; no
  visual conclusion is available remotely.
- Exact v6.0 PHY creation is hardware-negative.  The 189,664-byte XIP image
  had SHA-256
  `cfbe0285552631d6a8f9542cec3f7cdc5a093f4b7dce3adbe6472fd02cc57695`;
  its fresh build exited zero, contained no exact `error:`, passed SRAM
  residency and emitted the successful ESP32-P4 image marker.  Side-by-side
  object disassembly proved Vellum's separate shutdown/reset/ENABLECLK/
  FORCEPLL writes and absence of a TESTCLR pulse; AROS disassembly reproduced
  those writes and contained no early STOP_WAIT constant.  Only `ota_0` was
  written and independent digest verification passed on the identified v1.3
  board.  UART then read `PHY_IF_CFG=0x00000001` immediately after PLL lock,
  proving two lanes with STOP_WAIT still zero, and panel identity remained
  `93 65 04`.  Ordinary first-frame traffic nevertheless asserted
  `INT_ST1=0x80 / VID_PKT_STATUS=0x00020009` after `0x851` ticks in frame zero
  at `SAR=FB+0x3400`.  Bridge RAW stayed zero, measured throughput was 69 MB/s
  and DMA faults stayed zero through the two-second panel-safe stop; boot then
  discovered the SD card through its read-only path.  These residual PHY
  lifecycle differences are rejected, and no remote visual claim is made.
  The next exact-object audit is DBI/command creation order, whose individual
  bitfield writes still precede every panel command and can leave host state
  invisible to the final register snapshot.
- The DBI audit closes packet construction but leaves one bounded lifecycle
  discriminator.  `mipi_dsi_hal_host_gen_write_dcs_command()` and AROS both
  fill the long-packet payload FIFO before the header, select DCS short-write
  zero/one-parameter or long-write from the same total byte count, and wait
  only while the corresponding FIFO is full.  IDF contains no hidden write
  acknowledgement wait.  Earlier `P4_B5_IDF_CMD_STATE` runs already enabled
  ACK before the first command, retained BTA after the identity read and
  rejected extra FIFO/direction waits and 1-ms scheduling gaps.  The linked
  DBI object does, however, prove fourteen volatile `CMD_MODE_CFG` RMW writes:
  TE clear, ACK set, then each generic/DCS/MRPS LP selector separately.  AROS
  has so far written that same pre-command state in one operation.
- `P4_B5_EXACT_DBI_CREATE` is the last DBI discriminator.  It emits those
  object-ordered RMWs and retains BTA after the identity read, while leaving
  packet bytes, panel delays, video/bridge/GDMA configuration and the already
  exact PHY lifecycle unchanged.  Run it additively with early DPI/GDMA
  creation and split AUTO at the 1,000-Mbit/s, 200-MHz-PSRAM, 360-MHz-CPU
  baseline.  UART must prove the exact pre-command registers, identity, first
  host result, bridge RAW state, DMA health, bounded panel-safe stop and
  read-only SD.  The familiar frame-zero `0x80/0x00020009` rejects the whole
  DBI creation branch; no visual claim is possible while the operator is
  remote.
- Exact v6.0 DBI creation is hardware-negative.  The fresh build exited zero,
  contained no exact `error:`, passed SRAM residency and emitted a successful
  ESP32-P4 image.  Its 189,952-byte XIP core had SHA-256
  `9ecae2f4a00463e20edfc12122622f23dbb01e44607e0bbbb79f68c083a05e59`.
  Vellum object disassembly showed the TE/ACK and twelve LP-selector stores;
  AROS disassembly showed the corresponding fourteen ordered `dsi_clr`/
  `dsi_set` calls rather than the former combined write.  D1001 was identified
  as v1.3, MAC `e8:f6:0a:e0:46:4c`; only `ota_0` at `0x20000..0x4efff` was
  erased/written, and write-time plus independent digest verification passed.
  UART proved pre-command `PCKHDL_CFG/CMD_MODE_CFG=0x00000019/0x010f7f02`,
  exact-PHY `PHY_IF_CFG=0x00000001`, panel identity `93 65 04` and a completed
  vendor table.  First-frame traffic still asserted
  `INT_ST1=0x80 / VID_PKT_STATUS=0x00020009` after `0xd2e` ticks at
  `SAR=FB+0x3200`, with FIFO depth `0x302`.  Bridge RAW stayed zero, measured
  throughput was 69 MB/s and DMA faults stayed zero through 44 completed
  frames and the bounded two-second panel-safe stop.  The SD card was then
  mounted through its read-only path and reported write protected.  Thus DBI
  construction, packet construction and command-state scheduling are closed;
  no remote visual claim is made.  The next exact-object audit is the earlier
  post-PHY bus setup: IDF writes CRC, ECC, HS/LP EoTP and both clock-divider
  fields through separate volatile RMWs, where AROS still combines each
  register's final value.
- Exact bus-object disassembly expands that mismatch beyond the packet handler.
  After PLL lock and lane stop, v6.0 writes command mode, clears AUTO and the
  HS clock request separately, writes the four PHY transition fields one by
  one, then performs CRC, ECC, HS-EoTP-set and LP-EoTP-clear as four PCKHDL
  RMWs.  It writes timeout-clock division before escape-clock division, clears
  the two halves of `TO_CNT_CFG` separately, clears each remaining 16-bit
  timeout field and updates the 15-bit maximum-read field.  AROS previously
  collapsed every same-register group into its final word.  The final live
  snapshot cannot reveal any intermediate host/clock-lane FSM input.
- `P4_B5_EXACT_BUS_CREATE` reproduces that post-lock sequence field by field,
  additively to the exact PHY and DBI lifecycles.  It changes no final public
  value.  The same 1,000/200/360 baseline, first-frame host oracle, bridge/DMA
  health checks, two-second safe stop and read-only SD gate decide it remotely.
  An unchanged frame-zero payload error closes the complete DSI bus creation
  path and moves the source audit forward to DPI object creation writes.
- Exact post-PHY bus creation is hardware-negative.  The 190,320-byte XIP
  image had SHA-256
  `9d6039728f5610b5d83c6f3d2a2cd58d574d76835d7d8afb62b159313f4f4d0b`;
  its fresh build exited zero, contained no exact `error:`, passed SRAM
  residency and emitted the successful ESP32-P4 image marker.  AROS
  disassembly proved separate LP-clock clears, all four transition fields,
  CRC/ECC/HS+LP-EoTP writes, timeout-before-escape dividers, timeout fields
  and maximum-read RMW in the v6.0 object order.  Only `ota_0` was erased and
  written on D1001 v1.3, MAC `e8:f6:0a:e0:46:4c`; write-time and independent
  digest verification passed.  UART read exact bus values
  `PHY_TMR_CFG/PHY_TMR_LPCLK_CFG/CLKMGR_CFG=0x00320068/0x002e0080/0x00000d07`,
  exact DBI `0x00000019/0x010f7f02` and identity `93 65 04`.  Ordinary
  first-frame traffic still asserted `INT_ST1=0x80 / VID_PKT_STATUS=0x20009`
  after `0xdee` ticks in frame zero at `SAR=FB+0x3200`, with depth `0x336`.
  Bridge RAW stayed zero, measured throughput was 69 MB/s and DMA faults
  stayed zero through 44 frames.  A repeated long capture reached the
  two-second panel-safe stop, mounted the 121,942-MB SD card and repeatedly
  reported it write protected.  The complete bus/PHY/DBI creation history is
  therefore closed as causal, and no visual claim is made remotely.  The next
  exact-object audit starts at `esp_lcd_new_panel_dpi()`, concentrating on
  still-combined host-video and bridge field writes before the already tested
  early object lifetime and producer start.
- Local observation closes the visual part of that exact-object experiment.
  After the operator returned to D1001, the identical 190,320-byte image with
  SHA-256
  `9d6039728f5610b5d83c6f3d2a2cd58d574d76835d7d8afb62b159313f4f4d0b`
  was reset and observed twice.  The display remained completely black on
  both runs.  UART continued to report the same frame-zero
  `INT_ST1=0x80 / VID_PKT_STATUS=0x00020009`; there was therefore no visible
  geometry or colour result to accept.  Exact v6.0 bus/PHY/DBI construction
  neither repairs nor improves the 1,000-Mbit/s burst mode.  The next bounded
  hardware control restores the last accepted visual transport configuration:
  1,500-Mbit/s lanes, non-burst video, no frame ACK, VMUL=1 and one immutable
  60-second coordinate image.
- The 1,500-Mbit/s visual control restored output but reproduced the geometry
  failure.  All generated kernel objects were deleted before the flag change;
  the fresh XIP build used `P4_B5_VISUAL_GATE=1`, immutable preload,
  `P4_SCANOUT_SECS=60`, non-burst/no-ACK, VMUL=1, 200-MHz PSRAM, 360-MHz CPU
  and `ldscript-xip.lds`.  It exited zero, contained no exact `error:`, passed
  SRAM residency and emitted the successful ESP32-P4 image marker.  The
  189,360-byte core had SHA-256
  `9a2f6ae3b8a1649e56639c69cf4062d53b59c085dfd1b80dc561554570a57713`.
  D1001 identified as v1.3, MAC `e8:f6:0a:e0:46:4c`; only `ota_0` at
  `0x20000..0x4efff` was erased/written, and write-time plus independent
  digest verification passed.  UART proved panel identity `93 65 04`, no
  host payload error, zero bridge RAW status and zero DMA faults while frames
  rose from 11 to 2,006.  The start trace again reached `SAR=FB+0x2200` at
  host-video enable and did not move at feed enable.  The local observer saw
  the colourful asymmetric rectangles, but still cyclically displaced within
  each row.  The run then reached `scanout stopped, panel safe` and mounted the
  121,942-MB SD card through the read-only path.  This control excludes a dead
  panel or lost power state and reconfirms the constant horizontal phase
  defect.  Because the working Vellum driver was already measured at the same
  `FB+0x2200/FIFO=0x3fd` feed-entry state, the common prefill itself is not a
  sufficient cause; the next change must continue the exact DPI/host lifecycle
  audit or isolate non-burst line packetisation, not add a framebuffer offset.
- The exact linked DPI object exposes the remaining creation-write mismatch.
  Vellum's IDF-v6.0 object writes DPI clock source, divider and enable as
  three operations; assigns VCID, colour code and each polarity separately;
  assigns every LP, frame-ACK and video-mode field separately; and writes each
  host and bridge timing field through its own volatile RMW.  Its bridge setup
  is especially stateful: `RAW_NUM_CFG.raw_num_total`, alignment and the
  write-triggered internal-counter reload are three distinct writes.  It then
  writes input type, RGB/YUV selection, output type and sub-configuration
  separately, followed by flow-controller, multi-block count, frame-interval,
  burst, threshold, bridge-enable and update fields.  AROS previously combined
  all same-register groups and wrote the count plus reload trigger together.
  Equal final snapshots cannot show this history.
- `P4_B5_EXACT_DPI_CREATE` now reproduces that linked-object order at the
  already tested early DPI creation point.  It deliberately changes no final
  register value, framebuffer byte, source address, panel command or timing.
  The first bounded test keeps the visible 1,500-Mbit/s non-burst/no-ACK
  transport and immutable coordinate pattern, additively with early DPI/GDMA
  creation.  Zero host/bridge/DMA error, safe stop and read-only SD remain the
  technical gate; only direct observation can accept or reject horizontal
  phase.  An unchanged cyclic displacement closes DPI field-write history and
  returns the audit to the unsupported 1,000-Mbit/s burst start state.
- Exact-DPI visual artifact built.  After deleting every generated kernel
  object/dependency file, the 1,500-Mbit/s non-burst/no-ACK build added early
  DPI creation, early idle-GDMA creation and `P4_B5_EXACT_DPI_CREATE` to the
  immutable 60-second 200/360-MHz visual control.  It exited zero, contained
  no exact `error:`, passed SRAM residency and emitted a successful ESP32-P4
  image.  The 189,568-byte core has SHA-256
  `5765e5b06a0089e25fcdec908dd0d03b20ebebded56d7d6ea978ab3326d83a63`.
  Object disassembly contains 34 ordered `dsi_field`/bridge-timing calls in
  `krnP4DsiPatternOn()` and 17 bridge-field calls across creation/feed, rather
  than the former combined same-register stores.  D1001 identity, restricted
  flash write, technical run and direct geometry observation remain pending.
- Exact DPI creation order is hardware-negative for the horizontal phase
  defect.  D1001 identified as v1.3, MAC `e8:f6:0a:e0:46:4c`; only `ota_0`
  at `0x20000..0x4efff` was erased/written with the 189,568-byte image whose
  SHA-256 is
  `5765e5b06a0089e25fcdec908dd0d03b20ebebded56d7d6ea978ab3326d83a63`,
  and write-time plus independent digest verification passed.  The bounded
  60-second run read panel identity `93 65 04`, reported no host payload
  error, kept bridge RAW status and DMA faults at zero, and advanced completed
  frames from 11 to 2,006.  Host-video and feed enable again both saw
  `SAR=FB+0x2200`; exact bridge construction ended at
  `RAW_NUM_CFG=0x0003e800`, flow `0x10` and enable `1`.  Direct observation
  showed the colourful asymmetric rectangles but with the same cyclic
  horizontal displacement as the non-exact control.  The run reached
  `scanout stopped, panel safe`, then mounted the 121,942-MB SD card through
  the read-only path; the later missing-console alert is outside the bounded
  scanout gate.  Separate volatile field-write history throughout the linked
  IDF DPI object is therefore closed as causal.  The remaining live delta is
  not a constant framebuffer pointer phase and should be isolated in
  non-burst line packetisation versus the still-black 1,000-Mbit/s burst
  startup, using a packet-width/blanking oracle rather than more register
  write-order replicas.
- A working-driver coordinate control is built and waiting for USB recovery.
  A temporary, now fully reverted Vellum change filled all three native
  800x1280 RGB565 DPI framebuffers with the same asymmetric AROS coordinate
  card: red/green above blue/yellow with a four-pixel black centre cross.  It
  writes back every complete framebuffer, raises the backlight and holds the
  image for 60 seconds before normal Vellum/LVGL startup.  This is the direct
  control missing from the earlier statement that Vellum displayed normally:
  it will decide whether Espressif's known-working burst path maps native x=0
  correctly under the exact same diagnostic geometry.  The Vellum source tree
  and this AROS tree are both clean after the temporary build.  The resulting
  2,382,432-byte `vellum-d1001.bin` has SHA-256
  `c7f3096f477d34bd6f95d225075d9c7b34f6b627492316c3eece629823005bfb`;
  the ELF contains the unique `AROS coordinate control visible for 60 seconds`
  marker and ESP-IDF reported successful ESP32-P4 image creation.  No flash
  write occurred: after the AROS safe stop and later missing-console alert,
  D1001 disappeared from both `/dev` and the macOS USB registry.  Flashing
  remains restricted to `ota_0` once USB enumeration is restored.  A correct
  Vellum quadrant card would assign the defect to AROS's exact-burst startup;
  the same 550-pixel wrap under Vellum would instead invalidate that premise
  and redirect the audit to panel/vendor coordinate setup.
- Vellum coordinate control completed its technical D1001 gate.  USB returned
  as `/dev/cu.usbmodem101`; the target again identified as ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`, and the artifact hash was rechecked before writing.
  Only `ota_0` from `0x20000` was written; sector-rounded erase ended at
  `0x265fff`, the 2,382,432-byte image ended at `0x265a5f`, and both esptool's
  write-time hash plus a separate `verify-flash` digest matched.  Boot found
  the 32-MB AP PSRAM at 200 MHz, restored the normal 360-MHz CPU setting, read
  JD9365 identity `93 65 04` and initialized the working 800x1280 RGB565,
  40-MHz triple-framebuffer path.  The unique control marker appeared at
  3.85 seconds; the frame remained undisturbed until the programmed hold ended
  at 63.95 seconds, after which ordinary Vellum startup continued.  There was
  no reboot or panel error in that interval.  Direct visual classification of
  the quadrants is the sole pending result; the source trees remain clean.
- Vellum coordinate control passed direct observation.  The observer reported
  `rechtecke passen jetzt`: the same native red/green-over-blue/yellow card
  occupied the expected two equal horizontal halves under Espressif's
  1,000-Mbit/s burst/ACK scanout, with no cyclic third colour region.  This
  closes panel geometry, vendor coordinate setup and the diagnostic pattern
  itself as causes of AROS's approximately 550-pixel horizontal wrap.  It also
  makes the live transport mode decisive rather than merely correlative:
  Vellum's burst path preserves x=0 while AROS's otherwise clean 1,500-Mbit/s
  non-burst path does not.  The next bounded AROS artifact must combine the
  newly exact DPI-creation history with the already audited exact Vellum
  1,000-Mbit/s burst/ACK bus, PHY and DBI lifecycles.  That full combination
  has not yet been run; the exact-DPI hardware-negative result above used only
  the visible non-burst control and therefore cannot reject it.
- Full exact-reference combination is the next bounded test.  It restores the
  working 1,000-Mbit/s burst-with-sync-pulses and frame-ACK host mode and adds,
  in one artifact, the individually audited exact PHY, post-PLL bus, DBI and
  DPI creation histories; early DPI and idle-GDMA object lifetimes; separate
  AUTO-clock-lane then HS-request writes; the uninterrupted full-atomic start;
  and the reference bridge-underrun enable.  The framebuffer remains the same
  immutable native coordinate card and no source bias is permitted.  Earlier
  exact-burst runs predated `P4_B5_EXACT_DPI_CREATE`, while its only hardware
  run forced the non-burst control, so neither is evidence for or against this
  combined state.  A clean image would directly join Vellum's accepted x=0
  mapping; the familiar frame-zero `INT_ST1=0x80 / VID_PKT_STATUS=0x00020009`
  would prove that exact DPI creation still leaves an AROS-only startup-state
  delta and reject the combination without interpreting a black panel.
- Full exact-reference artifact built.  Every generated kernel `.o` and `.d`
  was deleted before each invocation.  The first fresh compile and link passed
  SRAM residency but did not create an image because `esptool` was absent from
  that shell's `PATH`; it was not flashed and is not hardware evidence.  A
  second fully fresh build supplied the pinned Espressif v6.0.1 `esptool`
  executable explicitly, exited zero, contained no exact `error:`, passed the
  same SRAM-residency check and emitted `Successfully created ESP32-P4 image`.
  Its 190,016-byte core has SHA-256
  `63014cd8fae64e85d898b1f1303b54a63571da81754251f0891667afde360e0b`.
  ELF strings independently contain the exact PHY, bus and DBI reports plus
  `DPI path created before panel reset, including idle GDMA channel`; the
  immutable 60-second coordinate path and start trace are present.  Target
  identity, restricted `ota_0` write, UART oracle and direct geometry remain
  pending.
- Correction: the full exact-reference AROS combination did not pass a native
  coordinate observation.  The target identified as ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`; only `ota_0` at `0x20000..0x4efff` was erased and
  written with the 190,016-byte image whose SHA-256 is
  `63014cd8fae64e85d898b1f1303b54a63571da81754251f0891667afde360e0b`,
  and write-time plus independent digest verification passed.  UART proved
  the exact PHY, bus, DBI and DPI creation paths, panel identity `93 65 04`,
  the intended `FB+0x1500` atomic start, zero bridge RAW status, zero DMA
  faults and sustained 69 MB/s scanout while completed frames advanced from
  11 to 2,006.  The PHY remained in the frame-ACK turnaround state
  `0x15bb`, and host `INT_ST1=0x80` asserted from the first frame.  The direct
  quote `rechtecke passen jetzt` was copied here from the immediately
  preceding Vellum control and was not a separate AROS observation.  That was
  an evidence-attribution error: the operator's later repeated AROS runs were
  completely black, including an artifact that preloaded the same coordinate
  card before DMA start.  The run reached `scanout stopped, panel safe`,
  then mounted the 121,942-MB SD card through the read-only path and reported
  it write protected; the later missing-console alert remains outside this
  bounded scanout gate.  The exact combination is therefore hardware-negative
  and does not close the horizontal coordinate blocker.  The correction is
  intentionally additive after the erroneous committed claim so the audit
  trail shows both the mistake and its withdrawal.
- The rejected combination is reproducible as
  `P4_B5_FULL_EXACT_TRIAL=1`.  The trial expands in the central hardware
  header to 1,000-Mbit/s lanes, burst video, frame ACK, exact PHY/bus/DBI/DPI
  creation, early DPI and idle-GDMA lifetimes, split AUTO/HS clock writes,
  full-atomic start and reference bridge-underrun enable.  It explicitly
  removes the visual harness's historical non-burst define and rejects a
  conflicting lane-rate override.  The individual switches remain available
  for diagnosis until B5 closes; this grouping records a failed trial and is
  neither an accepted transport nor a claim that every grouped mechanism is
  independently necessary.
- The full-exact visual-gate artifact is built and target-identified.
  Every generated kernel object/dependency file was deleted before invoking
  the then-named `P4_B5_REFERENCE_PROFILE=1` (now corrected to
  `P4_B5_FULL_EXACT_TRIAL=1`) with `P4_B5_VISUAL_GATE=1` and
  `P4_SCANOUT_SECS=60`
  with 200-MHz PSRAM, 360-MHz CPU, `ldscript-xip.lds` and the pinned Espressif
  v6.0.1 `esptool`.  The fresh build exited zero, its complete captured output
  contained no exact `error:`, SRAM residency passed and image generation
  ended with `Successfully created ESP32-P4 image`.  The 190,800-byte core has
  SHA-256
  `7ec35c0d1807904ab82508cf7d9b4e9cdbafd1f5b2742b4e86a5e4aaf2bac7ee`;
  ELF strings contain all three exact PHY/bus/DBI reports, the early DPI plus
  idle-GDMA marker and the bounded panel-safe stop.  D1001 enumerates as
  `/dev/cu.usbmodem101` and independently identifies as ESP32-P4 v1.3, MAC
  `e8:f6:0a:e0:46:4c`.  No flash write has yet occurred; restricted `ota_0`
  write, digest verification, UART gate and direct phase observation remain.
- The grouped full-exact trial completed its technical visual-gate run.  Only
  `ota_0` at `0x20000..0x4efff` was erased and written; esptool's write-time
  hash and an independent `verify-flash` digest both matched the 190,800-byte
  artifact.  The 60-second run identified panel `93 65 04`, selected
  1,000-Mbit/s lanes, logged every grouped exact-lifecycle marker and started
  DMA/video/feed at the same `FB+0x1480`.  The scheduled phases advanced from
  1 through 10 at their documented seconds.  Through the final sample,
  bridge RAW remained zero, DMA faults remained zero, the source and FIFO
  moved, measured scanout was 69 MB/s and completed frames advanced from 5 to
  2,079.  Host `INT_ST1=0x80` and PHY turnaround `0x15bb` again coexisted with
  active internal counters but a black panel; they remain part of the failure
  signature rather than being reclassified as non-fatal.
  Teardown reached `scanout stopped, panel safe`; SD then enumerated as
  121,942 MB and FAT propagated `TD_PROTSTATUS=-1` as write protection.  The
  later missing-console alert is the known post-gate state.  Direct
  classification of solids, checker, coordinate card, isolated corners,
  isolated one-pixel lines and the combined image is still required before
  the visual result was recorded.
- Direct observation rejects that dynamic visual-gate run.  Three identical
  resets were observed locally; the display stayed completely black through
  all ten scheduled phases even though each phase was logged as written back,
  frame completion reached 2,079, scanout stayed at 69 MB/s and bridge/DMA
  fault status remained zero.  At that point the apparent difference from an
  immutable AROS pass still looked like the first live full-frame fill at
  `t0`; the next discriminator below invalidated that premise.  The general
  B5 visual gate remains open.
- `P4_B5_LIVE_UPDATE_GATE=1` is the next bounded discriminator.  It preloads
  the accepted asymmetric coordinate card before scanout, holds it unchanged
  for 15 seconds, then writes one unaligned 127x73 magenta rectangle and
  cleans the cache.  Only after another 15 seconds does it replace the whole
  frame with green, followed by blue at 45 seconds.  If the coordinate card
  appears and survives the small rectangle but vanishes at the full-frame
  fill, live bulk-write contention/frame handoff is isolated; disappearance
  at the small rectangle instead assigns the blocker to any unsynchronised
  write of the active burst buffer.  A black initial card would invalidate the
  presumed immutable AROS pass and return the audit to startup reproducibility.
- The live-update discriminator built cleanly after deleting every generated
  kernel object/dependency file.  The fresh 200/360-MHz XIP build used only
  the then-named `P4_B5_REFERENCE_PROFILE=1` (now
  `P4_B5_FULL_EXACT_TRIAL=1`), `P4_B5_LIVE_UPDATE_GATE=1` and the bounded
  60-second run, exited zero, contained no exact `error:`, passed SRAM
  residency and emitted the successful ESP32-P4 image marker.  Its
  190,544-byte core has SHA-256
  `9234e13191f49c7f1a4f3a1c280015001cee98bdf497c57973b7f9f3fff46a77`;
  ELF strings retain the exact PHY/bus/DBI, early DPI/GDMA and safe-stop
  markers.  Restricted flash write, independent digest verification, UART
  run and the three direct observations remain pending.
- The live-update discriminator also remained completely black, which
  invalidates the presumed immutable AROS pass rather than isolating cache or
  handoff.  Only `ota_0` at `0x20000..0x4efff` was written, and both write-time
  and independent verification matched the 190,544-byte artifact.  The run
  preloaded and cleaned the coordinate card before any DMA/host/bridge start,
  logged the magenta dirty rectangle at `t15`, the green full-frame fill at
  `t30` and the blue fill at `t45`, yet none was visible.  Internally the
  exact-lifecycle markers and identity `93 65 04` passed; scanout measured
  69 MB/s, frame completion reached 2,020, bridge RAW and DMA faults stayed
  zero, and teardown reached `scanout stopped, panel safe` before read-only SD
  startup.  The operator's direct classification was `alles schwarz` from the
  preloaded frame through all three updates.  Therefore neither active-buffer
  writes nor cache maintenance explain the black exact-burst path.  Vellum
  remains the only accepted 1,000-Mbit/s burst/ACK coordinate result, and the
  next AROS discriminator must return to startup/turnaround state before the
  first frame rather than proceed to B6.
- The misleading source name was corrected together with the evidence:
  `P4_B5_REFERENCE_PROFILE` is now `P4_B5_FULL_EXACT_TRIAL`.  A fully fresh
  rebuild under the corrected name exited zero, contained no exact `error:`,
  passed SRAM residency and produced the identical 190,544-byte image with
  SHA-256
  `9234e13191f49c7f1a4f3a1c280015001cee98bdf497c57973b7f9f3fff46a77`.
  The rename therefore changes no tested machine code; it prevents a rejected
  black configuration from being mistaken for the port's maintained profile.
- A temporary, fully reverted Vellum/IDF instrument captured the working
  command-to-video transition rather than another late register snapshot.
  The 2,382,048-byte reference image had SHA-256
  `3a0e867b355895a60e5bd053ca7fc82acd1616d01c5068c82f61accd8a2fcb46`;
  only `ota_0` was written on the identified v1.3 board and both write-time
  and independent digest verification passed.  After the ID read and after
  the vendor table it read
  `PCKHDL/CMD/PKT/PHY/INT1/VID = 0x1d/0x010f7f02/0x00050015/0x15bd/0/0x00010005`.
  Immediately after video mode plus AUTO the only change was PHY `0x15b9`;
  immediately after feed, video status changed from `0x00010005` to
  `0x00010004`, still with zero host error.  Thus retained BTA/ACK does not
  leave the working driver in receive direction.  Both the Vellum and pinned
  IDF source trees were restored clean after the capture.
- `P4_B5_REFERENCE_TRANSITION_TRACE=1` now records the identical six words at
  those four boundaries in AROS.  The first unequal boundary, not the later
  black frame alone, will select the next fix.  Run it additively with the
  rejected full-exact trial and immutable preloaded coordinate card; retain
  the 200/360-MHz baseline, bounded safe stop and read-only SD gate.
- The symmetric AROS run located the first unequal boundary.  Its fresh
  190,432-byte image had SHA-256
  `a3b77b1a1933ffb2bf832771a71d402d4e7bdf4805238454e0b1e69aa639f714`;
  only `ota_0` was written and independently verified.  After ID, AROS was
  identical to Vellum at `0x1d/0x010f7f02/0x50015/0x15bd/0/0x10005`.
  After the vendor table AROS alone changed command status to `0x60015` and
  PHY to receive-direction `0x15af`; video/AUTO and feed inherited
  `0x60015/0x15ab`, and feed failed to make Vellum's `VID_PKT_STATUS`
  transition from `0x10005` to `0x10004`.  The later result was the known
  persistent `INT_ST1=0x80`, `PHY=0x15bb` and black panel with moving
  69-MB/s DMA, zero bridge RAW and zero DMA faults.
- `0x60015` is not merely an ACK/BTA state: bit 17 says the host's internal
  buffered command FIFO is full, whereas Vellum's `0x50015` has bit 16 set,
  buffered-command empty.  Every previous settle attempt polled only the
  external generic command/payload FIFO bits 0..3, so those negative runs did
  not test this newly observed difference.  `P4_B5_BUFFERED_CMD_DRAIN=1`
  now waits at the sole handover boundary for buffered command and payload
  empty, bounded to 20 ms.  Reaching `0x50015/0x15bd` before video is the
  acceptance condition; timeout must abort B3 and return the panel safe.
- The bounded drain discriminator timed out exactly as designed.  Its fresh
  190,496-byte image had SHA-256
  `3b2f9064d14ad5312b0563591ed5c856c64098c32e7d37510ee5d63725d78720`;
  only `ota_0` was written on D1001 v1.3 and independently verified.  The
  25-second run matched Vellum through the post-ID boundary at
  `0x1d/0x010f7f02/0x50015/0x15bd/0/0x10005`, then timed out after the vendor
  table with `B3 stage two FAILED: a command fifo never drained`.  The panel
  was returned safe with the backlight never enabled, and the observer
  confirmed an entirely black display.  The machine continued through the
  read-only SD boot path, so this is a bounded DSI failure rather than a board
  hang.  A post-table wait cannot cure the state; the next discriminator must
  serialize or trace individual vendor writes to find the first command that
  fills the internal buffer.
- `P4_B5_SERIAL_COMMAND_DRAIN=1` is that next bounded discriminator.  It waits
  after every DCS write for both internal buffers to become empty, and on a
  20-ms timeout prints the triggering command plus packet, PHY and host-error
  state before B3 fails safe.  If all writes drain, the existing four-boundary
  trace must match Vellum at the post-table boundary before video is allowed.
- The serialized run found the first non-draining transaction: the tail-table
  one-parameter DCS `0x29`, not an arbitrary table overflow.  Its fresh
  190,864-byte image had SHA-256
  `3c46b0c31c51077556b7da5262ac11458052df7c6545bce29369a2941c0b6916`;
  only `ota_0` was written and independently verified.  All earlier writes
  drained, then `0x29` remained `PKT/PHY/INT1 = 0x40015/0x15af/0` after the
  bounded 20-ms wait.  Bit 1 distinguishes that PHY value from Vellum's live
  transmit edge: AROS is waiting in receive direction, without a reported
  host error.  B3 failed safe and the read-only SD boot path continued.
- A second temporary Vellum oracle measured that same command directly.  The
  2,382,048-byte reference image had SHA-256
  `6af767f33890875ae8f6ebf3bdaaee07967772f60b4cc84069d3d5664b1c37c2`;
  only `ota_0` was written and independently verified.  Immediately before
  the tail `0x29`, Vellum read `0x50015/0x15bd/0`; immediately after the MMIO
  write it already read `0x50015/0x15ad/0`, and after the specified 20-ms
  delay it was back at `0x50015/0x15bd/0`.  Its following `0x35` made the same
  clean `0x15ad -> 0x15bd` excursion.  Therefore AROS's timeout is not a short
  wait and `0x60015` was only the queued `0x35` hiding the still-active
  `0x29`.  The Vellum instrumentation was removed and both external source
  trees were verified clean.  The next test must change the `0x29` turnaround
  itself while restoring the reference ACK/BTA register state before video;
  repeating generic FIFO waits or per-command yields is excluded.

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
