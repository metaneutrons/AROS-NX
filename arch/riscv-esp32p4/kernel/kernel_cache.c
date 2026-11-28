/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Cache maintenance, and the window in which the external memory
          cache is off.

    Two different jobs live here and they have opposite requirements.

    Maintenance runs with the cache enabled. Its reason to exist is that
    this port writes instruction bytes through the data path - the ELF
    loader placing module code in PSRAM, and __AROS_SET_FULLJMP building a
    library jump table entry - and an instruction fetch afterwards must not
    read a stale line. On this SoC that is three steps, not one: write back
    the data caches so the bytes reach memory, invalidate the instruction
    caches so the fetch goes out again, and fence.i so the hart's own
    fetch pipeline is flushed. The shared riscv layer's CacheClearE() is a
    bare `fence rw, rw`, which orders accesses and clears nothing.

    Turning the cache off is the other job. Writing an MMU entry while the
    cache is live means changing what an address means underneath lines
    that already hold its old contents, so the entry write has to happen
    with the cache suspended. Three things make that window delicate.
    Every instruction executed inside it must be fetched from somewhere
    other than the window being suspended, which is what P4_SRAMCODE is
    for. The branch predictor has to be off, because a speculative fetch
    into a suspended cache is exactly what the sequence is trying to
    avoid. And the internal-memory L1 data cache has to be written back
    first: PSRAM is live by this point, and its auto-writeback would
    otherwise deadlock against a suspended L2.

    The ROM entry points below are identical in the base and the ECO5 ROM
    linker scripts, which is worth stating because several other ROM
    symbols this port might want are not.
*/

#include <inttypes.h>

#include <aros/cacheops.h>

#include "hardware.h"
#include "kernel_intern.h"

/*
 * components/esp_rom/esp32p4/ld/esp32p4.rom.ld, and the same addresses in
 * esp32p4.rom.eco5.ld. Prototypes from
 * components/esp_rom/esp32p4/include/esp32p4/rom/cache.h.
 */
#define P4_ROM_CACHE_INVALIDATE_ADDR    0x4FC003E4UL
#define P4_ROM_CACHE_WRITEBACK_ADDR     0x4FC003F4UL
#define P4_ROM_CACHE_WRITEBACK_ALL      0x4FC00414UL
#define P4_ROM_CACHE_SUSPEND_L2         0x4FC00508UL
#define P4_ROM_CACHE_RESUME_L2          0x4FC0050CUL

typedef int (*rom_cache_range_t)(uint32_t map, uint32_t addr, uint32_t size);
typedef int (*rom_cache_all_t)(uint32_t map);
typedef uint32_t (*rom_cache_suspend_t)(void);
typedef void (*rom_cache_resume_t)(uint32_t autoload);

/* cache.h: BIT(0) and BIT(1) are the two L1 instruction caches, BIT(4) the
   L1 data cache, BIT(5) the shared L2. */
#define P4_CACHE_MAP_L1_ICACHE_0    (1UL << 0)
#define P4_CACHE_MAP_L1_ICACHE_1    (1UL << 1)
#define P4_CACHE_MAP_L1_DCACHE      (1UL << 4)
#define P4_CACHE_MAP_L2             (1UL << 5)

#define P4_CACHE_MAP_ALL    (P4_CACHE_MAP_L1_ICACHE_0 | P4_CACHE_MAP_L1_ICACHE_1 | \
                             P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2)
#define P4_CACHE_MAP_ICACHES (P4_CACHE_MAP_L1_ICACHE_0 | P4_CACHE_MAP_L1_ICACHE_1)

/* components/riscv/include/riscv/rv_utils.h: MHCR is CSR 0x7c1, and these
   three bits are the return stack, the predictive jump and the branch
   target buffer. */
#define P4_MHCR             0x7C1
#define P4_MHCR_PREDICTOR   ((1UL << 4) | (1UL << 5) | (1UL << 12))

/* Cache_Suspend_L2_Cache() returns this one-bit autoload state. The rest
   of our token carries the predictor state that was present before the
   cache-off window. */
#define P4_CACHE_AUTOLOAD    (1UL << 0)

/*
 * The ROM's cache maintenance drives one shared sync unit (range, map and
 * size registers, then a wait for its completion). Two harts doing it at
 * once, one for an executable it has just loaded and the other for the
 * frame it has just drawn or the SD buffer it hands to the controller,
 * would corrupt each other's request. Every maintenance call here, and
 * the ones drivers make through KATTR_CacheOps, takes this lock, which
 * holds interrupts masked and serves the other hart's cache-off window
 * request while it waits. The cache-off window itself (krnP4CacheOff()) is
 * not under it: the other hart is parked while it lasts.
 */
P4_SRAMDATA static volatile ULONG cache_lock;

/*
 * Make an address range coherent for instruction fetch after it has been
 * written through the data path.
 *
 * Write back before invalidating, and in that order: the bytes have to be
 * in memory before the instruction side is told to fetch them again. The
 * ROM routines take a map of cache levels and do the address arithmetic
 * themselves, so a range that is not line aligned is handled for us.
 *
 * Only the two L1 instruction caches are invalidated. This used to
 * invalidate the L1 data cache and the L2 as well, on the reasoning that
 * the fetch should "go out again". That discards the dirty lines the range
 * shares with its neighbours: a range starting or ending inside a cache
 * line takes the rest of the line with it, and what the other hart wrote
 * to that rest between the write-back and the invalidate is lost. A heap
 * block header next to a freshly loaded hunk is such a neighbour, and
 * SMP-Migrate (developer/debug/test/smp, phase 4) shows the loss: a clear
 * of eight bytes in a line while the other hart writes to the line's other
 * half lost over a hundred writes per million clears, and the heap's free
 * list was corrupted in the load test. After the write-back the L1 data
 * cache and the L2 hold nothing that is newer than memory for this range,
 * and the instruction caches refill from the L2 (or memory) with the new
 * bytes, so nothing else needs to be dropped.
 *
 * P4_SRAMCODE although the cache is enabled throughout. ESP-IDF places its
 * own equivalents (cache_hal, esp_cache_msync) in internal RAM in every
 * build that is not a pure-RAM application, which is the only available
 * evidence on whether running this out of the window whose cache it
 * manipulates is safe. Two ROM calls and a fence cost a few dozen bytes of
 * .sramtext, so there is nothing to weigh against.
 */
P4_SRAMCODE void krnP4SyncCode(void *addr, unsigned long len)
{
    rom_cache_range_t wb = (rom_cache_range_t)P4_ROM_CACHE_WRITEBACK_ADDR;
    rom_cache_range_t inv = (rom_cache_range_t)P4_ROM_CACHE_INVALIDATE_ADDR;
    unsigned long state;

    if (!len)
        return;

    state = krnP4LockTake(&cache_lock);
    wb(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2, (uint32_t)(unsigned long)addr,
       (uint32_t)len);
    inv(P4_CACHE_MAP_ICACHES, (uint32_t)(unsigned long)addr, (uint32_t)len);
    krnP4LockRelease(&cache_lock, state);

    asm volatile("fence.i" ::: "memory");
}

/*
 * Suspend the external memory cache and return the token that has to be
 * handed back to krnP4CacheOn().
 *
 * The token contains the L2 autoload state and the three MHCR predictor
 * bits. Cache_Resume_L2_Cache() needs the former, while the latter must be
 * restored exactly: pausing a cache must not silently turn predictors on
 * when firmware deliberately left one of them off.
 *
 * Interrupts must already be disabled and the active stack must be in
 * internal SRAM. The caller cannot service an interrupt or spill a stack
 * word through PSRAM while L2 is suspended. flash_map_entries() enforces
 * the interrupt half and, during early boot, runs on the SRAM boot stack.
 *
 * Suspend rather than disable, following the runtime path in
 * components/esp_mm/esp_mmu_map.c. ESP-IDF is not consistent about this:
 * its bootloader MMU path disables instead, and the ROM header describes
 * Cache_Disable_L2_Cache and Cache_Suspend_L2_Cache in word for word
 * identical terms. If a hang inside the window ever says otherwise, the
 * disable pair is the fallback, and it needs the autoload bit read out of
 * the cache registers because there is no snapshot to restore from.
 */
/*
 * Push every dirty line out to memory, and leave the caches running.
 *
 * The DMA reads a frame from PSRAM over AXI and does not see the caches, so a
 * frame written by the CPU is invisible to it until the lines are written back.
 * A frame that is half in cache does not fail visibly: it shows as a partly
 * wrong image, which is the kind of fault that gets blamed on timing.
 *
 * The ROM's own writeback-all is used rather than a loop over the range: it
 * knows the line size and the level map, and this runs once per frame at most.
 */
void krnP4CacheWriteback(void)
{
    rom_cache_all_t wb_all = (rom_cache_all_t)P4_ROM_CACHE_WRITEBACK_ALL;
    unsigned long state = krnP4LockTake(&cache_lock);

    (void)wb_all(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2);
    krnP4LockRelease(&cache_lock, state);
    asm volatile("fence" ::: "memory");
}

/*
 * Publish only one data range to an external DMA reader.
 *
 * Unlike krnP4CacheSyncData(), this deliberately leaves the CPU's data lines
 * valid: a graphics producer has just written them and the display DMA only
 * needs the bytes pushed to PSRAM.  The ROM routine accepts unaligned ranges
 * and expands them to cache-line boundaries.  B6 calls this once or twice per
 * affected physical row after rotating a bounded logical dirty rectangle.
 */
P4_SRAMCODE void krnP4CacheWritebackData(void *addr, unsigned long len)
{
    rom_cache_range_t wb = (rom_cache_range_t)P4_ROM_CACHE_WRITEBACK_ADDR;
    unsigned long state;

    if (!len)
        return;

    state = krnP4LockTake(&cache_lock);
    wb(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
       (uint32_t)(unsigned long)addr, (uint32_t)len);
    krnP4LockRelease(&cache_lock, state);
    asm volatile("fence rw, rw" ::: "memory");
}

/*
 * Drop a data range's lines without writing them back, for a buffer or
 * descriptor that a bus master has just written (the SD controller's
 * IDMAC): the CPU must read it from memory next. Same ROM routine and
 * map as the write-back above, so the lines leave both the L1 data cache
 * and the L2.
 */
P4_SRAMCODE void krnP4CacheInvalidateData(void *addr, unsigned long len)
{
    rom_cache_range_t inv = (rom_cache_range_t)P4_ROM_CACHE_INVALIDATE_ADDR;
    unsigned long state;

    if (!len)
        return;

    state = krnP4LockTake(&cache_lock);
    inv(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
        (uint32_t)(unsigned long)addr, (uint32_t)len);
    krnP4LockRelease(&cache_lock, state);
    asm volatile("fence rw, rw" ::: "memory");
}

/*
 * Force a data range through external memory before reading it again.
 *
 * The B5 concurrent stress writes a PSRAM scratch range while the display
 * DMA is consuming a different PSRAM range.  A CPU read immediately after a
 * write can otherwise be satisfied entirely from cache and prove nothing
 * about the shared memory path.  Write back first, then invalidate only the
 * data hierarchy for this range so the verification load must reach memory.
 */
P4_SRAMCODE void krnP4CacheSyncData(void *addr, unsigned long len)
{
    rom_cache_range_t wb = (rom_cache_range_t)P4_ROM_CACHE_WRITEBACK_ADDR;
    rom_cache_range_t inv = (rom_cache_range_t)P4_ROM_CACHE_INVALIDATE_ADDR;
    unsigned long state;

    if (!len)
        return;

    state = krnP4LockTake(&cache_lock);
    wb(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
       (uint32_t)(unsigned long)addr, (uint32_t)len);
    inv(P4_CACHE_MAP_L1_DCACHE | P4_CACHE_MAP_L2,
        (uint32_t)(unsigned long)addr, (uint32_t)len);
    krnP4LockRelease(&cache_lock, state);
    asm volatile("fence rw, rw" ::: "memory");
}

P4_SRAMCODE unsigned long krnP4CacheOff(void)
{
    rom_cache_all_t wb_all = (rom_cache_all_t)P4_ROM_CACHE_WRITEBACK_ALL;
    rom_cache_suspend_t suspend = (rom_cache_suspend_t)P4_ROM_CACHE_SUSPEND_L2;
    unsigned long predictor;

    asm volatile("csrr %0, %1" : "=r"(predictor) : "i"(P4_MHCR) : "memory");
    asm volatile("csrc %0, %1" :: "i"(P4_MHCR), "r"(P4_MHCR_PREDICTOR));

    /*
     * The internal memory L1 data cache first, and only that one. PSRAM is
     * live, and its lines are held in the L2 that is about to stop
     * answering; an auto-writeback landing in that gap is the documented
     * way this hangs.
     */
    wb_all(P4_CACHE_MAP_L1_DCACHE);

    return (suspend() & P4_CACHE_AUTOLOAD) |
           (predictor & P4_MHCR_PREDICTOR);
}

P4_SRAMCODE void krnP4CacheOn(unsigned long token)
{
    rom_cache_resume_t resume = (rom_cache_resume_t)P4_ROM_CACHE_RESUME_L2;

    resume((uint32_t)(token & P4_CACHE_AUTOLOAD));

    asm volatile("csrs %0, %1" ::
                 "i"(P4_MHCR), "r"(token & P4_MHCR_PREDICTOR));
}

/*
 * KATTR_CacheOps: what drivers in package modules use instead of calling
 * the ROM themselves, so that their maintenance and the kernel's go
 * through the same lock.
 */
static void cache_ops_writeback(APTR address, ULONG length)
{
    krnP4CacheWritebackData(address, length);
}

static void cache_ops_invalidate(APTR address, ULONG length)
{
    krnP4CacheInvalidateData(address, length);
}

const struct KrnCacheOps __p4_cache_ops =
{
    KRN_CACHE_OPS_VERSION,
    cache_ops_writeback,
    cache_ops_invalidate
};
