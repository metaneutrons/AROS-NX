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
does not contain the previous mirrored commit before a new proposal, or if the
merge conflicts. A successful run
fast-forwards `master`, merges that immutable commit into a new sync branch and
opens a pull request. Branch protection—not the sync job—decides whether the
proposal may enter `main`.

An existing integration PR is reported as pending when `master` already mirrors
its upstream commit but `main` does not contain it. The workflow does not claim
that a pending proposal was integrated. Manual runs use a repository dispatch
so the credential-bearing workflow always executes from the protected default
branch, never from an operator-selected branch:

```sh
gh api -X POST repos/metaneutrons/AROS-NX/dispatches \
  -f event_type=propose-upstream-sync
```

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

## Product CI

`AROS-NX CI` owns filename and line-ending hygiene, locked source preparation,
and the nine Linux/macOS product lanes. Pull requests with only Markdown file
changes run the lightweight gates; all other pull requests run the full product
matrix. A protected merge to `main` does not repeat the already qualified
matrix. Manual dispatch always runs it. The required `CI Success` check verifies
the selected gates, including the source-preparation job, so a failed or
skipped prerequisite cannot be mistaken for a successful product build.

The matrix uses hash-pinned native archives from the published `aros-tools`
release. `scripts/ci/product-sources.plan.json` records each locked source and
`scripts/ci/product-sources.staging.json` maps it into a fresh build tree.
The Linux source job exports only measured files from the fetch cache; every
product lane verifies them again before CMake configuration. Add new product
sources to these contracts rather than adding another source-specific workflow
job. Intel macOS remains outside the qualified host matrix.

## Cross-toolchain release baseline

`aros-toolchains.lock.toml` pins the stable
[`v0.1.4` release](https://github.com/metaneutrons/aros-toolchains/releases/tag/v0.1.4).
It enables the nine LLVM 11 artifacts for Linux x86-64, Linux AArch64, and
macOS ARM64 across `pc-x86_64`, `arm-raspi`, and `rpi-aarch64`. Intel macOS is
not a release target; RISC-V remains disabled until separately qualified.

Release qualification completed 18 native builds, nine byte-identical
comparisons, and nine compatibility/relocation lanes. Its 44 immutable assets
passed independent checksum, package-tree, SBOM, provenance, and public-URL
verification. A macOS ARM64 consumer smoke test installed and verified all
three released profiles from their public URLs. This is not evidence of a
complete AROS distribution build or a hardware boot test.
