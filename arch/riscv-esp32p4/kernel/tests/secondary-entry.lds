/* Isolated link fixture; never an ESP32 boot image. Override P4_TEST_BASE
 * only for the intentionally invalid XIP-residency counter-probe. */
OUTPUT_ARCH(riscv)
ENTRY(__p4_secondary_entry)
SECTIONS
{
    . = DEFINED(P4_TEST_BASE) ? P4_TEST_BASE : 0x4ff00000;
    .sramtext : { *(.sramtext*) }
    .bss (NOLOAD) : { *(.bss*) }
    __global_pointer$ = ADDR(.bss) + 0x800;
    ASSERT((__p4_secondary_entry & 63) == 0, "entry alignment")
    ASSERT((__p4_secondary_trap & 63) == 0, "trap alignment")
    ASSERT((__p4_secondary_report & 63) == 0, "mailbox alignment")
    ASSERT((__p4_secondary_stack_end & 15) == 0, "stack alignment")
    ASSERT(__p4_secondary_stack_end - __p4_secondary_stack == 4096,
        "private stack size")
    /DISCARD/ : { *(.riscv.attributes) *(.comment) }
}
