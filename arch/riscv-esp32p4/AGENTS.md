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
