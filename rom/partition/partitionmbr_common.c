/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: MBR/EBR helpers shared by partition.library and ptedit.library
*/

#include <exec/memory.h>
#include <exec/types.h>
#include <libraries/partition.h>
#include <proto/exec.h>

#include "partition_types.h"
#include "partition_support.h"
#include "partitionmbr.h"
#include "platform.h"
#include "debug.h"

ULONG MBRGetLE32(const void *p)
{
    const UBYTE *b = p;

    return b[0] | (b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

struct PartitionHandle *PartitionMBRNewHandle(struct Library *PartitionBase, struct PartitionHandle *root, UBYTE position, struct PCPartitionTable *entry)
{
    struct PartitionHandle *ph;

    if (entry->first_sector != 0)
    {
        /* Refuse an entry that cannot fit the disk before allocating for it.
           An MBR is four fixed entries, so the loop itself is bounded, but
           the three numbers inside each entry are not. */
        if (!partitionRangeIsSane(root, MBRGetLE32(&entry->first_sector),
                                        MBRGetLE32(&entry->count_sector)))
        {
            D(bug("[MBR] entry %u out of range: start %u count %u\n", position,
                  MBRGetLE32(&entry->first_sector),
                  MBRGetLE32(&entry->count_sector)));
            return NULL;
        }

        ph = AllocMem(sizeof(struct PartitionHandle), MEMF_PUBLIC | MEMF_CLEAR);
        if (ph)
        {
        struct MBRData *data;

            data = AllocMem(sizeof(struct MBRData), MEMF_PUBLIC);
            if (data)
            {
                data->entry    = entry;
                data->position = position;
                ph->data = data;

                /* initialize DosEnvec and DriveGeometry */
                initPartitionHandle(root, ph, MBRGetLE32(&data->entry->first_sector), MBRGetLE32(&data->entry->count_sector));

                /* Map type ID to a DOSType */
                setDosType(&ph->de, MBR_FindDosType(data->entry->type));

                /* Set position as priority */
                ph->ln.ln_Pri = MBR_MAX_PARTITIONS - 1 - position;
                return ph;
            }
            FreeMem(ph, sizeof(struct PartitionHandle));
        }
    }
    return NULL;
}

struct PartitionHandle *PartitionEBRNewHandle(struct Library *PartitionBase, struct PartitionHandle *root,
                                                     UBYTE type, ULONG block_no, ULONG block_count, ULONG ebr_block_no)
{
    struct PartitionHandle *ph;

    /* An EBR link is attacker-supplied like any other table entry, and the
       caller has already added block_no to a value read from the medium.
       Refuse a range outside the parent before allocating for it. */
    if (!partitionRangeIsSane(root, block_no, block_count))
    {
        D(bug("[EBR] logical partition out of range: start %u count %u\n",
              block_no, block_count));
        return NULL;
    }

    ph = AllocMem(sizeof(struct PartitionHandle), MEMF_PUBLIC | MEMF_CLEAR);
    if (ph != NULL)
    {
        struct EBRData *data;

        data = AllocMem(sizeof(struct EBRData), MEMF_PUBLIC);
        if (data != NULL)
        {
            data->type         = type;
            data->ebr_block_no = ebr_block_no;
            data->block_no     = block_no;
            data->block_count  = block_count;

            ph->data = data;

            /* Initialize DosEnvec and DriveGeometry */
            initPartitionHandle(root, ph, block_no, block_count);

            /* Map type ID to a DOSType */
            setDosType(&ph->de, MBR_FindDosType(data->type));

            return ph;
        }
        FreeMem(ph, sizeof(struct PartitionHandle));
    }
    return NULL;
}

