# Native LLVM SDK source contracts

`aros-targets.toml` selects one `sdk_consumer_contract` for each LLVM target.
These declarations belong to the AROS source: `aros-tools` executes them and
`aros-toolchains` distributes the resulting target-specific SDKs with compiler
artifacts. An SDK is a Developer sysroot, not a host compiler or boot medium.

Each profile declares:

- The complete `core-linklibs`, `includes` and `linklibs` graph roots.
- Its compiler triple, ABI and explicit compiler-option policy.
- Sealed MetaMake inputs, native configuration and exact optional selector hooks.
- C and C++ application probes for ordinary original and relocated SDK links.

Profiles do not infer compiler flags from their names. The ARM selection follows
the source's `armv7-a+fp`/hard-float configuration. x86-64 explicitly disables the
red zone; AArch64 explicitly disables PIC and PIE. A matching triple or successful
contract parse does not prove runtime ABI compatibility.

The shared native configuration preserves recipe-local `ISA_FLAGS` through
`TARGET_ISA_CFLAGS`, before ordinary `TARGET_CFLAGS`, as in classic Make.
These source feature flags are separate from the selected compiler/ABI role.

The x86-64 SDK explicitly declares an i386 companion role for its `lib32`
objects. `cpu32` selects the source recipes; it does not infer a compiler ABI.
The companion descriptor names the driver triple, ELF class/machine/byte order,
separate library root and required compiler-runtime library. Both roles use
the same selected LLVM compiler package. Compiler execution and measured ELF
outputs remain qualification gates, not facts proved by this descriptor.

## Authoring checks

Run from the source root:

```sh
python3 compiler/sdk/test_export.py
python3 compiler/sdk/export.py --check
```

After reviewing a deliberate source or declaration change, refresh its input
hashes and check again:

```sh
python3 compiler/sdk/export.py --update
python3 compiler/sdk/export.py --check
```

The exporter changes only sealed input measurements and ordering. It preserves
graph roots, ABI policy, selectors and probe declarations. It refuses duplicate
or ambiguous declarations, unsafe/aliased source files and reduced SDK roots.
The Rust loader and strict graph audit remain additional required checks.

Optionality names exact virtual MetaMake selector edges and their source owners.
It does not excuse a missing mandatory producer, unsupported generator or
transpiler capability. In particular, the nonvirtual `rom/exec` include edge
cannot be waived.

## Qualification

These contracts are development inputs, not published SDK qualification.
Qualification requires a clean pinned source, strict complete graph, actual SDK
generation, complete compiler/runtime inputs, four ordinary C/C++ link probes,
relocation and output-ABI checks, independently verified package bytes, and
authenticated release evidence. A diagnostic-only graph report must not be
treated as a successful SDK build.
