/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    A read-only RAM block device that serves deliberately malformed partition
    tables, so bounded discovery can be tested without writing to any medium.

    The unit number selects the corpus case.  That keeps the whole thing
    inside the existing device API: no extra call is needed to tell the device
    which table to serve, and a test can simply open the unit it wants.
*/

#ifndef RAMTEST_DEVICE_H
#define RAMTEST_DEVICE_H

#include <exec/devices.h>
#include <exec/io.h>
#include <exec/libraries.h>

/* Every case is described by a sparse list of sectors.  A table is mostly
   zeroes, so storing only the interesting sectors keeps this small enough to
   sit in the package without a second thought. */
struct RamTestSector
{
    ULONG   lba;
    const UBYTE *data;      /* 512 bytes */
};

struct RamTestCase
{
    const char *name;
    ULONG   total_sectors;      /* what TD_GETGEOMETRY reports */
    ULONG   readable_sectors;   /* reads at or beyond this fail: truncation */
    const struct RamTestSector *sectors;
    UWORD   sector_count;
    /*
     * What the test may accept.  Two fields rather than one flag, because
     * "rejected" is not always the right expectation: a protective MBR in
     * front of a broken GPT is a legitimate MBR, and falling back to it is
     * correct behaviour, not a failure.  So a case states how many partitions
     * may appear at the top level, and which table type must never be
     * accepted for it.
     */
    UWORD   max_partitions;
    UWORD   forbidden_type;     /* 0 = none forbidden, else PHPTT_* */
};

extern const struct RamTestCase RamTestCases[];
extern const UWORD RamTestCaseCount;

struct RamTestBase
{
    struct Device device;
    struct ExecBase *sysBase;
};

#endif /* RAMTEST_DEVICE_H */
