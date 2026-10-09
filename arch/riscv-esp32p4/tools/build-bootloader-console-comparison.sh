#!/usr/bin/env bash
# Build only. Never accesses hardware or modifies the shared IDF/reference.
# Activate the IDF toolchain/Python environment first, including
# IDF_PYTHON_ENV_PATH if its venv is not in IDF's conventional location.
set -euo pipefail
if [[ $# != 3 ]]; then
    echo "Usage: $0 <patched-idf-root> <reference-idf-project> <new-evidence-dir>" >&2
    exit 2
fi
idf_root=$(cd "$1" && pwd -P)
reference=$(cd "$2" && pwd -P)
port_root=$(cd "$(dirname "$0")/.." && pwd -P)
evidence=$3
[[ ! -e "$evidence" ]] || { echo "Evidence directory already exists" >&2; exit 2; }
[[ -f "$reference/sdkconfig" && -f "$reference/sdkconfig.defaults" &&
   -f "$reference/build/bootloader/bootloader.bin" ]]
[[ -f "$idf_root/components/bootloader_support/src/bootloader_utility.c" ]]
[[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]] || {
    echo "Activate IDF and set IDF_PYTHON_ENV_PATH to its installed venv" >&2; exit 2;
}
rg -q '^CONFIG_ESP_CONSOLE_UART=y$' "$reference/sdkconfig" || {
    echo "Comparison requires the existing UART console profile" >&2; exit 2;
}
command -v cmake >/dev/null
command -v ninja >/dev/null
command -v riscv32-esp-elf-objdump >/dev/null
mkdir -p "$evidence"
evidence=$(cd "$evidence" && pwd -P)
shasum -a 256 "$reference/sdkconfig" "$reference/build/bootloader/bootloader.bin" \
    > "$evidence/reference.sha256"
shasum -a 256 "$idf_root/components/bootloader_support/src/bootloader_console.c" \
    "$idf_root/components/bootloader_support/src/bootloader_utility.c" \
    "$idf_root/components/esp_rom/patches/esp_rom_sys.c" \
    > "$evidence/idf-inputs.sha256"
command cp "$reference/build/bootloader/bootloader.bin" "$evidence/rollback-bootloader.bin"
# Keep compile-time metadata comparable between the two clean builds.
export SOURCE_DATE_EPOCH=1791504000
export IDF_PATH="$idf_root"
for profile in baseline no-usb-secondary; do
    project="$evidence/$profile"
    mkdir -p "$project"
    command cp -R "$port_root/bootloader/project/." "$project/"
    command cp "$reference/sdkconfig" "$project/sdkconfig"
    command cp "$reference/sdkconfig.defaults" "$project/sdkconfig.defaults"
    if [[ "$profile" == no-usb-secondary ]]; then
        mkdir -p "$project/bootloader_components"
        command cp -R "$port_root/bootloader/diagnostics/no_usb_secondary" \
            "$project/bootloader_components/"
    fi
    cmake -S "$project" -B "$project/build" -G Ninja -DIDF_TARGET=esp32p4 \
        -DSDKCONFIG_DEFAULTS=sdkconfig.defaults 2>&1 | tee "$project/cmake.log"
    ninja -C "$project/build" bootloader 2>&1 | tee "$project/build.log"
    test -s "$project/build/bootloader/bootloader.bin"
    command cp "$project/build/bootloader/bootloader.bin" "$project/bootloader.bin"
    riscv32-esp-elf-objdump -d "$project/build/bootloader/bootloader.elf" \
        > "$project/disassembly.txt"
    shasum -a 256 "$project/bootloader.bin" "$project/sdkconfig" \
        > "$project/artifacts.sha256"
    # Neither profile may overlap the unchanged partition table at0x8000.
    size=$(wc -c < "$project/bootloader.bin")
    (( size <= 0x6000 )) || { echo "Bootloader exceeds0x2000..0x7fff" >&2; exit 1; }
done
cmp "$evidence/baseline/sdkconfig" "$evidence/no-usb-secondary/sdkconfig"
shasum -a 256 -c "$evidence/reference.sha256"
shasum -a 256 -c "$evidence/idf-inputs.sha256"
echo "Both clean builds complete; inspect wrapper linkage before any write."
