OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .sdata : { __global_pointer$ = . + 0x800; *(.sdata .sdata.*) }
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
}
