#!/usr/bin/env bash
#
#   Build the ESP32-P4 core image (kernel + exec + task + debug) with one
#   explicit flag set, and leave a log that shows what was built.
#
#   usage: build-core.sh <build-dir> <log-dir> [--clean] [--link-only] FLAG=VALUE ...
#
#   --clean deletes the kernel, exec, task and debug objects first. mmake
#   does not track -D switches or P4_BOARD, so any change of the flag set
#   needs it (see AGENTS.md, "Builds whose result gets documented").
#   timer.device and flashdisk.device are always made (incrementally),
#   flashdisk through its -quick target: the plain one also builds the
#   development volume image, which fails in a tree whose volume content
#   has outgrown it. Neither object depends on the flag set.
#
#   The image is linked directly from the generated kernel mmakefile, which
#   skips the oversized standalone flashdisk stage that the aggregate
#   kernel-esp32p4-riscv target still trips over. The script fails unless
#   the link prints "Creating .../aros-esp32p4.bin".
#
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <build-dir> <log-dir> [--clean] [--link-only] FLAG=VALUE ..." >&2
    exit 2
fi

build=$(cd "$1" && pwd)
logs=$2
shift 2
clean=0
link_only=0
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) clean=1; shift ;;
        --link-only) link_only=1; shift ;;
        *) break ;;
    esac
done
flags=("$@")
src=$(cd "$(dirname "$0")/../../.." && pwd)
# The tree's variant (empty, or "smp") and the suffix of its output
# directory, as configure recorded them.
variant=$(sed -n 's/^AROS_TARGET_VARIANT *?= *//p' "$build/Makefile")
suffix=$(sed -n 's/^AROS_TARGET_SUFFIX *= *//p' "$build/config/make.cfg")
gen="$build/bin/esp32p4-riscv$suffix/gen"

mkdir -p "$logs"
printf '%s\n' "${flags[@]}" > "$logs/flags.txt"
export PATH="$build/.venv/bin:$PATH"

if [ $clean = 1 ]; then
    for d in rom/kernel rom/exec rom/task rom/debug; do
        find "$gen/$d" -name '*.o' -delete 2>/dev/null || true
    done
    echo "cleaned kernel/exec/task/debug objects" > "$logs/clean.txt"
fi

cd "$build"
# timer.device and flashdisk.device are part of the core as well. They used
# to be built only when their objects were missing, on the reasoning that
# they do not depend on the flag set; but their sources change too, and a
# core then linked the old objects without a word (S8, 2026-10-09). Their
# targets are incremental, so they are always asked for now.
targets="kernel-kernel-kobj kernel-exec-kobj kernel-task-kobj kernel-debug-kobj"
targets="$targets kernel-timer-kobj kernel-flashdisk-kobj-quick"
[ $link_only = 1 ] || for t in $targets; do
    echo "=== $t" >> "$logs/build.log"
    if ! gmake "$t" "${flags[@]}" >> "$logs/build.log" 2>&1; then
        echo "build of $t failed; see $logs/build.log" >&2
        grep -n 'error:\|\*\*\*' "$logs/build.log" | tail -20 >&2 || true
        exit 1
    fi
done

if grep -q 'error:' "$logs/build.log"; then
    echo "compiler errors in build.log" >&2
    grep -n 'error:' "$logs/build.log" | head -20 >&2
    exit 1
fi

bin="$gen/rom/boot/aros-esp32p4.bin"
before=""
[ -f "$bin" ] && before=$(shasum -a 256 "$bin" | cut -d' ' -f1)
rm -f "$gen/rom/boot/core.elf"

cd "$build/arch/riscv-esp32p4/kernel"
gmake --no-print-directory TOP="$build" SRCDIR="$src" \
    AROS_HOST_ARCH=darwin AROS_HOST_CPU=aarch64 \
    AROS_TARGET_ARCH=esp32p4 AROS_TARGET_CPU=riscv AROS_TARGET_VARIANT="$variant" \
    CURDIR=arch/riscv-esp32p4/kernel TARGET=kernel-esp32p4-riscv \
    -f mmakefile "${flags[@]}" ESPTOOL="$build/.venv/bin/esptool" "$bin" \
    > "$logs/link.log" 2>&1 || {
        echo "link failed; see $logs/link.log" >&2
        tail -20 "$logs/link.log" >&2
        exit 1
    }

if grep -q 'error:' "$logs/link.log"; then
    echo "errors in link.log" >&2
    grep -n 'error:' "$logs/link.log" >&2
    exit 1
fi
grep -q "Creating   $bin" "$logs/link.log" || {
    echo "link.log lacks 'Creating $bin'" >&2
    exit 1
}

after=$(shasum -a 256 "$bin" | cut -d' ' -f1)
size=$(stat -f %z "$bin")
cp "$bin" "$logs/core.bin"
cp "$gen/rom/boot/core.elf" "$logs/core.elf"
cp "$gen/rom/boot/core.map" "$logs/core.map"
{
    echo "size $size"
    echo "sha256 $after"
    echo "previous $before"
} > "$logs/artifact.txt"
cat "$logs/artifact.txt"
