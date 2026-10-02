[![AROS — source, tools and toolchains](https://aros.metaneutrons.cc/social-card.png)](https://aros.metaneutrons.cc/)

# <img src="https://metaneutrons.cc/brand/aros.svg" alt="" width="40" height="40"> AROS-NX

**An upstream-tracking AROS fork, built with [aros-tools](https://github.com/metaneutrons/aros-tools).**

[Documentation](https://aros.metaneutrons.cc/aros-tools/) ·
[Build CI](https://github.com/metaneutrons/AROS-NX/actions/workflows/ci.yml) ·
[Upstream AROS](https://github.com/aros-development-team/AROS) ·
[Issues](https://github.com/metaneutrons/AROS-NX/issues)

## Purpose

AROS-NX integrates the [AROS operating system](https://aros.sourceforge.io/)
with a native Rust toolsuite and a translated CMake/Ninja build. It is an
independent integration fork, not the official upstream repository.

The goal is to **keep AROS-NX buildable with aros-tools while staying close to
upstream**. New upstream changes must pass the tools-based product checks before
entering `main`. Source fixes that also apply to classic AROS should be prepared
as focused patches and submitted upstream; integration-specific build metadata
stays here. The aim is a smaller patch delta, not a separate operating system.

## Three repositories, separate responsibilities

| Repository | Responsibility |
| --- | --- |
| **[AROS-NX](https://github.com/metaneutrons/AROS-NX)** — this repository | Operating-system sources, reviewed source patches, target/media profiles and the consumer toolchain lock. |
| **[aros-tools](https://github.com/metaneutrons/aros-tools)** | The `aros` CLI, MetaMake transpiler, embedded CMake engine, source fetching, build orchestration, verification and boot/media workflows. Start here for installation and usage. |
| **[aros-toolchains](https://github.com/metaneutrons/aros-toolchains)** | Locked compiler inputs, reproducible qualification and published cross-toolchain releases, produced with aros-tools. |

AROS-NX does not carry a second copy of the host tools or the CMake engine.
`aros-tools` reads the checkout's profiles and
[`aros-toolchains.lock.toml`](aros-toolchains.lock.toml), installs the selected
compiler and builds this source tree. Tools and compilers have independent
release versions; CI pins the tools runtime and the lock pins compiler artifacts.

## Build with aros-tools

Install the **complete [aros-tools release suite](https://aros.metaneutrons.cc/aros-tools/getting-started/installation/)**
and the [host prerequisites](https://aros.metaneutrons.cc/aros-tools/getting-started/prerequisites/).
Then follow the [AROS-NX quick start](https://aros.metaneutrons.cc/aros-tools/getting-started/quick-start/)
to create a recursive source checkout, install its locked toolchain and build a
selected preset. You do not need to build the cross-compiler yourself.

The product matrix covers `pc-x86_64`, `arm-raspi` and `rpi-aarch64` on Linux
x86-64, Linux ARM64 and macOS Apple silicon. macOS Intel is not a release target.
Passing product CI is **not** a hardware-boot or complete-distribution claim.
See the [current release status](https://aros.metaneutrons.cc/aros-tools/reference/release-status/)
and [image commands](https://aros.metaneutrons.cc/aros-tools/reference/cli/#composed-image-artifacts-experimental)
for the separately qualified capabilities.

## Upstream synchronization and patches

- **`master`** is the fast-forward-only mirror of upstream `master`.
- **`main`** combines upstream history with reviewed AROS-NX integration changes.
- **`sync/upstream-<12sha>`** branches propose upstream updates as pull requests.
- **`pr/<subsystem>-<topic>`** branches start from `master` for focused,
  upstream-compatible patch series.

The synchronization workflow runs daily and can also be requested manually.
It opens a proposal; it does not merge unchecked changes. A clean Git merge is
not enough: source changes and upstream syncs require the tools-based product
matrix. If upstream exposes a missing build capability, fix the generic tools
implementation or the relevant source defect before accepting the update.

Pull requests use merge commits to preserve upstream ancestry. Keep source
fixes separate from AROS-NX-specific integration work so they can be reviewed
and submitted upstream without importing the fork's build configuration.
See [the integration model](AROS-NX.md) for the complete branch and sync contract.

## Contribute

Report source/build problems in [AROS-NX issues](https://github.com/metaneutrons/AROS-NX/issues),
host-tool problems in [aros-tools issues](https://github.com/metaneutrons/aros-tools/issues),
and compiler-release problems in [aros-toolchains issues](https://github.com/metaneutrons/aros-toolchains/issues).
Include the source commit, tools version, preset, host and retained failure logs.

For contributions to classic AROS, consult the
[upstream contribution guide](https://github.com/aros-development-team/AROS/blob/master/CONTRIBUTING.md).

## License and acknowledgements

AROS is licensed under the [AROS Public License](LICENSE). Third-party components
retain their respective licenses. See [ACKNOWLEDGEMENTS](ACKNOWLEDGEMENTS) and
the notices in the source tree. This fork builds on the work of the AROS
Development Team and its contributors.
