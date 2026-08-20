/*
    Linker script for the esp32p4-riscv kickstart.

    Everything lands in one run of internal SRAM starting at 0x4FF00000,
    and nothing is linked into the high window at 0x4FF40000 even though
    there is room there. Two reasons, and the second is the important one:

      - The high window only exists as RAM when the L2 cache is smaller
        than 512 KB. On silicon before revision 3 the cache is carved out
        of the top of SRAM, and its size is set by software - which, for
        an image the ROM loader starts, is software that has not run yet.
        Linking the boot stack into an address that may or may not be
        cache is not a bet worth taking.

      - The high window and PSRAM belong to exec as heap, added to the
        memory list at runtime once their extent is known. That is a
        better use for them than link-time sections, and it needs no
        help from here.

    Where the run may end depends on who loads the image, and the lower
    of the two bounds is used, so that one image is valid either way:

      0x4FF29ED0  loaded from the bootloader offset by the first stage
                  ROM loader. This is the lowest address a working second
                  stage image is placed at, so everything below it is
                  demonstrably free while the ROM loader runs; the ROM
                  keeps its own variables at the top of the window
                  instead, from about 0x4FF3FFC8 up. 167 KB.

      0x4FF2CBD0  loaded from an app partition by a second stage
                  bootloader, this being where that bootloader's own
                  loader segment begins. 179 KB.

    Taking the lower costs 12032 bytes of headroom and buys an image that
    can be written to either 0x2000 or an app partition without being
    rebuilt. Pass -defsym P4_SRAM_TOP_OVERRIDE=0x4FF2CBD0 to spend that
    headroom on an image that is only ever loaded by a bootloader.
*/

OUTPUT_ARCH(riscv)
ENTRY(_start)

P4_SRAM_TOP = DEFINED(P4_SRAM_TOP_OVERRIDE) ? P4_SRAM_TOP_OVERRIDE : 0x4FF29ED0;

SECTIONS
{
    . = 0x4FF00000;

    .text : {
        __text_start = .;
        /*
         * An ESP-IDF bootloader reads the first bytes of segment 0 as an
         * application descriptor before it will load the image, so that
         * has to come before any code. See appdesc.c.
         */
        KEEP(*(.flash.appdesc))
        *(SORT_NONE(.aros.init*))
        *(.sramtext*)               /* see P4_SRAMCODE - already SRAM here */
        *(.text*)                   /* wildcard for "gcc -ffunction-sections" */
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
        __text_end = .;
    }

    .rodata : {
        __rodata_start = .;
        *(.rodata*)
        *(.srodata*)
        *(.gnu.linkonce.r.*)
        . = ALIGN(4);
        __rodata_end = .;
    }

    .data : {
        __data_start = .;
        *(.sramdata*)               /* see P4_SRAMDATA */
        *(.data*)                   /* wildcard for "gcc -fdata-sections" */
        *(.gnu.linkonce.d.*)
        . = ALIGN(16);
        start_ctors = .; *(SORT(.ctor*)) LONG(0)
        start_dtors = .; *(SORT(.dtor*)) LONG(0)
        . = ALIGN(4);
        __data_end = .;
    }

    /*
     * gp addresses the 4 KB around __global_pointer$, so .sdata and
     * .sbss have to sit next to it - which is why .bss follows straight
     * on rather than being placed elsewhere.
     */
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
        *(COMMON)                   /* "common" variables */
        . = ALIGN(4);
        __bss_end = .;
    }

    __kernel_end = .;

    /*
     * Where to look for resident modules, and which RAM the image itself
     * occupies. Here they are the same span; the XIP script splits them.
     */
    __romtags_start   = __text_start;
    __romtags_end     = __kernel_end;
    __kernel_lowest   = __text_start;
    __kernel_highest  = __kernel_end;

    /*
     * Overflowing this is the failure this port is most likely to hit,
     * and a linker error naming it is worth more than an image that
     * loads over whatever put it there.
     */
    ASSERT(__kernel_end <= P4_SRAM_TOP,
        "the kickstart does not fit below P4_SRAM_TOP - see the ceilings at the top of this script")

    /*
     * The CLIC reads the trap entry as mtvec[31:6] << 6, so anything less
     * than 64-byte aligned is silently rounded down to another address.
     * traps.S aligns it; this makes sure nothing has undone that.
     */
    ASSERT((__trap_entry & 0x3F) == 0,
        "__trap_entry must be 64-byte aligned - the CLIC ignores the low six bits of mtvec")

    /*
     * The unwind tables are the single largest thing in this image that
     * nothing reads: 25164 bytes of .eh_frame against a total of 120544,
     * and the backtrace walks the frame pointer chain instead (see
     * UnwindFrame in arch/riscv-all/exec/alert_cpu.c). There are no C++
     * exceptions here either. Keeping them cost more than debug.library
     * does, which is what made the difference between an image the ROM
     * loader takes and one it drops on the floor.
     */
    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.riscv.attributes)
        *(.comment)
        *(.note*)
    }
}
