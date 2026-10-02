OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    __global_pointer$ = . + 0x800;
    .sdata : { *(.sdata .sdata.*) }
    .bss : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) }
}
