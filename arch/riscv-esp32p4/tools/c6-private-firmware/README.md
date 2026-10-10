# Private C6 firmware candidate

This recipe creates an undeployed candidate. It does not authorize flashing,
establish installed C6 identity, or prove recovery. Never use the generated
`flash_args` before the documented actual-device recovery gate passes.

Source: ESP-Hosted archive for d5184217f710d54c01f932e75b4c55e3f4933fc9,
archive SHA-256 `1310dc05283b6adefa3ec217c017c7a8836801e263115c7192f9d8ef7d435c12`.
Use the complete matching protobuf-c dependency, not the earlier empty
reproduction directory. Apply `privacy.patch` to a separate source copy using
`git apply --check` followed by `git apply`. Copy `sdkconfig.private` into its
`slave` directory. Preserve the original archive and source hashes.

The patch removes Hosted SSID/password/current-AP logs and SDIO, serial,
Ethernet and HCI payload dumps regardless of later DEBUG/VERBOSE settings.
It bounds the response-string length read to the field width. Valid terminated
and full-width fields retain the previous intended length convention. Generic
Wi-Fi MAC/status logs and IDF/vendor-library logs are not all suppressed.
Do not claim whole-binary or hardware privacy certification from this patch.

Build environment used: IDF 5.4.3, Python 3.12.13 in a private uv venv,
IDF core requirements constrained by Espressif's `espidf.constraints.v5.4.txt`,
CMake 3.30.2 and official Ninja 1.12.1, RISC-V GCC 14.2.0_20250730.
Provisioning downloaded dependencies; the subsequent build used local files.
Archive exact resolved Python versions, constraints and IDF/tool identities.

Set `IDF_PATH`, `IDF_TOOLS_PATH`, `IDF_PYTHON_ENV_PATH` to the isolated locations;
prepend venv, Ninja and compiler bin directories to PATH. No global IDF install
or source replacement is required. Disable component resolution with
`IDF_COMPONENT_MANAGER=0` for this dependency-complete pinned tree.

```sh
cd PRIVATE_SOURCE/slave
python "$IDF_PATH/tools/idf.py" -B PRIVATE_BUILD \
  -D IDF_TARGET=esp32c6 \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.private' build
```

IDF loads `sdkconfig.defaults.esp32c6` automatically. Verify the resulting
sdkconfig selects SDIO, C6, maximum INFO, Wi-Fi NVS disabled, and the expected
partition CSV. Archive sdkconfig, ELF/BIN/map, bootloader, partition table,
build logs, source patch, manifest and hashes. A successful partition-size
check applies to this source layout only, not the installed device.

Run `sh test-string-response.sh PRIVATE_SOURCE` at O0/O2 with ASan/UBSan.
This extracts the actual response macro and exercises exact-width fields;
it does not test the full RPC handler, allocation failure, wire/radio behavior,
or compiled IDF library logging. Installed identity, security/backup/restore,
actual UART privacy, native association and data-path acceptance remain open.
