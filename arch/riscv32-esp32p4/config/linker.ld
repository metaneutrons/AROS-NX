/*
    ESP32-P4 Linker Script for AROS Standalone Kernel
    Internal SRAM (TCM): 0x4FF00000 (768KB, Zero-Wait-State @ 400MHz)
    External PSRAM:      0x48000000 (32MB Fast Memory Heap)
*/

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x4FF00000, LENGTH = 768K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > SRAM

    .data : {
        . = ALIGN(4);
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > SRAM
    __data_load = LOADADDR(.data);

    .bss : {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SRAM

    _end = .;
}
