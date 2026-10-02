#!/bin/sh

set -eu

usage()
{
    cat <<'EOF'
Usage: configure-smp-build.sh [--report|--dry-run|--configure]

--report / --dry-run  Report the blocked experimental configuration. Default;
                      reads configuration and toolchain metadata only.
--configure           Refuse. This helper has no configure or build path until
                      reviewed Exec SMP runtime integration replaces it.
EOF
}

MODE=report
case "${1-}" in
    "") ;;
    --report|--dry-run) MODE=report ;;
    --configure) MODE=configure ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
if [ "$#" -gt 1 ]; then
    usage >&2
    exit 2
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
SOURCE_PARENT=$(dirname -- "$SOURCE_ROOT")
REFERENCE_BUILD=${AROS_REFERENCE_BUILD:-"$SOURCE_PARENT/AROS-ESP32-build"}
SMP_BUILD_DIR=${AROS_SMP_BUILD_DIR:-"$SOURCE_PARENT/AROS-ESP32-SMP-build"}
ROADMAP="$SOURCE_ROOT/arch/riscv-esp32p4/ROADMAP.md"
CONFIGURE_IN="$SOURCE_ROOT/configure.in"
CONFIGURE="$SOURCE_ROOT/configure"
GUARD_TEXT='ESP32-P4 Exec SMP is blocked:'

E3_STATUS=
if [ -r "$ROADMAP" ]; then
    E3_STATUS=$(awk -F '|' '$2 ~ /^[[:space:]]*E3[[:space:]]*$/ { gsub(/^[[:space:]]+|[[:space:]]+$/, "", $4); gsub(/`/, "", $4); print $4; exit }' "$ROADMAP")
fi

HAS_SOURCE_GUARD=no
if [ -r "$CONFIGURE_IN" ] && grep -Fq "$GUARD_TEXT" "$CONFIGURE_IN"; then
    HAS_SOURCE_GUARD=yes
fi
HAS_GENERATED_GUARD=no
if [ -r "$CONFIGURE" ] && grep -Fq "$GUARD_TEXT" "$CONFIGURE"; then
    HAS_GENERATED_GUARD=yes
fi

REFERENCE_CROSSTOOLSDIR=
REFERENCE_CONFIG_ARGS=
if [ -r "$REFERENCE_BUILD/config.status" ]; then
    REFERENCE_CONFIG_ARGS=$(sed -n "s/^ac_cs_config='\(.*\)'$/\1/p" "$REFERENCE_BUILD/config.status" | sed -n '1p')
fi
if [ -r "$REFERENCE_BUILD/config/make.cfg" ]; then
    REFERENCE_CROSSTOOLSDIR=$(sed -n 's/^CROSSTOOLSDIR[[:space:]]*:=\{0,1\}[[:space:]]*//p' "$REFERENCE_BUILD/config/make.cfg" | sed -n '1p')
fi

REFERENCE_PYTHON=${AROS_REFERENCE_PYTHON:-"$REFERENCE_BUILD/.venv/bin/python"}
if [ ! -x "$REFERENCE_PYTHON" ]; then
    REFERENCE_PYTHON=$(command -v python3 2>/dev/null || printf '%s' python3)
fi

COMPILER=
SYSROOT=unavailable
GCC_VERSION=unavailable
if [ -n "$REFERENCE_CROSSTOOLSDIR" ]; then
    COMPILER="$REFERENCE_CROSSTOOLSDIR/riscv-aros-gcc"
    if [ -x "$COMPILER" ]; then
        SYSROOT=$($COMPILER -print-sysroot 2>/dev/null || printf '%s' unavailable)
        GCC_VERSION=$($COMPILER -dumpfullversion -dumpversion 2>/dev/null || printf '%s' unavailable)
    fi
fi

print_report()
{
    printf '%s\n' 'ESP32-P4 Exec SMP configuration: REFUSED'
    printf 'E3 roadmap status: %s\n' "${E3_STATUS:-unavailable}"
    printf 'configure.in fail-closed guard: %s\n' "$HAS_SOURCE_GUARD"
    printf 'generated configure fail-closed guard: %s\n' "$HAS_GENERATED_GUARD"
    printf 'Reference build: %s\n' "$REFERENCE_BUILD"
    printf 'Reference configure argv: %s\n' "${REFERENCE_CONFIG_ARGS:-unavailable}"
    printf 'Reference cross-toolchain: %s\n' "${REFERENCE_CROSSTOOLSDIR:-unavailable}"
    printf 'Reference GCC version: %s\n' "$GCC_VERSION"
    printf 'Reference GCC sysroot: %s\n' "$SYSROOT"
    printf 'Reserved isolated build path: %s\n' "$SMP_BUILD_DIR"
    printf 'Build path exists: '
    if [ -e "$SMP_BUILD_DIR" ]; then printf '%s\n' yes; else printf '%s\n' no; fi
    printf '\n%s\n' 'Current blockers:'
    if [ "$HAS_SOURCE_GUARD" = yes ] || [ "$HAS_GENERATED_GUARD" = yes ]; then
        printf '%s\n' '  - configure explicitly rejects --enable-target-variant=smp for ESP32-P4.'
    fi
    printf '%s\n' '  - E3-A1/A2/A3 acceptance remains open; no second-hart Exec scheduler/runtime is integrated, so hart 1 must remain outside Exec.'
    printf '%s\n' '  - Generic spinlocks, task/signal state, semaphore queues, scheduler/trap/timer/FPU migration and matched module ABI still require qualification.'
    printf '\n%s\n' 'Future configuration inputs for a reviewed runtime integration (not executed):'
    printf '%s\n' '  --target=esp32p4-riscv'
    printf '%s\n' '  --enable-target-variant=smp'
    printf '%s\n' '  --enable-ccache --disable-crosstools'
    printf '%s\n' '  --with-aros-toolchain-install=<isolated toolchain view in the new build tree>'
    printf '%s\n' '  target_alias=esp32p4-riscv'
    printf '  PYTHON=%s\n' "$REFERENCE_PYTHON"
    printf '%s\n' 'Configure writes the generated riscv-aros-uname wrapper into AROS_CROSSTOOLSDIR; it must not point that variable at the reference toolchain directory.'
    printf '\n%s\n' 'Matched ABI prerequisites:'
    printf '%s\n' '  - New target output: bin/esp32p4-riscv-smp'
    printf '%s\n' '  - Refresh compiler/include public headers into both gen/include and AROS/Developer/include before compiling consumers.'
    printf '%s\n' '  - execbase.h is generated from compiler/include/exec/execbase.inc with EXECSMP field substitutions; public conditional layouts include exec/tasks.h, ports.h, semaphores.h, libraries.h, memory.h and interrupts.h.'
    printf '%s\n' '  - Rebuild the complete Exec, kernel, BSP and module consumer set in the isolated tree; mmake does not invalidate objects when only flags or makefiles change.'
    printf '%s\n' '  - GCC -print-sysroot still resolves to the reference build Developer tree. Do not patch it; verify every ABI consumer selects the isolated generated headers before any compile.'
    printf '%s\n' 'This helper is report/refusal-only. It creates no build directory and never builds, packages, flashes, or copies kernel/BSP outputs.'
}

if [ "$MODE" = report ]; then
    print_report
    exit 0
fi

printf '%s\n' 'Refusing to configure: this helper has no configure or build path.' >&2
printf '%s\n' 'The current configure guard stays in place until reviewed E3 runtime integration is ready; replace this helper as part of that integration.' >&2
exit 2
