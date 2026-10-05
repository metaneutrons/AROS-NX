#!/bin/sh
# Standalone fixture only; produces no flashable image and touches no hardware.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: sh $0 CROSS_TOOL_BIN EVIDENCE_DIRECTORY" >&2
    exit 2
fi
toolbin=$1
evidence=$2
kernel=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$evidence"
"$toolbin/riscv-aros-gcc" -march=rv32imafc_zicsr_zifencei -mabi=ilp32f \
    -c "$kernel/secondary_entry.S" -o "$evidence/secondary_entry.o"
"$toolbin/riscv-aros-ld" -T "$kernel/tests/secondary-entry.lds" \
    -o "$evidence/secondary-entry.elf" "$evidence/secondary_entry.o"
sh "$kernel/check-sramtext.sh" "$toolbin/riscv-aros-objdump" \
    "$evidence/secondary-entry.elf"
"$toolbin/riscv-aros-objdump" -dr "$evidence/secondary-entry.elf" \
    > "$evidence/secondary-entry.dis"
if rg 'jal|call|__boot|kernel_cstart|krnStartExec' "$evidence/secondary-entry.dis"; then
    echo "unexpected call or primary/shared startup reference" >&2
    exit 1
fi
"$toolbin/riscv-aros-ld" --defsym=P4_TEST_BASE=0x40000000 \
    -T "$kernel/tests/secondary-entry.lds" \
    -o "$evidence/secondary-entry-invalid-xip.elf" "$evidence/secondary_entry.o"
if sh "$kernel/check-sramtext.sh" "$toolbin/riscv-aros-objdump" \
    "$evidence/secondary-entry-invalid-xip.elf" > "$evidence/counter-probe.log" 2>&1; then
    echo "invalid XIP fixture was accepted" >&2
    exit 1
fi
if ! rg -q 'error: .sramtext refers to the XIP flash window:' "$evidence/counter-probe.log"; then
    echo "counter-probe failed for an unexpected reason" >&2
    exit 1
fi
echo "secondary entry: isolated link/residency and XIP counter-probe passed"
