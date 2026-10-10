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

Next adapt these APIs with documented allocation/serialization semantics and
verify the full build. Do not globally disable compiler diagnostics, guess
reserved-field equivalents or claim that 5.4.3 is mandatory.
