# Pinned ESP-Hosted reference for JC1060

Source audit, 2026-10-10. This is not a build or hardware acceptance.

## Provenance

Use the complete official host at
`espressif/esp-hosted@d5184217f710d54c01f932e75b4c55e3f4933fc9`
as the next comparison candidate, rather than current main or registry version
0.0.6. The board source is
`cheops/JC1060P470C_I_W@6304eddef32a33aa9d9a85e971e39dfb24c3cca3`,
under `1-Demo/Demo_IDF/examples/slave/main/common/`.

Primary comparison against the official archive's root `common/` confirms
byte-identical schema and generated protobuf-C files:

| File under common/proto | SHA-256, both sources |
| --- | --- |
| esp_hosted_rpc.proto | bbdbc9bb69c2b1793de6441ea55051c3ceb3f7b30994227b8ff1e995b0137dec |
| esp_hosted_rpc.pb-c.c | 693efbce5db14bf6f2416f5fcf746049a459764ec9f8571a45f2b6a7bdffcc69 |
| esp_hosted_rpc.pb-c.h | ad4f03ade1ca38d88354cc3104f1f4838f7c25004197e19a8fac395c1ad05e85 |

`common/include/adapter.h` differs only by an HCI comment and vendor UART
extended-capability bits 8/9; its SDIO payload-header layout is unchanged.
The earlier interpretation that the vendor removed the newer RPC IDs was
reversed: the vendor matches d518, which adds them relative to registry 0.0.6.
Preserve this correction; do not infer compatibility from the version label.
The installed C6's `54aafd0-dirty` identity remains unresolved. Source lineage
does not establish the identity or behavior of its installed binary.

Official archive download:
`https://codeload.github.com/espressif/esp-hosted/tar.gz/d5184217f710d54c01f932e75b4c55e3f4933fc9`
SHA-256 `1310dc05283b6adefa3ec217c017c7a8836801e263115c7192f9d8ef7d435c12`.
Local inspection workspace: `/Volumes/Dev/Build/esp-hosted-reference.RzWnFu`.
Vendor comparison snapshots: `/tmp/esp-hosted-audit.QD6qYL` (temporary;
re-fetch pinned source paths to reproduce).

## Build and deployment gates

1. Build the actual root component and its complete transport/bootstrap/RPC
   implementation. The old host example CMake files name stale directories;
   do not repair that by inventing a replacement protocol. A small application
   using the existing APIs is the intended reference front end.
2. Primary build target remains ESP-IDF 6.0.1. ESP-IDF v5.4.3 is downloaded with submodules, commit
   `ea1c174c1cbb7348bd8ba0ff1eb306246938dd80`; its fetch process exited 0.
   Its isolated tool installation exited 0; it is a comparison option only,
   not a port downgrade or a proven requirement. The official host manifest
   requires >=5.3. Its first actual build under 6.0.1 fails; see below.
3. `esp_wifi_remote` 0.1.12 requires exactly IDF 5.3. Candidate 0.2.3 supports
   IDF >=5.3, but declares an unconditional eppp_link dependency. Resolve and
   pin the dependency closure, then verify host API compatibility by build.
   No dependency compatibility is claimed from this audit alone.
4. Board adaptation must forbid reset GPIO access when reset is absent.
   Official `reset_slave()` unconditionally configures/writes its pin;
   `MAX_RETRY_TRANSPORT_ACTIVE` is 1000. Replace this policy with an explicit
   bounded, fail-stop reference gate and record the exact source patch.
   JC1060 has `WIFI_RESET_GPIO=-1`; GPIO54 is not established on this board.
5. Use verified JC1060 SDIO pins CLK18/CMD19/D0..D3=14..17. Audit all
   implicit constructor initialization and task/error paths before deployment.
   Preserve C6 firmware, SD media, bootloader and partition table. No OTA call.
6. A full host performs function/IRQ/bootstrap writes, including OPEN; this
   changes powered C6 state. P4 backup/rollback cannot undo those effects.
   Quiesce existing scanout before a reference handover; independently back up
   and read back the exact P4 write span and restore the normal baseline.
7. First reference acceptance is bounded startup, MAC and scan, followed by
   explicit association/frame/recovery gates. A working IDF reference is not
   an AROS WLAN driver. Port OS/bus glue and integrate SANA-II only after the
   wire contract and behavior are demonstrated.

No further hardware probe was started during this source audit.

## Actual IDF 6.0.1 build attempt

The build-only front end is saved in `tools/hosted-reference/`. It selects the
complete external host and remote component; it does not bypass RPC/transport.
It is not a scan/association test or an approved firmware image. The tested
copy is `/Volumes/Dev/Build/esp-hosted-reference.RzWnFu/reference-app`.

Build environment: local IDF commit
`8c19b156084a0753687347cca1f5355782893533`, version 6.0.1, with pre-existing
bootloader and esp_hw_support changes left untouched. Python environment
`/Users/fabian/.espressif/tools/python/v6.0.1/venv`, GCC
`esp-15.2.0_20251204`. Explicit `IDF_PYTHON_ENV_PATH` is required; otherwise
idf.py looks for a nonexistent standard Python environment. Set IDF_PATH,
HOSTED_REFERENCE_ROOT and ESP_IDF_VERSION=6.0.1; run `idf.py -B build-v6 build`.
Component manager is disabled (`IDF_COMPONENT_MANAGER=0`) for the explicit
local component closure. Remote's EPPP sources are excluded by HOSTED selection;
no EPPP component is used in this build. This does not validate EPPP dependencies.

The official gitlink for `common/protobuf-c` is
`abc67a11c6db271bedbb9f58be85d6f4e2ea8389`. The codeload archive omits submodule
content; supply that exact checked-out revision, not the latest protobuf-c.

Actual attempts, each terminal process exit 2 after environment setup:

- `build-v6.log`: missing declared `esp_driver_sdmmc` dependency.
- `build-v6-dependencies.log`: additional DMA dependency and libc endian-macro
  redefinitions. External CMake now declares esp_driver_sdmmc, esp_driver_dma,
  esp_wifi_remote; common.h guards htole16/le16toh instead of suppressing warnings.
- `build-v6-dma.log`: missing GPIO dependency, removed public
  esp_dma_mem_info_t/esp_dma_capable_malloc, removed wifi_sta_config_t
  reserved/he_reserved fields, and two new HTTP event cases. This is concrete
  API incompatibility, not a working reference build. No image accepted/flashed.

The compatibility bridge is now retained as `tools/hosted-reference/idf6-compat.patch`,
SHA-256 `0232d1d573929724a076677cd4e521f0cbf5ebf382be5e0f324a7426131426cb`.
It applies cleanly to a freshly extracted pinned archive (`git apply --check`,
exit0). No global compiler-warning suppression or wire-schema change.
DMA uses overflow-checked 64-byte rounding/alignment and deliberately stricter
DMA|INTERNAL|8BIT placement, with PSRAM builds rejected. Reserved STA/HE wire
bits are zero on request and ignored on response, not remapped into IDF6's new
feature positions. Two new HTTP event variants are explicitly handled.

Full build session64334 exits0 under IDF6.0.1. Its first image379040B
SHA-256 `4c60061ab7c44db14f4bc645468cea65154fad80994e030e39b131c1e01dcdb9`
targets revisions3.1..3.99 and is rejected for the JC1060. IDF6 requires
ESP32P4_SELECTS_REV_LESS_V3=y in addition to REV_MIN_100. Corrected full
build session85767 exits0;377856B SHA-256
`8c6d4b967adf9820714b0beb1200c3a1320e337758aea65bba4b6220482b34bd`.
Image-info validates P4, revision1.0..1.99,16MB DIO80MHz. Build log
`build-v6-rev1.log` SHA-256
`6ab1bd3d9055a999bbf1efd12cf89727a327fff301ddc772b2a2d6825a093a60`.
The normal generated flash commands would overwrite the AROS layout; forbidden.

Independent actual-source allocator tests and primary reruns pass strict C11
O0/O2 ASan/UBSan,10 cases/41 checks each, exit0. Test extracts the external
function verbatim and checks overflow, rounding, exact allocation arguments,
failure and pointer forwarding with a heap stub. It does not test actual P4
memory placement or DMA. Retained test script SHA-256
`c13855a062f517c30de329de12580ecaee6503c60f5260a741bce9d4c44275b4`;
`allocator-tests-primary.log` SHA-256
`5786a8b111f0febef67f2be1aa240cc84223ec95aeb60c5701b16c8274b327da`.

This is build verification only. No serial/device activity. The reset/timeout,
constructor and task failure policies still require adaptation before a run.
The front end at that stage initializes Wi-Fi only; MAC/scan/association/frame
and recovery acceptance remain open. Do not claim that 5.4.3 is mandatory.

## Bounded MAC/scan candidate

Apply `idf6-compat.patch`, `bounded-reference.patch`, then `scan-safety.patch`
to the pinned archive, checking each before application. Primary reproduction
in `/Volumes/Dev/Build/hosted-reproduce.6qcg7E` exits0 and matches the actual
external host tree byte-for-byte. All three pinned protobuf hashes remain
unchanged. No replacement transport or RPC implementation.

The bounded patch removes implicit initialization/destruction, forbids reset
GPIO access for -1, uses a ten-second connection deadline and propagates
connection failure. Card-init, OPEN and worker-creation failures abort under
PRINT_HALT. The app starts its forty-second timer before explicit init, checks
each API before/after deadline, validates a unicast MAC, performs one blocking
scan and retrieves at most eight records before stopping Wi-Fi. Cooperative
timer dispatch cannot guarantee a hard deadline if scheduling/hardware hangs.
Persistent workers are not automatically torn down after application success.

The old scan wrapper copied the slave count without capacity validation and
never published that count. The parser also trusted repeated counts, pointers
and byte lengths and could report malformed replies as successful because
calloc initialized status to zero. The scan patch corrects these concrete
paths, validates MAC length and defaults decode failures to a nonzero error.
The pinned slave rejects empty get_records; the app therefore skips that API
after a successful zero-AP scan, preserving C6 firmware. This is an explicitly
named empty-scan compatibility workaround, not a slave API root fix.

Actual-source startup models pass strict O0/O2 ASan/UBSan:6 cases/49 checks
each. The final build process30271 exits0, image380304B SHA-256
`12383bd32aee7c2f4ad40dae48f3a58aa141a9cf9ba67c7f4190ca7237c5aa27`;
`build-v6-scan-safe-final.log` SHA-256
`5ca0d68d912fd73b1b9e46cb7fab9ef8aa2e27311a88c136c28e627e69da77bf`.
P4 revision1.0..1.99,16MB DIO80, PRINT_HALT, PSRAMoff, reset=-1,
slot1/1bit/400kHz unchanged. Scan/application tests and independent runtime
review are pending. No hardware deployed or accepted. Association, Ethernet
frames, native AROS/SANA-II integration, coexistence and recovery remain open.

A subsequent source correction removes the second mutex creation after worker
launch: replacing a live global lock could split bus ownership and leaked the
initial mutex. Session49737 exits0; final380240B image SHA-256
`a765b9f65617dddd52e3b670dc09310e4c4a64209313809a7923c8fc8d3e3a5a`;
`build-v6-lock-safe.log` SHA-256
`64d66f375af9c121ce4928822e31929b844c3c98407a6ad23f4f5ba21421ff4a`.
The earlier image above is superseded, not deployed. This correction is retained
in bounded-reference.patch; rebuild and review remain separate from hardware.

## First bounded hardware attempt: startup failure, recovery pending

Final actual-source startup/scan/app models and independent review pass (see
ROADMAP dated evidence). The380256-byte 2e92b5e6... image was explicitly
written/readback verified at0x20000 after quiescence. It fails before app entry
at esp_clk_init: the generated400MHz setting is unsupported on revision1.3.
No Hosted initialization or MAC/scan acceptance follows. Defaults now select
360MHz and check-config.py enforces21 required generated settings. Corrected
build380256B SHA-256349e8c588870d98e938055ac008dd3d5e4de5b430b298233c7bd7c94ad6ba900
passes, not deployed.

The entire380928-byte original core span was restored and independently
verified, SHA-2561880a2f41f07fe14a32df7dde2dd4a09119248e6b67442967b49b8b95ce3e62e.
Its return capture instead faults in ROM at store address0x50091000 and
watchdog-resets. Byte-identical flash restoration is not boot recovery.
Board identity is verified and the board is left in download loader; retained
state/root cause remains open. No more reference deployment before baseline
recovery. Bootloader/partition/BSP/SD/C6 firmware were not written. Empty-scan
workaround and its separate N1E uncompensated removal test remain open.
Subsequent current-state reading finds the240MHz parent gate off while Crypto
selects that parent (mux2, established by esp_crypto_clk.h). Exactly one masked
bit30 enable at0x500e6028 changes0x00170503 to0x40170503 and restores a complete
30.948s baseline return. This is a manual recovery workaround, not a permanent
fix or rail-off gate; N1R tracks warm/early-failure return without compensation.
Board now runs baseline AROS. Corrected360MHz reference is still not deployed.

## Corrected360MHz reference hardware gate

The subsequent exact349e8c58... candidate passes one full MAC/scan run under
IDF6.0.1. C6 INIT chip12/capabilities0x0d includes WLAN; MAC10:bd:a3:9b:bc:34,
13 APs/eight records, result=ok at4.085s. Complete45.822s capture has no
additional reset/fault. Entire380928-byte baseline span restored/readback
matched; complete30.925s AROS return passes without another parent-clock write.
See ROADMAP dated evidence for complete hashes, sessions and procedure.

This is a measured complete official-host reference, not a native AROS Wi-Fi
driver or an association/frame/coexistence/recovery pass. It proves installed
protocol compatibility for these calls, not installed binary/source lineage.
The empty-scan workaround was not exercised (13 APs); N1E remains open.
N1R still needs uncompensated early-failure return. Reference function/OPEN
mutations persist in powered C6 state; P4 flash restoration does not undo them.
Use the exact working card/bootstrap path for native comparison rather than
infer firmware incompatibility from prior timeouts. C6 firmware unchanged.
