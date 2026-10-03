# Working rules for the ESP32-P4 port

Read `README.md` and `ROADMAP.md` before changing this port.  `README.md`
contains verified history and board facts; `ROADMAP.md` is the authoritative
forward plan and evidence record.

## Mandatory progress documentation

Any change that starts, advances, blocks, invalidates or verifies a roadmap
step must update the documentation in the same change:

1. update the master progress table and affected phase in `ROADMAP.md`;
2. append a dated evidence entry with artifact identity, configuration,
   procedure, observed result, safety impact, remaining risk and next step;
3. update the README status/milestone text when externally visible status
   changed.

Do not label a feature `done` or `hardware verified` from a build or static
audit.  Only the complete acceptance gate observed on the D1001 permits that
state.  Keep failed experiments and corrections documented instead of erasing
them.  If a change has not reached hardware, use at most `build verified`.

This rule also applies when the implementation change is in shared AROS code
outside this directory but advances an ESP32-P4 roadmap phase.

## Workarounds are not fixes

An accepted workaround must remain named as a workaround in code, the master
progress table, the affected acceptance gate and the evidence log.  Record the
observed defect, what the workaround compensates, which root cause remains
unknown and a separate roadmap item with an uncompensated removal test.
Hardware acceptance of compensated output verifies only the workaround.  It
must never close the root-fix item or silently become a permanent hardware
contract.

The same rule applies to test-gate substitutions.  A forced peripheral reset
may unblock development on this battery-backed board, but it is not evidence
of rail-off behavior and must not be counted as a physical cold boot.

## Builds whose result gets documented

mmake does not invalidate objects when a `-D` flag or a mmakefile changes.  A
build command therefore does not determine what comes out of it: an object
compiled once without a diagnostic switch is reused when the switch returns.
This has already produced a flashed core that did not contain the diagnostics
its command line asked for, and a package carrying a module built without the
debug output the command line requested.

Since every evidence entry records the configuration it was produced with, a
stale object makes that record false, and a test can pass or fail for the
wrong reason.  So before any build whose outcome is going to be documented,
delete the objects that the changed switch affects:

    find <build>/bin/<target>/gen/rom/kernel -name "*.o" -delete    # kernel
    find <build>/bin/<target>/gen/rom/dos -name "*.o" -delete       # dos

and state the artifact size and SHA-256 in the entry, because those are what
show which build actually reached the board.

## Capture the console from the first byte

Use `tools/reset-and-log.py`, not esptool followed by a reader:

    uv run --with pyserial python arch/riscv-esp32p4/tools/reset-and-log.py \
        /dev/cu.usbmodem1101 12

esptool has to own the serial port to reset the board, and by the time it
releases it and something else opens it, the early output is already gone.  A
boot that loads the module package prints around 60 KB against a much smaller
USB CDC buffer, so everything before exec - the clock report, the whole PSRAM
bring-up - is overwritten unread.  This script holds the port open and asserts
the reset over the control lines instead.

That is not a convenience.  Findings in this port have been measured twice
because a first capture silently missed the lines that mattered, a diagnostic
was moved into a later report only so it could be read, and a `SW_SYS_RESET`
loop was misattributed for the same reason.  A measurement you cannot reliably
read is not a measurement.

## Confirm availability before interactive hardware tests

The user often works remotely and cannot necessarily see or touch the D1001.
Before starting any run whose acceptance or diagnosis depends on a live user
observation, touching the panel, inserting or removing the SD card, pressing a
button or reconnecting a cable, first describe the exact interaction and wait
for the user's explicit confirmation that they are ready.  Standing flash
authorization does not waive this synchronization step.  A run started before
that confirmation is non-interactive evidence only and must not be counted as
an interactive acceptance test, even if the user later reports that they
missed it.

## Comparing against the working reference

When a hardware question survives several rounds of elimination, stop adding
candidates and measure the difference against firmware that works.

The vendor's own firmware runs on this board and its source is available.  The
procedure that closed the PSRAM blocker: a branch in that tree, a read-only
dump of the registers involved printed where the working path has already
succeeded, and a comparison against the same set printed where this port's has
failed.  One register differed, and it was one nothing in ESP-IDF's own P4
sources pointed at - `PERIF_I2C_RSTB`, released by IDF only in the C5 and C61
bootloader ports.  Eight hypotheses had been eliminated before that, each
costing a build, a flash and a run; the comparison answered it in one.

Two conditions make it worth reaching for:

  - **A read-only dump.**  Print registers, write none.  The reference
    firmware's behaviour has to stay the behaviour being compared against.
  - **A branch, and cleaned up afterwards.**  The diagnostic belongs in that
    tree's history, not in its main line, and the change is the owner's to
    keep or drop.

The inverse also holds and is worth stating: a value-by-value comparison of
this port's code against ESP-IDF's found nothing, twice, because the fault was
a register outside the sequence being compared.  Reading the reference's source
tells you what it writes; reading the reference's *state* tells you what it has.

## One disturbance at a time, and one variable at a time

Four findings were recorded and later withdrawn in a single session, and every
one of them was a measurement taken while something known to be wrong was still
in the path, or a measurement whose window could not have shown what it was
asked about:

  - frame acknowledge was recorded as stopping the host, measured while
    `ACK_RQST_EN` had the link turned around;
  - the pixel depth was recorded as 24-bit from an image that appeared in a
    change that also raised the backlight from 20 per cent to 100;
  - the DPI clock was recorded as the reason burst mode would not start,
    without burst having been tried at the other clock;
  - the bridge was recorded as reading through vertical blanking, from a 5 ms
    rate window inside a 14.6 ms frame - which measures the active rate by
    construction and cannot see blanking at all.

Each cost a build, a flash, a run and a round of reasoning built on top of it,
and each had to be unpicked from the documentation afterwards.  So:

  - **Fix the known fault before measuring past it.**  A run with a defect
    still in the path measures the defect.
  - **One variable per run.**  Two changes and one observation yield no
    attribution, however obvious the answer looks.
  - **Check the window against the thing being measured.**  A rate window
    shorter than a frame measures the active rate; a status register read
    before the event reports the state before the event.
  - **Prefer an instrument over a photograph.**  DCS `0x0A` read *after* the
    video handover distinguishes a panel that accepts the stream from one that
    does not, and needs no camera and no interpretation.  Two rounds of this
    phase were spent reading camera artefacts as panel behaviour.

## Read the firmware that ships, not the component's test application

The vendor component for this panel carries a test application configuring 24
bits per pixel at 80 MHz over 1500 Mbit/s lanes, and a header macro giving a
vertical back porch of 12.  The firmware that actually drives this board -
`~/Source/Vellum`, `firmware/components-lcd/lcd_jd9365/lcd_jd9365.c` and
`components-lcd/d1001_board/include/d1001_board.h` - configures RGB565 at 40
MHz over 1000 Mbit/s with a back porch of 30.  Four values were changed to the
test application's and all four were wrong.

A component's test application demonstrates the component.  A board's firmware
is the configuration that hardware is known to run.  When both are available,
the second is the reference; when they disagree, that disagreement is itself
worth recording rather than resolving by preference.

## Check that the build succeeded, not that a pattern is absent

The build invocations in this work filtered with `grep -icE " error"`.  The
SRAM-residency check reports

    error: .sramtext refers to the XIP flash window

which has no leading space, so a failed link reported zero errors.  The image
was not regenerated, a stale core was flashed, and its behaviour was recorded as
a measurement of the code that had just been changed.  That happened several
times in one session and produced a finding - "no marker appears, so the hang
precedes them" - that meant nothing at all.

Filter on `error:`, and confirm the artifact: the line
`Creating .../aros-esp32p4.bin` has to appear, and its size and hash have to
differ from the previous build when the change was not cosmetic.  A build whose
output binary is byte-identical after a real change did not rebuild it.

## Instrumenting SRAM-resident code

The PSRAM bring-up runs from `.sramtext` with interrupts off, and a hang inside
it produces no output at all.  Two obvious ways to mark its progress do not
work, and both cost a build here:

  - calling `krnP4PutStr`, which lives in flash on an XIP build;
  - passing it a string, because the literal lands in flash `.rodata`.

`check-sramtext.sh` rejects both.  What works is a single character: it is an
immediate in the instruction stream and needs no storage.  `psram_mark` in
`psram_init.c` duplicates `krnP4PutC`'s mechanism - poll the endpoint's
data-free bit, write the byte, mark the transfer done - which takes no
interrupts and touches two registers.  It is deliberately a copy: making the
console SRAM-resident would move it into a 40 KB budget for a diagnostic.

Validate an instrument on hardware that works before trusting it on hardware
that does not.  `P4_PSRAM_TRACE=1` prints `123456` on a good boot; a truncated
run names the stage that hung.

## Active-scanout reset continuity

The GDMA survives a CPU reset, exactly as the PMU and the clock dividers do.  A
scanout left running is still reading PSRAM over AXI while the next boot's
bring-up reconfigures the controller, and that has made this board unbootable
twice.  Recovery took flashing the vendor firmware to `ota_0`; a power cycle did
not substitute for it, plausibly because the LP domain holding the PMU registers
is battery-backed on this board.

`P4_SCANOUT_SECS` bounds it, and `krnP4ScanoutQuiesce` stops whatever the last
boot left running before the PSRAM is touched.  PSRAM mode-register commands
are bounded and reset both PSRAM FSMs after a timeout; the deterministic
`P4_PSRAM_TIMEOUT_TEST=1` run proved that a timed-out command can be followed by
a complete 200 MHz bring-up.  The quiesce also disables the DSI pixel clock and
bridge in Espressif's teardown order before pulsing the chip-level DSI reset.

One USB reset taken during a measured 69 MB/s scanout recovered all 32 MB, the
DSI identity reply and a second measured scanout without vendor firmware or a
power cycle.  A later exact-artifact repeat exposed an independent diagnostic
hazard: stopping video for a post-video DCS 0x0A read can leave
`GEN_RD_CMD_BUSY` set when the panel does not reply.  Clearing BTA does not
abort it, and restarted video then consumes no pixels.  That read is therefore
off by default and may only be enabled deliberately with
`P4_DSI_POST_VIDEO_QUERY=1`; do not use it in reset or scanout acceptance runs.
Two immediate default-off repeats, including recovery from an already-retained
busy state, then completed with moving DMA.  Routine USB reset is no longer
prohibited merely because bounded B5 scanout may still be active.  Ten warm
starts have passed; the separately named forced-peripheral-cold-state
development gate remains open, and physical cold cycles remain a distinct C3
obligation.  Continue to record PSRAM, command-path and live DMA evidence
separately rather than treating a DCS reply as proof of scanout continuity.

The sustained B5 gate is `P4_B5_CONCURRENT_STRESS=1`.  It must use the existing
read-only `sdcard.device`, keep the fixed framebuffer outside Exec's allocatable
PSRAM, force CPU verification reads through external memory, and run at least
1,800 seconds.  A smoke build is harness validation only.  Accept a sustained
run only when the log contains every `t0..t1799` sample, zero bridge/host error
status, 1,800 referenced SD reads, 1,800 complete 1 MB PSRAM passes, the final
zero-failure `PASSED` line and the bounded panel-safe stop.  If a retained
scanout state makes PSRAM identify fail, record that boot as a boot-cycle
failure; a Vellum recovery boot may restore the development board but cannot be
counted as AROS recovery or as part of the stress gate.

## Safety boundaries

- Keep SD media read-only through the first graphical boot.  Preserve both
  the command denylist and the absence of a FIFO transmit path.  DOS/FAT must
  also expose write protection and reject mutation without dirty cache or an
  interactive retry requester.
- Do not enable CMD18 until CMD12 and full error recovery pass A1.
- Do not run automatic partition discovery until the bounded A2 gate passes.
- Keep `dosboot.resource` out of the package until A1 and A2 pass; it takes
  over during COLDSTART and makes the current post-`krnStartExec()` diagnostics
  unreachable.
- Do not start PSRAM-backed DSI scanout on the 20 MHz fallback path.
- Keep panel reset asserted and backlight dark on every display-init failure.
- Use explicit flash offsets and do not touch bootloader, partition table or
  `storage` unless the task specifically authorizes that exact write.

## JC1060P470C development-board flash authorization

Fabian made the Guition JC1060P470C the active development board on
2026-10-03 and granted the same standing authorization as for the D1001,
for the board with MAC `80:f1:b2:d3:3b:a6` (ESP32-P4 v1.3, 16 MB flash):

- a verified AROS core to `ota_0` at `0x20000`;
- a verified BSP package to `arosbsp` at `0x820000`;
- a verified development volume to `arosbsp` at `0xc00000`.

The same rules apply as below: identify the device by MAC, verify artifact
hashes, report exact offsets in the roadmap evidence. Bootloader, partition
table, `otadata`, NVS and `phy_init` stay explicit-write targets.

The same message authorized the one-time provisioning that gives the whole
16 MB to AROS: AROS bootloader to `0x2000`, the board's partition table to
`0x8000`, erasing `otadata` (`0x10000`-`0x11fff`, where the vendor app
began), core, package and volume. The factory image is backed up first and
kept: `ESP32P4-board-backups/jc1060p470c-80f1b2d33ba6-factory-16MB.bin`,
SHA-256 `03222de1887e5368a964c483c666ee71f66e4941a573974e95ae678587eed1df`,
read twice and identical. Restoring it is a full `write-flash 0x0` of that
file; the board's BOOT and RESET buttons force the ROM loader if needed.

## D1001 development-board flash authorization

On 2026-08-22 the user granted standing authorization for routine bring-up
writes to the connected D1001 development board, extended on 2026-08-23 to
cover the whole of `arosbsp`.  Do not request a fresh confirmation before
writing:

- a verified AROS core to `ota_0` at `0x20000`;
- a verified BSP package to `arosbsp` at `0x820000`;
- a verified development volume to `arosbsp` at `0xc00000`.

The last two share one partition, and the split between them is
`P4_FLASHDISK_PART_OFFSET` in `kernel/kernel_intern.h`.  The package must stay
below it, which `kernel-package-esp32p4-riscv-checksize` asserts; run that
before writing a package, because a package over the split would have the
loader read filesystem bytes as members.

Before each write, still identify the serial device, verify the artifact
hashes and report the exact offsets in the roadmap evidence.  This standing
authorization does not include bootloader, partition table, `otadata`, NVS or
`storage`; those remain explicit-write targets.

The development volume is mounted as `FLASHDISK0P0:` by default, from a boot
node `flashdisk.device` registers at priority -10.  That is below the SD
card's, so a present card still wins the boot and the flash volume is only
the fallback; `bootdevice=FLASHDISK0P0` on the kernel command line forces it,
and `P4_NO_FLASHDISK_BOOTNODE=1` leaves it unregistered.  Writing test
content to it is a flash write to `0xc00000` and therefore covered above, but
it does not replace a card run: a change that touches the storage stack has
to be shown on the card too, because the two go through different devices and
different FAT widths.
