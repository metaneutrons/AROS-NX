/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: GENETv5 SANA-II driver, initialisation.
*/

#include <aros/debug.h>

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/utility.h>

#include <devices/sana2.h>

#include LC_LIBDEFS_FILE

#include "genet.h"

#undef UtilityBase

/*
 * Ask the PHY who it is. A bus with nothing on it reads back all ones
 * or all zeroes, which is how an empty address is told from a real one.
 */
static BOOL GENET_ProbePHY(struct genet_hw *hw)
{
    LONG id1, id2;
    ULONG phy = hw->phyAddr;

    id1 = GENET_MDIORead(hw, phy, MII_PHYSID1);
    id2 = GENET_MDIORead(hw, phy, MII_PHYSID2);

    if (id1 < 0 || id2 < 0)
        return FALSE;

    if ((id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0))
    {
        D(bug("[genet] nothing answering at mdio address %u\n", phy);)
        return FALSE;
    }

    D(bug("[genet] phy %u: id %04x%04x, bmsr %04x\n", phy,
          (UWORD)id1, (UWORD)id2,
          (UWORD)GENET_MDIORead(hw, phy, MII_BMSR));)

    return TRUE;
}

static int GENET_Init(LIBBASETYPEPTR LIBBASE)
{
    struct genet_hw *hw = &LIBBASE->gnb_HW;
    ULONG rev, major, minor;

    LIBBASE->gnb_Found = FALSE;
    LIBBASE->gnb_Unit = NULL;
    InitSemaphore(&LIBBASE->gnb_UnitSem);

    LIBBASE->gnb_KernelBase = OpenResource("kernel.resource");
    if (!LIBBASE->gnb_KernelBase)
    {
        D(bug("[genet] no kernel.resource\n");)
        return FALSE;
    }

    LIBBASE->gnb_UtilityBase = TaggedOpenLibrary(TAGGEDOPEN_UTILITY);
    if (!LIBBASE->gnb_UtilityBase)
    {
        D(bug("[genet] no utility.library\n");)
        return FALSE;
    }

    if (!GENET_Discover(LIBBASE, hw))
        return FALSE;

    /*
     * The revision register is the first thing worth reading: it says
     * the mapping reached the block at all, and which generation of
     * register map is behind it.
     */
    rev = GENET_Read(hw, GENET_SYS_REV_CTRL);

    if (rev == 0 || rev == 0xffffffff)
    {
        D(bug("[genet] register block reads %08x - not there\n", rev);)
        return FALSE;
    }

    major = (rev & GENET_REV_MAJOR) >> GENET_REV_MAJOR_SHIFT;
    minor = (rev & GENET_REV_MINOR) >> GENET_REV_MINOR_SHIFT;

    /*
     * Only the v5 map is here. The earlier generations moved registers
     * around rather than merely adding to them, so an older core needs a
     * second register map, not a version test that lets it through.
     */
    if (major != GENET_REV_MAJOR_V5)
    {
        D(bug("[genet] core reports major %u, this drives v5 only\n",
              major);)
        return FALSE;
    }

    D(bug("[genet] GENET v5.%u, phy revision %04x\n", minor,
          rev & GENET_REV_PHY);)

    if (!GENET_ProbePHY(hw))
        return FALSE;

    LIBBASE->gnb_Found = TRUE;

    return TRUE;
}

static int GENET_Expunge(LIBBASETYPEPTR LIBBASE)
{
    if (LIBBASE->gnb_Unit)
    {
        GENET_DeleteUnit(LIBBASE, LIBBASE->gnb_Unit);
        LIBBASE->gnb_Unit = NULL;
    }

    if (LIBBASE->gnb_UtilityBase)
    {
        CloseLibrary(LIBBASE->gnb_UtilityBase);
        LIBBASE->gnb_UtilityBase = NULL;
    }

    return TRUE;
}

AROS_LH1(void, GENET_BeginIO,
    AROS_LHA(struct IOSana2Req *, req, A1),
    LIBBASETYPEPTR, LIBBASE, 5, GENET)
{
    AROS_LIBFUNC_INIT

    struct GENETUnit *unit = (APTR)req->ios2_Req.io_Unit;

    req->ios2_Req.io_Error = 0;

    if (!unit || !unit->gnu_InputPort)
    {
        req->ios2_Req.io_Error = IOERR_OPENFAIL;
        if (!(req->ios2_Req.io_Flags & IOF_QUICK))
            ReplyMsg(&req->ios2_Req.io_Message);
        return;
    }

    if (AttemptSemaphore(&unit->gnu_Lock))
        GENET_HandleRequest(LIBBASE, req);
    else
    {
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(unit->gnu_InputPort, &req->ios2_Req.io_Message);
    }

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, GENET_AbortIO,
    AROS_LHA(struct IOSana2Req *, req, A1),
    LIBBASETYPEPTR, LIBBASE, 6, GENET)
{
    AROS_LIBFUNC_INIT

    /*
     * Still NT_MESSAGE means still sitting on some queue; anything else
     * is either being processed right now or already replied, and in
     * neither case may it be unlinked here.
     */
    Disable();
    if (req->ios2_Req.io_Message.mn_Node.ln_Type == NT_MESSAGE &&
        (req->ios2_Req.io_Flags & IOF_QUICK) == 0)
    {
        Remove((struct Node *)req);
        req->ios2_Req.io_Error = IOERR_ABORTED;
        req->ios2_WireError = S2WERR_GENERIC_ERROR;
        ReplyMsg(&req->ios2_Req.io_Message);
    }
    Enable();

    return 0;

    AROS_LIBFUNC_EXIT
}

static const IPTR genet_rx_tags[] =
{
    S2_CopyToBuff,
    S2_CopyToBuff16
};

static const IPTR genet_tx_tags[] =
{
    S2_CopyFromBuff,
    S2_CopyFromBuff16,
    S2_CopyFromBuff32
};

static int GENET_Open(LIBBASETYPEPTR LIBBASE, struct IORequest *io,
                      ULONG unitnum, ULONG flags)
{
    struct Library *UtilityBase = LIBBASE->gnb_UtilityBase;
    struct IOSana2Req *req = (struct IOSana2Req *)io;
    struct GENETUnit *unit;
    struct Opener *opener;
    struct TagItem *tags;
    ULONG i;

    io->io_Error = IOERR_OPENFAIL;
    io->io_Unit = NULL;

    /* One controller, one unit */
    if (!LIBBASE->gnb_Found || unitnum != 0)
        return FALSE;

    if (io->io_Message.mn_Length < sizeof(struct IOSana2Req))
    {
        D(bug("[genet] open request is not a sana2 one\n");)
        return FALSE;
    }

    /* The hardware side comes up on first open and then stays up */
    ObtainSemaphore(&LIBBASE->gnb_UnitSem);
    if (!LIBBASE->gnb_Unit)
        LIBBASE->gnb_Unit = GENET_CreateUnit(LIBBASE);
    unit = LIBBASE->gnb_Unit;
    ReleaseSemaphore(&LIBBASE->gnb_UnitSem);
    if (!unit)
        return FALSE;

    opener = AllocMem(sizeof(struct Opener), MEMF_PUBLIC | MEMF_CLEAR);
    if (!opener)
        return FALSE;

    tags = req->ios2_BufferManagement;
    for (i = 0; i < sizeof(genet_rx_tags) / sizeof(IPTR); i++)
        opener->rx_function =
            (APTR)GetTagData(genet_rx_tags[i], (IPTR)opener->rx_function,
                             tags);
    for (i = 0; i < sizeof(genet_tx_tags) / sizeof(IPTR); i++)
        opener->tx_function =
            (APTR)GetTagData(genet_tx_tags[i], (IPTR)opener->tx_function,
                             tags);
    opener->filter_hook = (APTR)GetTagData(S2_PacketFilter, 0, tags);

    if (!opener->rx_function || !opener->tx_function)
    {
        D(bug("[genet] opener brought no copy functions\n");)
        FreeMem(opener, sizeof(struct Opener));
        return FALSE;
    }

    NEWLIST(&opener->read_port.mp_MsgList);
    opener->read_port.mp_Flags = PA_IGNORE;
    NEWLIST(&opener->initial_stats);

    if (flags & SANA2OPF_PROM)
        unit->gnu_Flags |= IFF_PROMISC;

    req->ios2_BufferManagement = opener;
    io->io_Unit = (struct Unit *)unit;
    io->io_Error = 0;

    Disable();
    AddTail((struct List *)&unit->gnu_Openers, (struct Node *)opener);
    Enable();
    unit->gnu_OpenCount++;

    return TRUE;
}

static int GENET_Close(LIBBASETYPEPTR LIBBASE, struct IORequest *io)
{
    struct IOSana2Req *req = (struct IOSana2Req *)io;
    struct GENETUnit *unit = (APTR)io->io_Unit;
    struct Opener *opener = req->ios2_BufferManagement;
    struct Node *node;

    if (unit && opener)
    {
        Disable();
        Remove((struct Node *)opener);
        Enable();

        while ((node = RemHead((struct List *)&opener->initial_stats)))
            FreeMem(node, sizeof(struct TypeStats));

        FreeMem(opener, sizeof(struct Opener));
        req->ios2_BufferManagement = NULL;
        unit->gnu_OpenCount--;
    }

    io->io_Unit = (struct Unit *)-1;
    io->io_Device = (struct Device *)-1;

    return TRUE;
}

ADD2INITLIB(GENET_Init, 0)
ADD2EXPUNGELIB(GENET_Expunge, 0)
ADD2OPENDEV(GENET_Open, 0)
ADD2CLOSEDEV(GENET_Close, 0)
