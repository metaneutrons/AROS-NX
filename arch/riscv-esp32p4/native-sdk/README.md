# Native ESP32-P4 GNU SDK consumer

This source-owned contract selects the complete ordinary `includes` and
`linklibs` MetaMake roots for `esp32p4-riscv-gnu`. Its separate compiler
package profile is `rv32-esp32p4`. It does not build a kernel, bootloader or
boot medium, deploy to a board or run a hardware test.

The declaration is not a published SDK. It requires an aros-tools runtime
with native-consumer-contract support; the published v0.3.19 runtime does not
have that capability. Source integration, a new qualified tools runtime and
the complete compiler/SDK release gates must pass before release activation.

The SDK is target/ABI-specific, not host-specific. Its three host projections
select host generators, not different target ABIs. Host-independent SDK bytes
still require cross-host comparison evidence. A compiler package is separately
selected for the actual host; installing it does not install this Developer SDK.

The configuration follows `configure.in`: RV32
`rv32imafc_zicsr_zifencei_zaamo_zalrsc`, `ilp32f`, `medany`, standalone,
MMU off and the mandatory SMP variant. Platform SMP and Exec SMP are separate
facts; the generated Exec include configuration explicitly enables the latter.
The generic `config/kobj-romtag.ld` contract is retained.

`AROS_TOOLCHAIN_RELEASE=0` and `ports-includes` retain the full SDK closure.
A reduced producer-only SDK, omitted required producer or pre-exported graph
cannot qualify this consumer. The transpiler independently discovers and
checks the source recipes using the sealed MetaMake invocation projection.

The native configuration binds compiler/archive roles supplied by the engine,
not ambient PATH programs. It uses the build's explicit Developer sysroot.
The existing host `genctbl` generator uses hash-verified Unicode 16.0.0 inputs
and generates the real locale table; no pregenerated substitute is supplied.

The SDK includes shared kernel headers that require a board macro. Its
configuration explicitly selects the existing `d1001` source default and
seals `board/board.mk`, which derives the flags. This is a header compilation
context, not board support inferred from the compiler or a hardware claim.
Other board choices, watchdog/PSRAM/XIP settings, partition maps and
kernel/media roots belong to a separate source-owned board build contract.

The empty `INCEMPTYDIRS` configuration is explicit: classic Make expands that
undefined variable to empty, while the native engine refuses an unbound value.

The private GSM link library explicitly exports no SDK headers. Its empty
`%copy_includes` list preserves the classic Make result without treating an
unknown variable as proof of an empty native producer.

Contract v2 also seals two ordinary application-link fixtures in
`native_sdk_link_probes`: one C source and one C++ source. Each path must name
the corresponding `.c` or `.cpp` file in the measured `inputs` list. The C
fixture includes the public Exec list and proto headers and calls `NewList`;
the C++ fixture uses the same SDK API and `std::string` out-of-line operations.
Both declare an empty additional `libraries` list deliberately: the eventual
probe uses the measured language driver and must retain its normal startup and
standard-runtime defaults. The list is only for ordered, explicit extra SDK
libraries; it does not replace driver defaults.

The source loader verifies the probe declarations and their input seals. That
is not evidence that the SDK was built or that either fixture linked. Native
execution, relocated SDK inventory/replay, retained receipt read-back and
release admission remain separate TC0 gates.

The contract records selector-extension edges in the SDK root, startup and
GNU libatomic recipes for source-binding checks. It uses the transpiler's
normalized selector names, not the similarly named Classic Make variables.
No active MetaMake dependency is omitted: even an optionality declaration
cannot erase an active edge with a missing producer. Exec's mandatory
platform/CPU header prerequisites remain ordinary graph dependencies, not
optional declarations. Unsupported producers and missing dependencies remain
errors. The generic header, startup and library producers stay selected.

After reviewing changed source inputs, refresh and check their seals:

```sh
python3 arch/riscv-esp32p4/native-sdk/export.py --update
python3 arch/riscv-esp32p4/native-sdk/export.py --check
python3 arch/riscv-esp32p4/native-sdk/test_export.py
```

The exporter changes measured input hashes only, not ABI, roots or policy.
`source_baseline` identifies the declaration's starting point, not the
modified checkout. Actual qualification must record the clean source commit,
compiler package identity, full native build, relocation and comparison
evidence. Declaration validation is not compiler, firmware or hardware proof.
