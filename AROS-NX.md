# AROS-NX integration model

AROS-NX is a non-destructive integration fork of
[upstream AROS](https://github.com/aros-development-team/AROS). Its permanent
branches have deliberately different jobs:

- `master` is an exact, fast-forward-only mirror of upstream `master`;
- `main` is upstream plus reviewed AROS-NX patches;
- `pr/<subsystem>-<topic>` starts at `master` and carries one upstreamable
  patch series;
- `feature/*`, `fix/*` and `docs/*` start at `main` for AROS-NX integration;
- `sync/upstream-<12sha>` is an ephemeral automated merge proposal into `main`.

The daily and manual `Propose upstream sync` workflow never force-pushes or
rebases a permanent branch. It stops if upstream rewrote history, if `main`
does not contain the previous mirrored commit, if a proposal for the same
upstream commit already exists, or if the merge conflicts. A successful run
fast-forwards `master`, merges that immutable commit into a new sync branch and
opens a pull request. Branch protection—not the sync job—decides whether the
proposal may enter `main`.

## Required repository configuration

The workflow requires a dedicated fine-grained `AROS_SYNC_TOKEN` secret with
contents write and pull-request write access to this repository. It is not
optional: pull requests created with the ordinary workflow token do not trigger
the normal event-driven qualification workflows.

Rulesets should enforce all of the following:

1. `master` rejects deletion, force-pushes and direct writes except
   fast-forwards from the dedicated sync identity.
2. `main` rejects deletion and force-pushes and requires pull requests,
   resolved review conversations and the complete product qualification
   checks. Add mandatory independent approvals when a second maintainer can
   supply them without making the repository permanently unmergeable.
3. `pr/*` and `sync/*` reject force-pushes after review begins.
4. Administrators do not bypass failed required checks for routine syncs.

The sync workflow deliberately does not enable automatic merge. A clean Git
merge proves only that histories combine; it does not prove that AROS-NX still
builds or behaves correctly on every supported target.

AROS-NX permits only merge commits for pull requests. Squash and rebase merges
would discard the original upstream commit as an ancestor of `main`, breaking
the invariant that the next sync verifies before it changes anything.

## Cross-toolchain release baseline

`aros-toolchains.lock.toml` pins the stable
[`toolchain-v1-20260924` release](https://github.com/metaneutrons/aros-toolchains/releases/tag/toolchain-v1-20260924).
It enables the nine LLVM 11 artifacts for Linux x86-64, Linux AArch64, and
macOS ARM64 across `pc-x86_64`, `arm-raspi`, and `rpi-aarch64`. Intel macOS is
not a release target; RISC-V remains disabled until separately qualified.

Release qualification completed 18 native builds, nine byte-identical
comparisons, and nine compatibility/relocation lanes. A macOS ARM64 consumer
smoke test installed and verified the published `pc-x86_64` artifact and
compiled freestanding C and C++ objects for `x86_64-unknown-aros`. This is not
evidence of a complete AROS distribution build or a hardware boot test.
