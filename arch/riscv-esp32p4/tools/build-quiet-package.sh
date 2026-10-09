#!/usr/bin/env bash
# Build a diagnostic-off DOS/FAT/dosboot candidate, preserving the other
# package records exactly. No hardware writes; baseline is never overwritten.
# Usage: bash build-quiet-package.sh <build-dir> <baseline.pkg> <new-evidence-dir>
#        FLAG=VALUE ...
set -euo pipefail
export LC_ALL=C
[ "$#" -ge 3 ] || { echo "build-dir baseline.pkg new-evidence-dir required" >&2; exit 2; }
build=$(cd "$1" && pwd)
baseline=$2
logs=$3
shift 3
[ -f "$baseline" ] || { echo "baseline package missing" >&2; exit 1; }
baseline="$(cd "$(dirname "$baseline")" && pwd)/$(basename "$baseline")"
mkdir "$logs" # Refuse to mix new artifacts with an earlier experiment.
logs=$(cd "$logs" && pwd)
src=$(cd "$(dirname "$0")/../../.." && pwd)
suffix=$(sed -n 's/^AROS_TARGET_SUFFIX *= *//p' "$build/config/make.cfg")
variant=$(sed -n 's/^AROS_TARGET_VARIANT *?= *//p' "$build/Makefile")
gen="$build/bin/esp32p4-riscv$suffix/gen"
stage="$build/bin/esp32p4-riscv$suffix/AROS"
flags=("$@" FAT_DEBUG=0 DOS_DEBUG=0 DOSBOOT_DEBUG=0)
printf '%s\n' "${flags[@]}" > "$logs/flags.txt"
shasum -a 256 "$baseline" > "$logs/baseline.sha256"
baseline_hash=$(shasum -a 256 "$baseline" | cut -d' ' -f1)
for dir in rom/dos rom/dosboot rom/filesys/fat; do
    [ -d "$gen/$dir" ] || { echo "missing generated directory $dir" >&2; exit 1; }
    find "$gen/$dir" -name '*.o' -delete
done
echo "invalidated DOS, dosboot and FAT objects (make does not track -D)" > "$logs/clean.txt"
cd "$build"
for target in kernel-dos-quick kernel-fs-fat-quick kernel-dosboot-quick; do
    gmake "$target" "${flags[@]}" >> "$logs/build.log" 2>&1
done
if grep -q 'error:' "$logs/build.log"; then
    echo "compiler error; see build.log" >&2; exit 1
fi
cp -f "$stage/Libs/dos.library" "$logs/dos.library"
cp -f "$stage/L/fat-handler" "$logs/fat-handler"
cp -f "$stage/Devs/dosboot.resource" "$logs/dosboot.resource"
[ "$(shasum -a 256 "$baseline" | cut -d' ' -f1)" = "$baseline_hash" ] || {
    echo "baseline changed during build; refusing replacement" >&2; exit 1;
}
python3 "$src/arch/riscv-esp32p4/tools/replace-package-member.py" \
    "$baseline" dos.library "$logs/dos.library" "$logs/step1.pkg"
python3 "$src/arch/riscv-esp32p4/tools/replace-package-member.py" \
    "$logs/step1.pkg" fat-handler "$logs/fat-handler" "$logs/step2.pkg"
python3 "$src/arch/riscv-esp32p4/tools/replace-package-member.py" \
    "$logs/step2.pkg" dosboot.resource "$logs/dosboot.resource" "$logs/aros-bsp.pkg"
# Check the candidate with the board's actual size rule. -o marks only the
# immutable candidate as already built: do not regenerate the whole package.
cd "$build/arch/riscv-esp32p4/boot"
gmake --no-print-directory TOP="$build" SRCDIR="$src" \
    AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
    AROS_TARGET_ARCH=esp32p4 AROS_TARGET_CPU=riscv AROS_TARGET_VARIANT="$variant" \
    CURDIR=arch/riscv-esp32p4/boot TARGET=kernel-package-esp32p4-riscv-checksize \
    AROSARCHDIR="$logs" -f mmakefile -o "$logs/aros-bsp.pkg" "${flags[@]}" \
    kernel-package-esp32p4-riscv-checksize > "$logs/checksize.log" 2>&1
stat -f '%z bytes' "$logs/aros-bsp.pkg"
shasum -a 256 "$logs/aros-bsp.pkg"
