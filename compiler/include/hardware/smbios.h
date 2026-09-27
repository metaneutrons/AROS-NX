#ifndef HARDWARE_SMBIOS_H
#define HARDWARE_SMBIOS_H

/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SMBIOS (DMTF DSP0134) entry point and structure definitions.
*/

#include <exec/types.h>

/* Common structure header. Strings follow the formatted area, each
   NUL terminated, with the set terminated by an extra NUL. */
struct SMBIOSHeader
{
    UBYTE sm_Type;
    UBYTE sm_Length;
    UWORD sm_Handle;
} __attribute__((packed));

/* 32-bit ("_SM_") entry point, SMBIOS 2.x. Length 0x1F. */
struct SMBIOSEntryPoint2
{
    UBYTE anchor[4];                /* "_SM_" */
    UBYTE checksum;                 /* bytes [0, length) sum to zero */
    UBYTE length;
    UBYTE major;
    UBYTE minor;
    UWORD max_structure_size;
    UBYTE entry_point_revision;
    UBYTE formatted_area[5];
    UBYTE intermediate_anchor[5];   /* "_DMI_" */
    UBYTE intermediate_checksum;
    UWORD table_length;             /* exact length of the structure table */
    ULONG table_address;
    UWORD number_of_structures;
    UBYTE bcd_revision;
} __attribute__((packed));

/* 64-bit ("_SM3_") entry point, SMBIOS 3.x. Length 0x18. */
struct SMBIOSEntryPoint3
{
    UBYTE anchor[5];                /* "_SM3_" */
    UBYTE checksum;                 /* bytes [0, length) sum to zero */
    UBYTE length;
    UBYTE major;
    UBYTE minor;
    UBYTE docrev;
    UBYTE entry_point_revision;
    UBYTE reserved;
    ULONG table_length;             /* maximum length of the structure table */
    UQUAD table_address;
} __attribute__((packed));

/* Structure types used by AROS */
#define SMBIOS_TYPE_BIOS            0
#define SMBIOS_TYPE_SYSTEM          1
#define SMBIOS_TYPE_BASEBOARD       2
#define SMBIOS_TYPE_CHASSIS         3
#define SMBIOS_TYPE_PROCESSOR       4
#define SMBIOS_TYPE_IPMI            38
#define SMBIOS_TYPE_END             127

static inline BOOL SMBIOS_ChecksumValid(const UBYTE *entry, UBYTE length)
{
    UBYTE checksum = 0;
    UBYTE i;

    for (i = 0; i < length; i++)
        checksum += entry[i];

    return checksum == 0;
}

static inline BOOL SMBIOS_EntryPointValid(const UBYTE *entry, UBYTE version,
    const UBYTE *limit)
{
    UBYTE length;
    IPTR remaining = 0;

    if (limit)
    {
        if ((IPTR)entry >= (IPTR)limit)
            return FALSE;
        remaining = (IPTR)limit - (IPTR)entry;
        if (remaining < 7)
            return FALSE;
    }

    if (version == 3)
    {
        if (entry[0] != '_' || entry[1] != 'S' || entry[2] != 'M' ||
            entry[3] != '3' || entry[4] != '_')
            return FALSE;
        length = entry[6];
        if (length < 0x18 || length > 0x40)
            return FALSE;
    }
    else if (version == 2)
    {
        if ((limit && remaining < 0x1f) || entry[0] != '_' ||
            entry[1] != 'S' || entry[2] != 'M' || entry[3] != '_')
            return FALSE;
        length = entry[5];
        if (length < 0x1f || length > 0x40 || entry[0x10] != '_' ||
            entry[0x11] != 'D' || entry[0x12] != 'M' ||
            entry[0x13] != 'I' || entry[0x14] != '_' ||
            !SMBIOS_ChecksumValid(entry + 0x10, 0x0f))
            return FALSE;
    }
    else
        return FALSE;

    return (!limit || remaining >= length) &&
        SMBIOS_ChecksumValid(entry, length);
}

#endif /* HARDWARE_SMBIOS_H */
