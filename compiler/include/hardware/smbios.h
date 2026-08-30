#ifndef HARDWARE_SMBIOS_H
#define HARDWARE_SMBIOS_H

#include <exec/types.h>

struct SMBIOSHeader
{
    UBYTE sm_Type;
    UBYTE sm_Length;
    UWORD sm_Handle;
};

struct SMBIOSEntryPoint2
{
    UBYTE anchor[4];
    UBYTE checksum;
    UBYTE length;
    UBYTE major;
    UBYTE minor;
    UWORD max_structure_size;
    UBYTE entry_point_revision;
    UBYTE formatted_area[5];
    UBYTE intermediate_anchor[5];
    UBYTE intermediate_checksum;
    UWORD table_length;
    ULONG table_address;
    UWORD number_of_structures;
    UBYTE bcd_revision;
};

struct SMBIOSEntryPoint3
{
    UBYTE anchor[5];
    UBYTE checksum;
    UBYTE length;
    UBYTE major;
    UBYTE minor;
    UBYTE docrev;
    UBYTE entry_point_revision;
    UBYTE reserved;
    ULONG table_length;
    UQUAD table_address;
};

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
