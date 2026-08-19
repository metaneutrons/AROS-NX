/*
    ESP32-P4 Linker Script for AROS Standalone Kernel
    PSRAM: 0x48000000 (32MB)
    SRAM:  0x4FF00000 (768KB)
*/

MEMORY
{
    FLASH_APP (rx) : ORIGIN = 0x48020000, LENGTH = 8M
    SRAM (rwx)     : ORIGIN = 0x4FF00000, LENGTH = 768K
}

ENTRY(_start)

SECTIONS
{
    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
        *(.rodata .rodata.*)
    } > FLASH_APP

    .data : {
        . = ALIGN(4);
        __data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > SRAM AT > FLASH_APP
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
