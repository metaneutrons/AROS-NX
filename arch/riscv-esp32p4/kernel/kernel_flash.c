/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Reading flash from a kernel that is executing out of it.

    The kickstart runs XIP: its .text and .rodata are mapped from the app
    partition by the second stage bootloader and fetched through the cache.
    To load anything from another partition it has to reach flash that is
    not mapped, and there are two ways to do that.

    The ROM's SPI flash driver is one. It talks to the controller directly,
    so it needs the external memory cache off for the whole transfer, and
    with the cache off PSRAM is unreachable too - both go through the same
    L2 - so the destination has to be an internal SRAM staging buffer and
    the transfer has to be chunked. It also needs the ROM's chip
    descriptor, reached through a pointer at 0x4FF3FFE8 whose target may
    sit in memory this port hands to exec.

    The MMU is the other, and it is what this file does. One entry per
    64 KB page makes the partition byte addressable in the flash window,
    after which reading it is a load. The cache has to be off only for the
    entry writes, which is a handful of register stores rather than a
    megabyte of transfer, and the result is readable with the cache on and
    at cache speed.

    Two things it cannot do, both recorded here so they are not discovered
    later. It cannot write, so flashing from the running system will need
    the ROM path after all. And cache mapped flash stops at 16 MB in this
    bootloader configuration: components/spi_flash/flash_mmap.c rejects any
    range that so much as crosses 0x1000000 unless
    CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH is set, which needs
    IDF_EXPERIMENTAL_FEATURES and which this port's bootloader does not
    set. The package partition is placed below that line deliberately; the
    board's storage partition, at 0x1020000, is above it and will need the
    ROM path when it becomes SYS:.

    The flash MMU is a different register block from the PSRAM MMU this
    port already programs, with a different entry layout. Reusing psram.h's
    constants here would be wrong in three ways at once: wrong base, wrong
    valid bit and one bit too few for the page number.
*/

#include <inttypes.h>

#include <asm/cpu.h>

#include "hardware.h"
#include "kernel_intern.h"

/*
 * components/soc/esp32p4/register/hw_ver1/soc/reg_base.h:52 and
 * spi_mem_c_reg.h:2630,2642. The PSRAM pair is +0x2000 from here, which is
 * how close these two are to being confused.
 */
#define P4_MSPI_FLASH_BASE      0x5008C000UL
#define P4_MMU_FLASH_CONTENT    (P4_MSPI_FLASH_BASE + 0x37C)
#define P4_MMU_FLASH_INDEX      (P4_MSPI_FLASH_BASE + 0x380)

/*
 * ext_mem_defs.h:57-78. SOC_MMU_FLASH_VALID is BIT(12) where PSRAM's is
 * BIT(11), and there is no access bit: SOC_MMU_ACCESS_FLASH is zero, so
 * bit 10 here is the top bit of an eleven bit page number rather than a
 * target selector.
 */
#define P4_MMU_FLASH_VALID      (1UL << 12)
#define P4_MMU_FLASH_SENSITIVE  (1UL << 13)
#define P4_MMU_FLASH_PAGE_MASK  0x7FFUL

#define P4_MMU_ENTRY_COUNT      1024UL
#define P4_MMU_PAGE_SIZE        0x10000UL
#define P4_MMU_PAGE_SHIFT       16
#define P4_MMU_VADDR_MASK       0x3FFFFFFUL     /* 1024 pages of 64 KB */

/* ext_mem_defs.h:22-23 and :31. Instruction and data share the window. */
#define P4_FLASH_WINDOW_LOW     0x40000000UL
#define P4_FLASH_WINDOW_HIGH    0x44000000UL

/* The line beyond which a mapped range is refused, see the file comment */
#define P4_FLASH_MMAP_LIMIT     0x1000000UL

/*
 * Where this file maps what it reads. Entry 256 of 1024, chosen to be far
 * from both the image's own mapping at the bottom and the boot partition
 * page the bootloader leaves at the top. krnP4FlashSurvey() is what
 * establishes that it is free; it is called first for that reason.
 */
#define P4_FLASH_SCRATCH_VADDR  0x41000000UL

#define P4_PARTTABLE_OFFSET     0x8000UL
#define P4_PARTTABLE_SIZE       0xC00UL
#define P4_PARTTABLE_ENTRIES    (P4_PARTTABLE_SIZE / 32)

#define P4_PART_MAGIC           0x50AA
#define P4_PART_MD5_MAGIC       0xEBEB
#define P4_PART_TERMINATOR      0xFFFF

#define P4_FLASH_SIZE           0x2000000UL     /* 32 MB, read off the chip */

static unsigned long flash_scratch_paddr = ~0UL;
static unsigned long flash_scratch_pages;

__attribute__((always_inline)) static inline void mmu_wr(unsigned long entry, uint32_t val)
{
    *(volatile uint32_t *)P4_MMU_FLASH_INDEX = (uint32_t)entry;
    *(volatile uint32_t *)P4_MMU_FLASH_CONTENT = val;
}

__attribute__((always_inline)) static inline uint32_t mmu_rd(unsigned long entry)
{
    *(volatile uint32_t *)P4_MMU_FLASH_INDEX = (uint32_t)entry;
    return *(volatile uint32_t *)P4_MMU_FLASH_CONTENT;
}

__attribute__((always_inline)) static inline unsigned long mmu_entry_of(unsigned long vaddr)
{
    return (vaddr & P4_MMU_VADDR_MASK) >> P4_MMU_PAGE_SHIFT;
}

/*
 * Write the entries for a range. P4_SRAMCODE, and the only thing in this
 * file that is: it runs with the cache suspended, so every instruction it
 * executes and every byte it reads has to be somewhere other than the
 * window it is changing. Nothing in it prints, divides or copies a struct,
 * for the same reason.
 *
 * Reading entries needs none of this. ESP-IDF's own paddr-to-vaddr lookup
 * (esp_mm/esp_mmu_map.c:820) reads them under nothing but a mutex, because
 * selecting an entry with the index register does not change any mapping.
 */
P4_SRAMCODE static void flash_map_entries(unsigned long entry,
                                          unsigned long page,
                                          unsigned long pages,
                                          unsigned long release,
                                          unsigned long flags)
{
    unsigned long status = csr_read(mstatus);
    unsigned long token;
    unsigned long i;

    /*
     * The timer is already live when the partition table is mapped. An
     * interrupt taken after L2 has been suspended would fetch the trap
     * entry and dispatcher from flash and stop before it could report why.
     * Preserve rather than assume the caller's interrupt state: this path
     * will later also be used after exec is running.
     */
    csr_clear(mstatus, MSTATUS_MIE);
    token = krnP4CacheOff();

    /* Whatever the scratch window held before, in the same window, so the
       cache is suspended once rather than twice */
    for (i = 0; i < release; i++)
        mmu_wr(entry + i, 0);

    for (i = 0; i < pages; i++)
        mmu_wr(entry + i, (uint32_t)(((page + i) & P4_MMU_FLASH_PAGE_MASK) |
                                     flags));

    krnP4CacheOn(token);

    if (status & MSTATUS_MIE)
        csr_set(mstatus, MSTATUS_MIE);
}

/*
 * Mirror the encryption-sensitive flag from a mapping the bootloader made.
 * The running XIP image guarantees that at least one valid flash entry
 * exists, and its flag says whether the cache decrypts mapped reads on this
 * board. Reading the eFuse a second way would add a silicon-revision-specific
 * dependency for information the live MMU already states unambiguously.
 */
static unsigned long flash_mapping_flags(void)
{
    unsigned long i;

    for (i = 0; i < P4_MMU_ENTRY_COUNT; i++)
    {
        uint32_t v = mmu_rd(i);

        if (v & P4_MMU_FLASH_VALID)
            return P4_MMU_FLASH_VALID | (v & P4_MMU_FLASH_SENSITIVE);
    }

    return P4_MMU_FLASH_VALID;
}

/*
 * Report every valid entry in the flash MMU, changing nothing.
 *
 * This is the first thing in this port to touch the flash controller's
 * register block at all, and what it prints is the map the bootloader left
 * behind: which pages of flash are reachable, at which addresses, and
 * therefore which entries are free for anything else. Deriving that from
 * the bootloader's source would be reasoning; reading it back is not.
 */
void krnP4FlashSurvey(void)
{
    unsigned long i;
    unsigned long valid = 0;

    krnP4PutStr("[flash] mmu   ");

    for (i = 0; i < P4_MMU_ENTRY_COUNT; i++)
    {
        uint32_t v = mmu_rd(i);

        if (!(v & P4_MMU_FLASH_VALID))
            continue;

        if (valid)
            krnP4PutStr(", ");

        krnP4PutStr("e");
        krnP4PutDec((uint32_t)i);
        krnP4PutStr(" -> page ");
        krnP4PutDec((uint32_t)(v & P4_MMU_FLASH_PAGE_MASK));
        valid++;
    }

    if (!valid)
        krnP4PutStr("no valid entries, which cannot be true while this "
                    "code is executing");

    krnP4PutStr("\n");
}

/*
 * Make a flash range readable at P4_FLASH_SCRATCH_VADDR.
 *
 * Returns the address the range starts at, or NULL. The range is rounded
 * out to whole 64 KB pages because that is the only granularity the MMU
 * has, so the caller gets back an address inside the first page rather
 * than at its start.
 */
void *krnP4FlashMap(unsigned long paddr, unsigned long len)
{
    unsigned long page;
    unsigned long last;
    unsigned long pages;
    unsigned long entry = mmu_entry_of(P4_FLASH_SCRATCH_VADDR);
    unsigned long i;

    /* Subtraction form, so an untrusted length cannot wrap the addition. */
    if (!len || paddr >= P4_FLASH_SIZE || len > P4_FLASH_SIZE - paddr)
        return NULL;

    /*
     * The 16 MB ceiling. Refusing here rather than creating an entry that
     * looks valid while the bootloader's 24-bit flash-address setup cannot
     * service it reliably.
     */
    if (paddr >= P4_FLASH_MMAP_LIMIT ||
        len > P4_FLASH_MMAP_LIMIT - paddr)
    {
        krnP4PutStr("[flash] range crosses the 16 MB cache mapping limit\n");
        return NULL;
    }

    page = paddr >> P4_MMU_PAGE_SHIFT;
    last = (paddr + len - 1) >> P4_MMU_PAGE_SHIFT;
    pages = last - page + 1;

    if (P4_FLASH_SCRATCH_VADDR + pages * P4_MMU_PAGE_SIZE > P4_FLASH_WINDOW_HIGH)
        return NULL;

    /*
     * Never write over a mapping that is already in use - the image's own
     * code is behind one of them. Entries this function mapped last time
     * are not in use, and are released rather than refused.
     */
    for (i = flash_scratch_pages; i < pages; i++)
    {
        if (mmu_rd(entry + i) & P4_MMU_FLASH_VALID)
        {
            krnP4PutStr("[flash] scratch entry ");
            krnP4PutDec((uint32_t)(entry + i));
            krnP4PutStr(" is already mapped\n");
            return NULL;
        }
    }

    flash_map_entries(entry, page, pages, flash_scratch_pages,
                      flash_mapping_flags());
    flash_scratch_pages = pages;

    /*
     * The range means something different now than whatever the cache last
     * held for it, so it has to be invalidated before the first read. This
     * is the data side of the same call the ELF loader needs for the
     * instruction side.
     */
    krnP4SyncCode((void *)P4_FLASH_SCRATCH_VADDR, pages * P4_MMU_PAGE_SIZE);

    flash_scratch_paddr = page << P4_MMU_PAGE_SHIFT;

    return (void *)(P4_FLASH_SCRATCH_VADDR + (paddr - flash_scratch_paddr));
}

static inline uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A partition label is 16 bytes, NUL padded rather than NUL terminated */
static int label_match(const unsigned char *label, const char *want)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        if (want[i] != (char)label[i])
            return 0;
        if (!want[i])
            return 1;
    }

    return want[16] == 0;
}

static void put_label(const unsigned char *p)
{
    int i;

    for (i = 0; i < 16 && p[i]; i++)
        krnP4PutC((char)p[i]);
    for (; i < 16; i++)
        krnP4PutC(' ');
}

/*
 * Walk the partition table and report it, optionally resolving one entry.
 *
 * The format is a run of 32 byte records at flash offset 0x8000: magic
 * 0x50AA is a partition, 0xEBEB is the MD5 marker that ends the run, and
 * magic 0xFFFF with type and subtype 0xFF is the terminator the bootloader
 * accepts at any index above the first. Anything else means the table is
 * not a table, and that is treated as fatal rather than skipped: a
 * mis-parse here would resolve the package to an arbitrary flash offset.
 *
 * The MD5 is deliberately not verified. The ROM's MD5 entry points move
 * between silicon revisions - 0x4FC005EC in the base ROM linker script
 * against 0x4FC005E0 in the ECO5 one - so hardcoding them would tie this
 * port to pre-ECO5 parts for a check against a corruption mode the build
 * already prevents. The magics and the terminator are checked instead.
 *
 * Returns 1 when a wanted partition was found, 0 when the table parsed but
 * held no such entry, and -1 when the table did not parse.
 */
int krnP4PartitionScan(unsigned char want_type, const char *want_label,
                       unsigned long *out_off, unsigned long *out_size,
                       int report)
{
    const unsigned char *tbl;
    unsigned long i;
    int found = 0;

    tbl = krnP4FlashMap(P4_PARTTABLE_OFFSET, P4_PARTTABLE_SIZE);
    if (!tbl)
    {
        krnP4PutStr("[flash] could not map the partition table\n");
        return -1;
    }

    for (i = 0; i < P4_PARTTABLE_ENTRIES; i++)
    {
        const unsigned char *e = tbl + i * 32;
        uint16_t magic = rd16(e);
        unsigned long off, size;
        int match;

        if (magic == P4_PART_MD5_MAGIC)
            break;

        if (magic == P4_PART_TERMINATOR && e[2] == 0xFF && e[3] == 0xFF)
        {
            if (i == 0)
            {
                krnP4PutStr("[flash] the partition table is empty\n");
                return -1;
            }
            break;
        }

        if (magic != P4_PART_MAGIC)
        {
            krnP4PutStr("[flash] entry ");
            krnP4PutDec((uint32_t)i);
            krnP4PutStr(" has magic ");
            krnP4PutHex32(magic);
            krnP4PutStr(", not a partition table\n");
            return -1;
        }

        off = rd32(e + 4);
        size = rd32(e + 8);

        if (!size || off >= P4_FLASH_SIZE || size > P4_FLASH_SIZE - off)
        {
            krnP4PutStr("[flash] entry ");
            krnP4PutDec((uint32_t)i);
            krnP4PutStr(" does not fit the chip\n");
            return -1;
        }

        match = (e[2] == want_type) && want_label &&
                label_match(e + 12, want_label);

        if (report)
        {
            krnP4PutStr("[flash] part  ");
            put_label(e + 12);
            krnP4PutStr(" type ");
            krnP4PutHex32(e[2]);
            krnP4PutStr(" sub ");
            krnP4PutHex32(e[3]);
            krnP4PutStr(" at ");
            krnP4PutHex32((uint32_t)off);
            krnP4PutStr(" size ");
            krnP4PutHex32((uint32_t)size);
            if (match)
                krnP4PutStr("  <- wanted");
            krnP4PutStr("\n");
        }

        if (match && !found)
        {
            if (out_off)
                *out_off = off;
            if (out_size)
                *out_size = size;
            found = 1;
        }
    }

    return found;
}
