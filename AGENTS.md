# Repository working rules

## Pinned aros-tools runtime

Treat `scripts/ci/install-aros-tools.sh` as part of every change that adds or
changes MetaMake recipes, transpiler capability fingerprints, locked-source
formats, or other syntax or capabilities consumed by `aros-tools`.

Before pushing such a change:

1. Determine the minimum released `aros-tools` version that understands it.
2. Compare that version with the CI runtime pin.
3. If necessary, update the version plus every platform size and SHA-256 in
   `scripts/ci/install-aros-tools.sh` in the same pull request.
4. Run the affected contract tests and a real installer probe against the
   pinned release archive.

A successful test against a local `aros-tools` checkout does not verify the CI
runtime: CI executes the released archive pinned by the installer.

## ESP32-P4 bring-up

Before any change intended to advance, block, invalidate or verify the
ESP32-P4 port, read `arch/riscv-esp32p4/README.md`, `ROADMAP.md` and the
port-local `AGENTS.md`.

The roadmap and its evidence log must be updated in the same change as the
implementation progress.  This applies even when the implementation is in a
shared directory such as `rom/`, `workbench/` or `compiler/`, outside the port
directory.  A build or source audit is not hardware verification; only the
documented D1001 acceptance gate permits that state.
