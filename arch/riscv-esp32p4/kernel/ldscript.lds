/*
    Linker script for the esp32p4-riscv kickstart.

    This is built as an ESP32-P4 application image, the kind a second
    stage bootloader already on the device loads out of an app partition.
    That keeps the bootloader out of what AROS distributes - it is
    whatever the board came with - and lets the existing partition table
    and OTA data stand.

    The window is the one the bootloader leaves free, and it is a
    property of the silicon revision. These are the pre-v3 numbers, which
    is the revision this port has been able to test on:

      0x4FF00000 - 0x4FF2CBD0  179 KB, code and data, ends where the
                               bootloader's own loader segment begins
      0x4FF2CBD0 - 0x4FF40000   77 KB, reserved by the ROM
      0x4FF40000 +             512 KB minus the L2 cache size, data only
      top of SRAM               the L2 cache, 128, 256 or 512 KB

    From v3 on the cache moved to the bottom of SRAM and the rest is one
    contiguous window; the numbers for that go in beside these when there
    is silicon to check them against. See the port README.

    Executable code therefore has 179 KB to fit in. .data and .bss go to
    the high window, which is where the room is, and only .text and
    .rodata compete for the low one.
*/

OUTPUT_ARCH(riscv)
ENTRY(_start)

/* L2 cache size, 0x20000 / 0x40000 / 0x80000. The high window ends
   where the cache begins. */
P4_L2_CACHE_SIZE = DEFINED(P4_L2_CACHE_SIZE) ? P4_L2_CACHE_SIZE : 0x20000;

MEMORY
{
    sram_low  (rwx) : ORIGIN = 0x4FF00000, LENGTH = 0x2CBD0
    sram_high (rw)  : ORIGIN = 0x4FF40000, LENGTH = 0x80000 - 0x20000
}

SECTIONS
{
    .text : {
        __text_start = .;
        *(SORT_NONE(.aros.init*))
        *(.text*)                   /* wildcard for "gcc -ffunction-sections" */
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
        __text_end = .;
    } > sram_low

    .rodata : {
        __rodata_start = .;
        *(.rodata*)
        *(.srodata*)
        *(.gnu.linkonce.r.*)
        *(.eh_frame_hdr)
        *(.eh_frame)
        . = ALIGN(4);
        __rodata_end = .;
    } > sram_low

    .data : {
        __data_start = .;
        *(.data*)                   /* wildcard for "gcc -fdata-sections" */
        *(.gnu.linkonce.d.*)
        . = ALIGN(16);
        start_ctors = .; *(SORT(.ctor*)) LONG(0)
        start_dtors = .; *(SORT(.dtor*)) LONG(0)
        . = ALIGN(4);
        __data_end = .;
    } > sram_high

    /*
     * gp addresses the 4 KB around __global_pointer$, so it has to sit
     * with .sdata and .sbss, which is why they are adjacent and in the
     * same region.
     */
    .sdata : {
        __global_pointer$ = . + 0x800;
        *(.sdata*)
    } > sram_high

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.sbss*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > sram_high

    __kernel_end = .;

    /*
     * Nothing is silently allowed to spill. Overflowing sram_low is the
     * failure this port is most likely to hit, and a linker error naming
     * it is worth more than an image that loads over the bootloader.
     */
    ASSERT(__rodata_end <= 0x4FF2CBD0,
        "kickstart .text/.rodata overflow the 179 KB below the bootloader's loader segment")
    ASSERT(__kernel_end <= 0x4FF40000 + 0x80000 - P4_L2_CACHE_SIZE,
        "kickstart .data/.bss overflow the high SRAM window left by the L2 cache")
}
