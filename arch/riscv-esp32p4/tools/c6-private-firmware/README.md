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

## C6 vendor logging extension (build verified, undeployed)

`idf-wifi-privacy.patch` applies to the pinned IDF 5.4.3 commit
`ea1c174c1cbb7348bd8ba0ff1eb306246938dd80`, specifically its C6 Wi-Fi OS
adapter. Apply it with `git apply --check` and `git apply` only to a separate
complete IDF source copy, then select that copy through `IDF_PATH`. Preserve
the reference IDF and the previous candidate/evidence unchanged.

Both vendor `_log_write`/`_log_writev` callbacks become unconditional sinks:
they neither format arguments nor forward to ESP logging. This deliberately
disables callback-routed vendor Wi-Fi diagnostics at every level, while ordinary Hosted
status/error logs remain available. A runtime tag/INFO setting alone is not
the privacy policy. The IDF constructor sets Wi-Fi tags to the default level,
and `esp_wifi_init()` sets the vendor level from the configured maximum.

Run `python3 test-vendor-log.py PRIVATE_IDF` to exercise the exact patched
callback bodies at O0/O2 with ASan/UBSan. Each run checks18 cases; the original
IDF must fail the sink-call assertion as a negative control. Additional source
guards require the reviewed no-op bodies and expected table-binding text, so
a future direct printf/ROM-output replacement cannot silently pass this test.
The fixture mocks the downstream logger and does not execute the adapter
table, vendor blobs, IDF startup or a radio. Review actual table bindings separately.

Apply `hosted-metadata-privacy.patch` to the Hosted tree after `privacy.patch`.
It removes the remaining Wi-Fi/Bluetooth MAC and direct country/HE AP diagnostic
outputs without changing RPC payloads or Wi-Fi event forwarding. Verify patch
application and retain all three patches with the build inputs.

A fresh complete build with both extensions now generates a1105760-byte app,
SHA-256 `d4a992b9aa806d61b0e406b848dfc4e449f3a06eab102120b84453c7ae7c358e`.
The previous1106624-byte candidate is unchanged. The new ELF's two logging
callbacks have no outgoing calls; the variadic callback still has ordinary
ABI register spills. Vendor format strings may remain despite those sinks.
Direct stdio/ROM and PHY diagnostics require link-map/reachability review;
callback suppression does not prove whole-firmware UART privacy. Actual UART
qualification after secured recovery remains required before credentials.

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
