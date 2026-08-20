/*
    Linker script for the esp32p4-riscv kickstart, code and read-only data
    executed from flash.

    The variant of ldscript.lds for the app partition path only. Where that
    script puts everything in internal SRAM, this one leaves .text and
    .rodata in flash and has the second stage bootloader map them through
    the cache, which is what an ESP-IDF application does and what the
    loader is built to expect.

    Two things follow from it, and the second is the reason to do it.

      - The four bootloader patches this port needed exist because an
        image with no flash-mapped segment breaks assumptions in the
        loader. An image shaped like this needs none of them.

      - The size of AROS's code stops being bounded by internal SRAM.
        Today that bound costs 115 KB of 523 KB, which is bad enough; the
        point is that a megabyte of Intuition and graphics would not fit
        at all, and here it would not have to.

    The flash window is 0x40000000-0x44000000, 64 MB, and instruction and
    data mappings share it. The mapped segment must be first in the image
    and its virtual address must sit at the same offset within a 64 KB MMU
    page as its physical address does. ota_0 begins at 0x20000, which is
    page aligned, and the image and segment headers take 0x20 bytes, hence
    0x40000020.

    Not usable on the ROM boot path: the first stage loader is not asked
    to set up these mappings. Use ldscript.lds for that.
*/

OUTPUT_ARCH(riscv)
ENTRY(_start)

P4_FLASH_BASE = 0x40000020;
P4_SRAM_BASE  = 0x4FF00000;
P4_SRAM_TOP   = DEFINED(P4_SRAM_TOP_OVERRIDE) ? P4_SRAM_TOP_OVERRIDE : 0x4FF2CBD0;

SECTIONS
{
    /*
     * The address is given explicitly rather than through the location
     * counter, because an input section with a stronger alignment would
     * otherwise push the start to 0x40000040. esptool then has to pad the
     * file with the SRAM segment to reach the alignment, that segment
     * becomes segment 0, and the bootloader looks for the application
     * descriptor in it.
     */
    .flash.text P4_FLASH_BASE : {
        __text_start = .;
        /*
         * The bootloader reads the first bytes of segment 0 as an
         * application descriptor, so it has to come before any code -
         * see appdesc.c.
         */
        KEEP(*(.flash.appdesc))
        *(SORT_NONE(.aros.init*))
        *(.text*)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
        __text_end = .;
    }

    .flash.rodata : {
        __rodata_start = .;
        *(.rodata*)
        *(.srodata*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
        __rodata_end = .;
    }

    . = P4_SRAM_BASE;

    /*
     * Code that has to run while the cache is off, and the data it reads.
     * Kept out of the flash run above on purpose - see P4_SRAMCODE.
     */
    .sramtext : {
        __sramtext_start = .;
        *(.sramtext*)
        . = ALIGN(4);
        __sramtext_end = .;
    }

    .data : {
        __data_start = .;
        *(.sramdata*)
        *(.data*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(16);
        start_ctors = .; *(SORT(.ctor*)) LONG(0)
        start_dtors = .; *(SORT(.dtor*)) LONG(0)
        . = ALIGN(4);
        __data_end = .;
    }

    .sdata : {
        __global_pointer$ = . + 0x800;
        *(.sdata*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start = .;
        *(.sbss*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    }

    __kernel_end = .;

    /*
     * The modules are in the flash-mapped run; the RAM the image occupies
     * is only the small SRAM run at the other end of the address space.
     * Scanning from one to the other would walk 200 MB of mostly unmapped
     * address space, which faults about as far as the first module past
     * the romtags.
     */
    __romtags_start   = __text_start;
    __romtags_end     = __rodata_end;
    __kernel_lowest   = P4_SRAM_BASE;
    __kernel_highest  = __kernel_end;

    ASSERT(__kernel_end <= P4_SRAM_TOP,
        "the SRAM part of the image does not fit below P4_SRAM_TOP")

    ASSERT((__trap_entry & 0x3F) == 0,
        "__trap_entry must be 64-byte aligned - the CLIC ignores the low six bits of mtvec")

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.riscv.attributes)
        *(.comment)
        *(.note*)
    }
}
