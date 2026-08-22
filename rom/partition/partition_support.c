/*
    Copyright (C) 1995-2011, The AROS Development Team. All rights reserved.

*/

#include <proto/exec.h>
#include <devices/newstyle.h>
#include <utility/tagitem.h>

#include "partition_intern.h"
#include "partition_support.h"
#include "debug.h"

const struct PTFunctionTable * const PartitionSupport[] =
{
    &PartitionGPT, /* The order of these two is important, GPT must be checked before MBR */
    &PartitionMBR,
    &PartitionEBR,
    &PartitionRDB,
    NULL
};

/* Keep RDB last. RDB scans up to 16 blocks and in case of
 * MBR->EBR->RDB having RDB first detects RDB as root and
 * causes MBR and EBR offsets not be taken into account
 */

/* get geometry */
LONG PartitionGetGeometry
   (
      struct Library *PartitionBase,
      struct IOExtTD *ioreq,
      struct DriveGeometry *dg
   )
{

   ioreq->iotd_Req.io_Command = TD_GETGEOMETRY;
   ioreq->iotd_Req.io_Data = dg;
   ioreq->iotd_Req.io_Length = sizeof(struct DriveGeometry);
   return DoIO((struct IORequest *)ioreq);
}

/* query NSD commands */
void PartitionNsdCheck
    (
        struct Library *PartitionBase,
        struct PartitionHandle *root
    )
{
struct NSDeviceQueryResult nsdq;
struct IOExtTD *ioreq = root->bd->ioreq;
UWORD *cmdcheck;

   if (
         (
                root->de.de_HighCyl*
                root->de.de_Surfaces*
                root->de.de_BlocksPerTrack*
                ((root->de.de_SizeBlock<<2)/512)
         )>8388608)
   {
      nsdq.SizeAvailable=0;
      nsdq.DevQueryFormat=0;
      ioreq->iotd_Req.io_Command=NSCMD_DEVICEQUERY;
      ioreq->iotd_Req.io_Data=&nsdq;
      ioreq->iotd_Req.io_Length=sizeof(struct NSDeviceQueryResult);
      if (DoIO((struct IORequest *)ioreq)==0)
      {
         if (
               (ioreq->iotd_Req.io_Actual<=sizeof(struct NSDeviceQueryResult)) &&
               (ioreq->iotd_Req.io_Actual!=0) &&
               (ioreq->iotd_Req.io_Actual==nsdq.SizeAvailable)
            )
         {
            if (nsdq.DeviceType != NSDEVTYPE_TRACKDISK)
               D(bug("partition: NSDcheck: WARNING no trackdisk type\n"));
            for (cmdcheck=nsdq.SupportedCommands;*cmdcheck;cmdcheck++)
            {
               if (*cmdcheck == NSCMD_TD_READ64)
                  root->bd->cmdread = NSCMD_TD_READ64;
               if (*cmdcheck == NSCMD_TD_WRITE64)
                  root->bd->cmdwrite = NSCMD_TD_WRITE64;
            }
         }
            else
            D(bug("partition: NSDcheck: WARNING wrong io_Actual using NSD\n"));
      }
   }
}

/* get real first block of partition ph */
ULONG getStartBlock(struct PartitionHandle *ph) {
ULONG start = 0;

    while (ph)
    {
        start += ph->de.de_LowCyl*ph->de.de_BlocksPerTrack*ph->de.de_Surfaces;
        ph = ph->root;
    }
    return start;
}

LONG deviceError(LONG err)
{
    switch (err)
    {
    case 0:
        return 0;

    case TDERR_WriteProt:
        return ERROR_DISK_WRITE_PROTECTED;

    case TDERR_SeekError:
        return ERROR_SEEK_ERROR;
    
    default:
        return ERROR_NOT_A_DOS_DISK;
    }
}

/*
 * Initialize partition handle based on parent's data.
 * Geometry will be inherited from parent and adjusted if needed
 * for given start block and length in blocks to fit in.
 */
/*
 * How many sectors the parent actually has.  A root handle gets its geometry
 * from TD_GETGEOMETRY, where dg_TotalSectors is authoritative; a handle made
 * by initPartitionHandle() below only has cylinders times cylinder sectors.
 * Take whichever is larger so a valid table is never rejected over a
 * rounding difference, and return 0 when the geometry says nothing at all.
 */
static UQUAD partitionExtent(struct PartitionHandle *root)
{
    UQUAD byCylinders = (UQUAD)root->dg.dg_Cylinders * root->dg.dg_CylSectors;
    UQUAD byTotal     = (UQUAD)root->dg.dg_TotalSectors;

    return (byTotal > byCylinders) ? byTotal : byCylinders;
}

/*
 * Reject a partition range that cannot exist inside its parent, before any
 * of it is used to derive a geometry or to read.  A table is attacker-supplied
 * data: every entry has to be treated as three independent 32-bit values that
 * happen to sit next to each other, not as a description that can be trusted
 * to be self-consistent.
 *
 * The sum is computed in 64 bits precisely so that a start plus a count which
 * wraps in 32 bits is caught rather than folded into a small, plausible
 * number.  A zero count is refused too: it produces a zero-cylinder handle
 * and a de_HighCyl that underflows below de_LowCyl.
 */
BOOL partitionRangeIsSane(struct PartitionHandle *root, ULONG first_sector, ULONG count_sector)
{
    UQUAD extent = partitionExtent(root);
    UQUAD end    = (UQUAD)first_sector + (UQUAD)count_sector;

    if (count_sector == 0)
        return FALSE;

    /* Unknown parent geometry: the overflow check above is all there is. */
    if (extent == 0)
        return TRUE;

    if (end > extent)
        return FALSE;

    return TRUE;
}

void initPartitionHandle(struct PartitionHandle *root, struct PartitionHandle *ph, ULONG first_sector, ULONG count_sector)
{
    ULONG cylsecs = root->de.de_BlocksPerTrack * root->de.de_Surfaces;

    /* Attach parent */
    ph->root = root;
    ph->bd   = root->bd;

    /* initialize DosEnvec */
    CopyMem(&root->de, &ph->de, sizeof(struct DosEnvec));

    /* Check if partition starts and ends on a cylinder boundary */
    if ((first_sector % cylsecs != 0) || (count_sector % cylsecs != 0))
    {
        /* Treat each track as a cylinder if possible */
        ph->de.de_Surfaces = 1;
        cylsecs = ph->de.de_BlocksPerTrack;

        if ((first_sector % cylsecs != 0) || (count_sector % cylsecs != 0))
        {
            /*
             * We can't. We could find the highest common factor of
             * first_sector and count_sector here, but currently we
             * simply use one block per cylinder (flat LBA)
             */
            ph->de.de_BlocksPerTrack = 1;
            cylsecs = 1;
        }
    }

    /* initialize DriveGeometry */
    ph->dg.dg_DeviceType   = DG_DIRECT_ACCESS;
    ph->dg.dg_SectorSize   = ph->de.de_SizeBlock<<2;
    ph->dg.dg_Heads        = ph->de.de_Surfaces;
    ph->dg.dg_TrackSectors = ph->de.de_BlocksPerTrack;
    ph->dg.dg_Cylinders    = count_sector / cylsecs;
    ph->dg.dg_CylSectors   = cylsecs;
    ph->dg.dg_BufMemType   = ph->de.de_BufMemType;

    /* Set start/end cylinder in DosEnvec */
    ph->de.de_LowCyl    = first_sector / cylsecs;
    ph->de.de_HighCyl   = ph->de.de_LowCyl + ph->dg.dg_Cylinders - 1;

    /* Fix up DosEnvec size if necessary */
    if (ph->de.de_TableSize < DE_BUFMEMTYPE)
        ph->de.de_TableSize = DE_BUFMEMTYPE;
}

/* Set DOSType and some defaults according to it */
void setDosType(struct DosEnvec *de, ULONG type)
{
    de->de_TableSize      = DE_DOSTYPE;
    de->de_SectorPerBlock = 1;
    de->de_DosType        = type;
    /*
     * These two are actually device-dependent. However there's no way
     * to obtain them, so just fill in defaults.
     */
    de->de_MaxTransfer    = 0x00200000;
    de->de_Mask           = 0x7ffffffe;

    /* Some more filesystem-specific defaults */
    switch (type)
    {
    case ID_DOS_DISK:
    case ID_FFS_DISK:
    case ID_INTER_FFS_DISK:
    case ID_SFS_BE_DISK:
        de->de_TableSize      = DE_BOOTBLOCKS;
        de->de_Reserved       = 2;
        de->de_BootBlocks     = 2;
    }
}
