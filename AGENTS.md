# Repository working rules

## ESP32-P4 bring-up

Before any change intended to advance, block, invalidate or verify the
ESP32-P4 port, read `arch/riscv-esp32p4/README.md`, `ROADMAP.md` and the
port-local `AGENTS.md`.

The roadmap and its evidence log must be updated in the same change as the
implementation progress.  This applies even when the implementation is in a
shared directory such as `rom/`, `workbench/` or `compiler/`, outside the port
directory.  A build or source audit is not hardware verification; only the
documented D1001 acceptance gate permits that state.
