/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Read-only block device over the flash development volume.

    Why it exists.  Every change to a test file on the MicroSD card costs a
    card handoff: out of the board, into the host, written, verified, back.
    A5 took four of those and Track B will iterate harder.  A volume in flash
    is written with esptool in a second and read from the board without
    touching anything mechanical.

    Why it is in the kickstart.  Package members are relocated by
    kernel_elf.c, whose symbol resolution fails on an undefined symbol, so a
    package module cannot call krnP4FlashMap(); it can only reach the outside
    world through library vectors.  A device that reads flash therefore has
    to be linked where that function is.

    Read-only by construction, like the SD backend and for the same reason.
    There is no code below the command switch that writes anything, and
    TD_PROTSTATUS says so, so a filesystem above it can refuse mutations
    before they dirty a cache.

    One property of the underlying map deserves stating: krnP4FlashMap() owns
    a single scratch window and is not re-entrant.  Two callers interleaving
    would each see the other's mapping.  Every use here is inside Forbid(),
    which is enough because the map and the copy out of it are a few hundred
    cycles and no request waits on anything.
*/

#define DEBUG 0
#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/expansion.h>

#include <string.h>

#include <exec/types.h>
#include <exec/exec.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/newstyle.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>

#include "kernel_intern.h"

#include LC_LIBDEFS_FILE

#define FLASHDISK_SECTOR_SIZE   512

/*
   Mapping costs a cache invalidate over the pages it covers, so a request is
   served in bounded pieces rather than mapped whole.  64 KB is one MMU page
   on this SoC, and no caller asks for more anyway: partition.library reads a
   sector at a time and FAT's cache reads thirty-two.
*/
#define FLASHDISK_CHUNK         0x10000UL

struct FlashDiskUnit
{
    struct Unit unit;
    ULONG       reads;      /* requests served since the unit was opened */
    ULONG       sectors;    /* sectors served, which is the real measure */
};

static struct FlashDiskUnit FlashDiskUnit0;

/* Where the volume is, and how big.  Set by the kickstart once the arosbsp
   partition has been located; zero means it never was, and then this device
   refuses to open rather than serving whatever happens to be at offset
   zero. */
extern unsigned long __esp32p4_flashdisk_base;

static ULONG FlashDiskSectors(void)
{
    return (ULONG)(P4_FLASHDISK_SIZE / FLASHDISK_SECTOR_SIZE);
}

static void FlashDiskRead(struct IOStdReq *io)
{
    UBYTE *dest = io->io_Data;
    ULONG length = io->io_Length;
    UQUAD offset = ((UQUAD)io->io_Actual << 32) | (UQUAD)io->io_Offset;

    if (dest == NULL)
    {
        io->io_Error = IOERR_BADADDRESS;
        return;
    }
    if ((length % FLASHDISK_SECTOR_SIZE) != 0
        || (offset % FLASHDISK_SECTOR_SIZE) != 0)
    {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }
    /* Subtraction form, so a length from an untrusted caller cannot wrap the
       addition and land inside the volume by accident. */
    if (offset >= P4_FLASHDISK_SIZE
        || length > P4_FLASHDISK_SIZE - offset)
    {
        io->io_Error = IOERR_BADADDRESS;
        return;
    }

    io->io_Actual = 0;

    while (length)
    {
        ULONG piece = length > FLASHDISK_CHUNK ? FLASHDISK_CHUNK : length;
        const void *src;

        /*
         * The map and the copy are one critical section: the window is
         * shared, so a task switch between them would copy out of somebody
         * else's mapping.
         */
        Forbid();
        src = krnP4FlashMap(__esp32p4_flashdisk_base
                                + (unsigned long)offset, piece);
        if (src != NULL)
            CopyMem((APTR)src, dest, piece);
        Permit();

        if (src == NULL)
        {
            /* The only way this fails for a range already checked against
               the volume is the 16 MB cache-mapping limit, which the volume
               is placed to stay below.  Reported rather than assumed away. */
            io->io_Error = IOERR_BADADDRESS;
            return;
        }

        dest += piece;
        offset += piece;
        length -= piece;
        io->io_Actual += piece;
    }

    io->io_Error = 0;
}

static void FlashDiskGeometry(struct IOStdReq *io)
{
    struct DriveGeometry *dg = io->io_Data;

    if (io->io_Length < sizeof(struct DriveGeometry))
    {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    memset(dg, 0, sizeof(struct DriveGeometry));
    dg->dg_SectorSize   = FLASHDISK_SECTOR_SIZE;
    dg->dg_TotalSectors = FlashDiskSectors();
    dg->dg_Cylinders    = FlashDiskSectors();
    dg->dg_CylSectors   = 1;
    dg->dg_Heads        = 1;
    dg->dg_TrackSectors = 1;
    dg->dg_BufMemType   = MEMF_PUBLIC;
    dg->dg_DeviceType   = DG_DIRECT_ACCESS;
    /* Not removable: it is soldered to the board. */
    dg->dg_Flags        = 0;

    io->io_Actual = sizeof(struct DriveGeometry);
    io->io_Error  = 0;
}

static const UWORD FlashDiskSupported[] =
{
    CMD_RESET, CMD_READ, CMD_UPDATE, CMD_CLEAR,
    TD_MOTOR, TD_CHANGENUM, TD_CHANGESTATE, TD_PROTSTATUS,
    TD_GETGEOMETRY, TD_READ64, NSCMD_TD_READ64,
    0
};

static void FlashDiskQuery(struct IOStdReq *io)
{
    struct NSDeviceQueryResult *d = io->io_Data;

    if (io->io_Length < (LONG)sizeof(struct NSDeviceQueryResult))
    {
        io->io_Error = IOERR_BADLENGTH;
        return;
    }

    d->DevQueryFormat    = 0;
    d->SizeAvailable     = sizeof(struct NSDeviceQueryResult);
    d->DeviceType        = NSDEVTYPE_TRACKDISK;
    d->DeviceSubType     = 0;
    d->SupportedCommands = (UWORD *)FlashDiskSupported;

    io->io_Actual = sizeof(struct NSDeviceQueryResult);
    io->io_Error  = 0;
}

AROS_LH1(void, BeginIO,
    AROS_LHA(struct IORequest *, io, A1),
    LIBBASETYPEPTR, LIBBASE, 5, FlashDisk)
{
    AROS_LIBFUNC_INIT

    struct FlashDiskUnit *unit = (struct FlashDiskUnit *)io->io_Unit;
    struct IOStdReq *std = (struct IOStdReq *)io;

    io->io_Error = 0;

    if (unit == NULL || __esp32p4_flashdisk_base == 0)
    {
        io->io_Error = IOERR_OPENFAIL;
        goto done;
    }

    switch (io->io_Command)
    {
    case CMD_READ:
        /* A 32-bit read: the high half of the offset is not a parameter. */
        std->io_Actual = 0;
        unit->reads++;
        unit->sectors += std->io_Length / FLASHDISK_SECTOR_SIZE;
        FlashDiskRead(std);
        break;

    case TD_READ64:
    case NSCMD_TD_READ64:
        unit->reads++;
        unit->sectors += std->io_Length / FLASHDISK_SECTOR_SIZE;
        FlashDiskRead(std);
        break;

    case TD_GETGEOMETRY:
        FlashDiskGeometry(std);
        break;

    case NSCMD_DEVICEQUERY:
        FlashDiskQuery(std);
        break;

    case TD_PROTSTATUS:
        /* Non-zero means write protected, and it is: there is no write path
           in this file.  A filesystem reads this to decide whether to refuse
           mutations before dirtying anything. */
        std->io_Actual = -1;
        break;

    case TD_CHANGENUM:
        /* Soldered down, so the medium has never changed. */
        std->io_Actual = 1;
        break;

    case TD_CHANGESTATE:
        /* Zero means a medium is present, and one always is. */
        std->io_Actual = 0;
        break;

    case CMD_RESET:
    case CMD_UPDATE:
    case CMD_CLEAR:
    case TD_MOTOR:
        std->io_Actual = 0;
        break;

    /*
     * Every write, named rather than left to the default.  IOERR_NOCMD is
     * what an unimplemented command deserves and it is not what this is: a
     * filesystem can act on TDERR_WriteProt and can only guess at NOCMD, and
     * TD_PROTSTATUS above already says the medium is protected, so the two
     * now agree.  io_Actual is zeroed because "nothing was written" has to be
     * readable from the reply without knowing what the caller left in it.
     * This is the same correction sdcard.device needed for A4.
     */
    case CMD_WRITE:
    case TD_WRITE64:
    case NSCMD_TD_WRITE64:
    case TD_FORMAT:
    case TD_FORMAT64:
    case NSCMD_TD_FORMAT64:
    case TD_RAWWRITE:
        std->io_Actual = 0;
        io->io_Error = TDERR_WriteProt;
        break;

    /* Everything else is simply not implemented.  There is no code below this
       switch that could write anything. */
    default:
        io->io_Error = IOERR_NOCMD;
        break;
    }

done:
    if (!(io->io_Flags & IOF_QUICK))
        ReplyMsg(&io->io_Message);

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, AbortIO,
    AROS_LHA(struct IORequest *, io, A1),
    LIBBASETYPEPTR, LIBBASE, 6, FlashDisk)
{
    AROS_LIBFUNC_INIT

    /* Every request completes inside BeginIO, so there is never one to
       abort. */
    return 0;

    AROS_LIBFUNC_EXIT
}

#if !defined(P4_NO_FLASHDISK_BOOTNODE)
/*
 * The whole-disk boot node, so dosboot finds the volume.
 *
 * Nothing here parses the partition table.  dosboot_BootScan() takes every
 * boot node whose DeviceNode has no handler and no seglist, runs
 * partition.library over it, and replaces it with one node per partition;
 * for this device that yields FLASHDISK0P0, named from the device name, the
 * unit and the partition index in rom/dosboot/bootscan.c.  The card's node
 * is made the same way by sdcard.device and becomes SDCARD0P0.
 *
 * The priority is the whole point of this function.  Both partition nodes
 * end up at DE_BOOTPRI 0, because partition.library copies the root's
 * DosEnvec into each partition's and nothing in the MBR path sets a boot
 * priority.  Enqueue() is FIFO among equals, so what decides which one dos
 * boots is the order in which their parent disks were scanned, and that
 * order is the MountList order of the whole-disk nodes, which is by
 * priority.  A negative priority here therefore puts the card ahead of the
 * flash without touching any shared code: a present card is scanned first,
 * its partition is enqueued first, and dosboot_Init() promotes the head of
 * the list to 127.  With no card, sdcard.device registers no unit at all and
 * the flash volume is the only candidate left.
 *
 * bootdevice=FLASHDISK0P0 on the kernel command line overrides all of this;
 * that mechanism is dosboot's own and needed nothing added for it.
 */
#if !defined(P4_FLASHDISK_BOOTPRI)
#define P4_FLASHDISK_BOOTPRI    (-10)
#endif

static const TEXT fd_UnitName[] = "fd0";

static void FlashDiskAddBootNode(void)
{
    IPTR pp[4 + DE_BOOTBLOCKS + 1] = { 0 };
    struct DeviceNode *devnode;

    if ((ExpansionBase = TaggedOpenLibrary(TAGGEDOPEN_EXPANSION)) == NULL)
    {
        bug("[FlashDisk] expansion.library did not open; no boot node\n");
        return;
    }

    pp[0]                       = (IPTR)fd_UnitName;
    pp[1]                       = (IPTR)MOD_NAME_STRING;
    pp[2]                       = 0;                    /* unit */
    pp[3]                       = 0;                    /* flags */

    /*
     * One sector per cylinder, which is flat LBA.  The geometry this device
     * reports has no heads or tracks to speak of, and pretending otherwise
     * would only give initPartitionHandle() a boundary to trip over.
     */
    pp[DE_TABLESIZE    + 4]     = DE_BOOTBLOCKS;
    pp[DE_SIZEBLOCK    + 4]     = FLASHDISK_SECTOR_SIZE / 4;
    pp[DE_NUMHEADS     + 4]     = 1;
    pp[DE_SECSPERBLOCK + 4]     = 1;
    pp[DE_BLKSPERTRACK + 4]     = 1;
    pp[DE_RESERVEDBLKS + 4]     = 2;
    pp[DE_LOWCYL       + 4]     = 0;
    pp[DE_HIGHCYL      + 4]     = FlashDiskSectors() - 1;
    pp[DE_NUMBUFFERS   + 4]     = 10;

    /*
     * These three are inherited by every non-RDB partition found below this
     * node, in AddPartitionVolume(), because an MBR carries no equivalent.
     * MEMF_PUBLIC without MEMF_31BIT: reads land wherever the caller asked,
     * PSRAM at 0x48000000 included, because CopyMem() out of the mapped
     * window has no addressing limit of its own.
     */
    pp[DE_BUFMEMTYPE   + 4]     = MEMF_PUBLIC;
    pp[DE_MAXTRANSFER  + 4]     = 0x00200000;
    pp[DE_MASK         + 4]     = 0x7FFFFFFE;

    pp[DE_BOOTPRI      + 4]     = P4_FLASHDISK_BOOTPRI;
    pp[DE_DOSTYPE      + 4]     = 0;
    pp[DE_CONTROL      + 4]     = 0;
    pp[DE_BOOTBLOCKS   + 4]     = 2;

    devnode = MakeDosNode(pp);
    if (devnode == NULL)
    {
        bug("[FlashDisk] MakeDosNode() failed; no boot node\n");
        return;
    }

    if (AddBootNode(P4_FLASHDISK_BOOTPRI, 0, devnode, NULL))
        bug("[FlashDisk] boot node %s at priority %ld, %lu sectors\n",
            fd_UnitName, (long)P4_FLASHDISK_BOOTPRI,
            (unsigned long)FlashDiskSectors());
    else
        bug("[FlashDisk] AddBootNode() refused %s\n", fd_UnitName);
}
#endif /* !P4_NO_FLASHDISK_BOOTNODE */

static int FlashDiskInit(LIBBASETYPEPTR LIBBASE)
{
    NEWLIST(&FlashDiskUnit0.unit.unit_MsgPort.mp_MsgList);
    FlashDiskUnit0.unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
    FlashDiskUnit0.unit.unit_MsgPort.mp_Flags        = PA_IGNORE;
    FlashDiskUnit0.unit.unit_OpenCnt                 = 0;
    FlashDiskUnit0.reads                             = 0;
    FlashDiskUnit0.sectors                           = 0;

    if (__esp32p4_flashdisk_base == 0)
        bug("[FlashDisk] no arosbsp partition was found;"
            " the device will refuse to open\n");
    else
    {
        D(bug("[FlashDisk] volume at flash 0x%08lx, %lu sectors\n",
              __esp32p4_flashdisk_base, (unsigned long)FlashDiskSectors()));
#if !defined(P4_NO_FLASHDISK_BOOTNODE)
        FlashDiskAddBootNode();
#endif
    }

    return TRUE;
}

static int FlashDiskOpen(LIBBASETYPEPTR LIBBASE, struct IORequest *iorq,
                         ULONG unitnum, ULONG flags)
{
    if (unitnum != 0 || __esp32p4_flashdisk_base == 0)
    {
        iorq->io_Error = IOERR_OPENFAIL;
        return FALSE;
    }

    iorq->io_Unit = (struct Unit *)&FlashDiskUnit0;
    FlashDiskUnit0.unit.unit_OpenCnt++;
    iorq->io_Error = 0;
    return TRUE;
}

static int FlashDiskClose(LIBBASETYPEPTR LIBBASE, struct IORequest *iorq)
{
    struct FlashDiskUnit *unit = (struct FlashDiskUnit *)iorq->io_Unit;

    iorq->io_Unit = (struct Unit *)~0;
    if (unit != NULL && unit != (struct FlashDiskUnit *)~0)
        unit->unit.unit_OpenCnt--;
    return TRUE;
}

ADD2INITLIB(FlashDiskInit, 0)
ADD2OPENDEV(FlashDiskOpen, 0)
ADD2CLOSEDEV(FlashDiskClose, 0)
