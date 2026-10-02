# ESP32-P4 / AROS SMP handoff to Claude Code

Date: 2026-10-02. Fabian requested this handoff and explicitly stopped Codex.
No further implementation, build, flash or hardware test should be inferred
from this document. The SMP objective is **unfinished**, not hardware accepted.

## Start here: exact working paths

Start Claude Code in the real source checkout:

```sh
cd /Volumes/Dev/Source/Amiga/AROS
claude
```

| Purpose | Absolute path |
|---|---|
| Source checkout / working directory | `/Volumes/Dev/Source/Amiga/AROS` |
| Port sources | `/Volumes/Dev/Source/Amiga/AROS/arch/riscv-esp32p4` |
| Existing normal build and toolchain | `/Volumes/Dev/Source/Amiga/AROS-ESP32-build` |
| Latest SMP integration evidence | `/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-priority-2026-10-01` |
| Earlier normal firmware / runtime evidence | `/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-e3-lock-runtime-2026-10-01` |
| Reserved future isolated SMP build path, not configured/qualified | `/Volumes/Dev/Source/Amiga/AROS-ESP32-SMP-build` |

Do **not** work in the stale conversation directory
`/Volumes/Dev/Source/AROS-ESP32`. It exists but is not this checkout.
`/Users/fabian/Source` points to the case-sensitive `/Volumes/Dev/Source` tree.

Read before editing: root `CLAUDE.md`, root `AGENTS.md`, then port `AGENTS.md`,
`README.md`, `ROADMAP.md` and `SMP.md`. ROADMAP is the authoritative plan/evidence;
its master table, affected phase and dated evidence must change together with
implementation. README changes only for an externally visible milestone.
ROADMAP is very large (over 23,000 lines); its latest dated entries are at the
bottom before the evidence template. This handoff does not override its rules.

Branch: `feat/riscv32-esp32p4-v2`.
HEAD at handoff: `ad62d8257ed34ce0832fbe9fb1bf98466d9a784f`.
The checkout is extensively dirty and has many untracked port/test files.
The latest integration is **not committed**. Preserve everything, including
unrelated Raspberry Pi changes and the source-tree `build/` evidence directory.
Do not reset, clean, stage everything or discard files to obtain a clean build.

## User instructions and hardware safety

- Call the user Fabian; concise German, no praise. Git messages in English,
  no AI coauthor trailer.
- Routine D1001 flashing and headless UART tests have standing authorization.
  Verify device identity, artifact hashes, exact ranges and recoverable backup.
- **Sight/touch/SD insertion/removal/button/cable tests require fresh readiness.**
  Describe the exact interaction and wait for explicit `bereit`. Reconnection
  does not establish readiness. Fabian recently said the display is connected;
  no visual acceptance was obtained during these source-only SMP changes.
- Preserve read-only SD behavior. Do not touch bootloader, partition table,
  otadata, NVS or storage without specific authorization.
- Authorized routine offsets: core `0x20000`; BSP package `0x820000`;
  development volume `0xc00000`. Verify package size before any BSP write.
- Capture boot from first byte with port `tools/reset-and-log.py`, not a
  reset followed by a late reader. Historical device `/dev/cu.usbmodem101`
  must be reidentified; it was not verified at handoff.
- Keep the normal working image available. Do not mix normal/SMP module ABI.
  Do not enable hart1 Exec or remove the hard configure refusal merely because
  isolated objects or host tests pass.

## Actual hardware / firmware state

Normal graphical AROS/Wanderer, touch and storage worked before this SMP
integration effort. See README/ROADMAP for the exact prior gates; no new
graphical acceptance is claimed here. E1/E2 private second-hart primitives have
hardware evidence. **Normal Exec still uses one HP hart (CPUCount1).**

Last recorded deployed normal core is 201,504 bytes, SHA256:

`1d77ae9251e702b00ac68163bdcf61944f34f5a1207a90afa8fc5a8ad1a645e6`

At handoff, these local files were independently checked and both match:

- `/Volumes/Dev/Source/Amiga/AROS-ESP32-build/bin/esp32p4-riscv/gen/rom/boot/aros-esp32p4.bin`
- `/Volumes/Dev/Source/Amiga/AROS-ESP32-build/evidence/smp-e3-lock-runtime-2026-10-01/normal-core-final.bin`

No device readback/reset/flash was performed in the last source-only work, so
local hash verification is not a fresh measurement of device contents.

## Objective and honest completion status

Finish E3-A1/A2/A3: actual per-hart Exec runtime, public synchronization and
complete Task lifecycle; matched core/BSP/public ABI rebuild; real concurrent
Exec tasks, contention and FPU/context migration qualification on D1001.
Host models and isolated objects are supporting evidence, not completion.

Too much time has gone into isolated foundations. Prioritize coherent
integration and end-to-end evidence, not ever more standalone green fixtures.
Nevertheless, do not paper over the known storage/cancellation races or bypass
the activation guard to manufacture an apparent two-core success.

## Latest implemented source state

### TaskResource observer foundations — dormant

- `kernel/p4_task_registry.c/.h` adds `taskresource_unobserved` and validates
  the private marker. Observer leaf admits exact live identities under an
  ordinary notification pin above all durable reservations, or a designated
  construction reference; -1 malformed, 0 refused, 1 admitted. No Task read.
- `kernel/p4_task_pins.c/.h`: `krnP4TaskResourceObserve` samples the resource
  acquire-load and admits/marks under the common lifetime gate. Successful
  NULL observation is distinct from refusal; output changes only on status1.
  Malformed result is fatal. **No production TaskLaunch call selects it.**
- Debt clearing exists only in actual detach/claim/finish paths, not bootstrap.

### Reportable registration — selected in the experimental source branch

- `rom/exec/taskresource_cleanup.h`: private callback now BOOL, matched
  contract `0x50345233`.
- `rom/task/task_execfuncs.c`: `task_RegisterNewTask` allocates an empty public,
  cleared, non-expungable entry outside IRQ/gate, then inserts under topology
  lock into TaskList or NewTasks; allocation failure returns FALSE unchanged.
- `rom/exec/newaddtask.c`: TaskLaunch still raw-acquire-loads `lb_TaskResBase`.
  Registration refusal stops before user Hook/Ready with terminal fatal.
  **This is temporary fail-closed policy, not recoverable constructor rollback.**
  Legacy void Hook registration remains separate and unchanged.

### Conservative bootstrap now publishes inside the lifetime gate

- `rom/task/task_bootstrap.c`: preallocate empty entries outside Disable,
  final `commit_inventory(void*)` recounts, checks capacity, assembles all
  four RUN/SPIN/READY/WAIT inventories and release-publishes the resource.
- `kernel/p4_taskresource_bootstrap.c/.h`: hardware-isolation check requires
  hart0 finalized/MIE off and hart1 prepared/unbound/reset asserted/clock off/
  boot0/CPUCount1. Transaction wrapper checks this before gated Apply.
- `kernel/p4_task_pins.c/.h`: Apply holds the common lifetime gate through
  the callback, preserving all conservative quiescence refusals. Callback
  0 refusal, 1 committed, 2 insufficient pool retry; no mutation before refusal.
- `exec/exec_platform.h` routes the experimental final transaction through it.
- This closes the gate-scope prerequisite, **not bootstrap availability**.
  Ordinary constructor pins still refuse coldstart initialization. No debt
  inventory, clearing or unconditional observer transition was implemented.

Frozen handoff source SHA256:

| Source relative to checkout | SHA256 |
|---|---|
| `arch/riscv-esp32p4/kernel/p4_task_pins.c` | b06e8b96df3ef70aa97c59a13cdefda4edcd637accba10f7c9648af1cb93a1d6 |
| `rom/task/task_bootstrap.c` | f82852252f28e50b2c9a4128a0ff24505e5c7beecb2f3a4f34a08788d5f8576f |
| `rom/exec/newaddtask.c` | 1269fc16f157fb034be7864dbc683ac9ba81f85d507c7cce5666e70c7f509157 |
| `rom/task/tests/p4-taskres-bootstrap-test.py` | 26e03eb4116d554e43aefcdcf55ba607876a6a88b32d737f8a2fbe7ce7a58ce9 |
| `rom/task/tests/p4-taskres-service-cleanup-test.py` | 7df4e5105f40ee3ceba9f1d686f2cd95acb28b00a2514da1bec78a17eefa67dc |

## Latest verified evidence

All reports below are under the canonical evidence directory in the paths table:

- `bootstrap-transaction-root-20261002.md`: exact commands, flags, source and
  binary sizes/hashes; root actual binding O1/O2 ASan/UBSan **82,941** checks;
  extracted bootstrap **461** each, intended no-quiescence negative detected;
  service **293 runtime +18 source** each. Six isolated SMP RV32 objects and
  excluded normal bootstrap object compile. No full linked SMP core.
  Report SHA256 `88674f38ee2c6c8bf2c2ad51415be5696dd48fbd519d8fdb91b3b7e07fbe84fe`.
- `bootstrap-transaction-review-20261002.md`: independent bounded source
  acceptance of conservative gate ordering only. SHA256
  `0bf6698bbba5b404eb7aeb422c4b4bb87ceb54647374e6cce67739c59ce0d9d1`.
- `bootstrap-debt-storage-audit-20261002.md`: exact storage authority and
  retirement source map. SHA256
  `5880cdf31e8aabd405198ea7a6716ff9c468e8c9d3de06bfe13915ba92103b51`.
- `taskresource-observer-binding-review-20261002.md`: keep Observe dormant
  until complete same-gate inventory/clear/publication. SHA256
  `5750d386f10f6750d24571f5f04ec2c473bd2ba2406598693d072e5c9526c181`.
- `taskresource-debt-registry-20261002.md` and
  `registration-result-20261002.md`: previous 228 registry /64 registration /
  32 ordering tests and preserved failed experiments. The author registration
  report's stale service-fixture failure was subsequently corrected by root;
  later root evidence supersedes that pending test status.

Limits: host gate, IRQ, hardware/runtime, Ready, allocator and list boundaries
are mocked. PROT_NONE identity tests help detect unauthorized Task reads;
they do not qualify actual harts. Extra bootstrap `-Wconversion` builds pass.
Extra `-pedantic` fails on embedded complete-source strings exceeding ISO's
required 4095-character limit: no pedantic pass was claimed. Retain failures.

## Immediate next implementation — proposed, NOT implemented

The storage audit's important result: **registry identity + gate + hardware
isolation is not a Task-field dereference capability**. Acquire a borrowed
pin under that gate before reading marker Task state, release it within the
same transaction. Do not recursively call public pin wrappers inside Apply.
Retired/claimed/inflight/post-free records must be classified using record
metadata before any Task read; a retained post-free identity can name freed
memory. NewAddTask's temporary constructor pin ends only after Ready/final log.

The intended complete debt handoff is still only a design direction:

1. Give the trusted bootstrap callback a gate-held validated registry view,
   or an equivalent bounded visitor with explicit borrowed-reference authority.
2. Count ordinary queues plus already-observed live TS_ADDED debts; validate
   construction authority, and ensure queued marked tasks appear exactly once.
   No Task snapshots may escape across Enable/preallocation.
3. Allocate only empty entries outside IRQ/gate; final transaction revalidates
   capacity and every identity before mutation, then copies, clears debt and
   publishes the binding under the same lifetime gate.
4. Select unconditional TaskLaunch Observe only with that complete packet;
   distinguish refusal from valid NULL and keep registration OOM reportable.
5. Verify paused NULL observer, marked READY dedup, retired PROT_NONE identities,
   balanced temporary pins, overflow/capacity refusal with unchanged debt/base,
   and publication interleavings. Independently review before activation.

There is **no** new DebtApply API, changed callback signature or marker-scan
implementation in the source at handoff. A test worker received a proposed
packet and was interrupted before modifying the fixtures. Do not assume that
briefing describes implemented code.

## Other mandatory E3 closure work

- Atomic TS_ADDED cancellation versus Ready/service ownership. A constructor
  Hook must not synchronously wait on its own held child pin. Current terminal
  fatal and TS_ADDED refusal do not solve lifecycle or recoverable OOM.
- Retired creator no-return/no-save unwind, orphaned construction cleanup,
  metadata/parent authority, raw TaskResource iterator/Hook/ETask reader leases.
- Public semaphore LIST and async Procure/Vacate composition, cancellation and
  durable notifications; actual concurrent AMO/IRQ/wake behavior and fairness.
- Actual secondary Exec entry/cache/XIP transition, online publication,
  scheduler/trap/timer/IPI/FPU/context migration. Private 32-byte XIP signature
  leaf exists but is unselected and has not run on hart1.
- Matched generated/public headers, all core/BSP/module consumers and full
  linkage, then actual concurrent D1001 Exec workloads with counters/timeouts.
  SMP stays opt-in; E4 graphical/default qualification is a separate gate.

Use ROADMAP/SMP acceptance requirements rather than treating this list as a
replacement or reducing the goal to the components already tested.

## Build pitfalls and tools

Reference configure argv from config.status:
`--target=esp32p4-riscv --enable-ccache target_alias=esp32p4-riscv PYTHON=/Volumes/Dev/Source/Amiga/AROS-ESP32-build/.venv/bin/python`.

Compiler: `/Volumes/Dev/Source/Amiga/AROS-ESP32-build/bin/darwin-aarch64/tools/crosstools/riscv-aros-gcc`.
Isolated RV32 compile commands/explicit sysroot/includes/defines are reproduced
in `bootstrap-transaction-root-20261002.md`; they are not a full-build recipe.

`configure.in`, generated `configure` and `tools/configure-smp-build.sh` still
refuse experimental configuration. The helper is report/refusal-only. A
read-only investigation of a complete safe build route was interrupted; no
`full-core-build-map-20261002.md` existed at handoff. Resolve the actual build
graph/readiness conditions; do not bypass the guard or mutate the normal tree
as a shortcut. The reserved SMP build path above is not proof of a configured
tree. Check filesystem state before reuse.

mmake does not invalidate on changed defines/makefiles. Invalidate affected
objects for documented builds, refresh both generated and Developer public
headers via include-copy rules, check explicit sysroot and compiled ABI.
Filter build errors with `error:`, require fresh `Creating .../aros-esp32p4.bin`,
verify artifact size/SHA. Failed link means no new image, not a successful run.
Respect SRAM residency and PSRAM atomic restrictions in the port instructions.

## Stop state

On Fabian's request, Codex interrupted the waiting fixture task and the
read-only full-build mapping task. The storage audit had completed. No live
build/test session was left running by this handoff. No commit, flash, reset,
media mutation or interactive test was performed for the handoff.
Continue in Claude Code from the actual dirty worktree, not old transcript
assumptions. Codex's persistent goal is paused, not complete.
