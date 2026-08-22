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
writes to the connected D1001 development board.  Do not request a fresh
confirmation before writing a verified AROS core only to `ota_0` at `0x20000`
and its verified BSP package only to `arosbsp` at `0x820000`.  Before each
write, still identify the serial device, verify the artifact hashes and report
the exact offsets in the roadmap evidence.  This standing authorization does
not include bootloader, partition table, `otadata`, NVS or `storage`; those
remain explicit-write targets.
