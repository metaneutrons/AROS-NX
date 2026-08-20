/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: GENETv5 SANA-II driver, the unit: rings, interrupts and the
          task that serves deferred requests.

    The ring handling is ported from FreeBSD's if_genet.c; see genet.h for
    the copyright notice that travels with it.
*/

#include <aros/debug.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <devices/newstyle.h>
#include <aros/macros.h>

#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/utility.h>

#include <aros/kernel.h>

#include "genet.h"

#undef UtilityBase

static const UBYTE genet_broadcast[ETH_ADDRESSSIZE] =
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

#define GENET_QID           GENET_DMA_DEFAULT_QUEUE
#define GENET_RING_MASK     (GENET_DMA_DESC_COUNT - 1)

/*
 * How many transmit descriptors the block has not finished with. The
 * producer index is only ever written by the transmit soft interrupt and
 * the consumer index only ever by the hardware interrupt, so each side
 * reads the other's without a lock and the difference is always sound.
 */
#define GENET_TX_INFLIGHT(u) \
    ((((u)->gnu_TXProd - (u)->gnu_TXCons)) & GENET_DMA_PROD_CONS_MASK)

/* The descriptor area, in the 32-bit words the address registers count */
#define GENET_DESC_AREA_WORDS \
    (GENET_DMA_DESC_COUNT * GENET_DMA_DESC_SIZE / 4)

static void GENET_RXProcess(struct GENETUnit *unit);
static void GENET_TXComplete(struct GENETUnit *unit);
static void GENET_UnitTask(void);

/* The service task makes no DOS calls, so it can be a bare exec task */
#define GENET_TASK_STACK 32768

/* == cache maintenance for the packet buffers ============================ */

/*
 * The GENET sits behind the SoC's system bus and its reads and writes are
 * not snooped by the processor caches. Packet buffers therefore have to
 * be memory the processor does not cache either, which is arranged by
 * mapping them Normal Non-Cacheable (see genet_allocbuffers below).
 *
 * Changing a live mapping from cacheable to non-cacheable is not free:
 * lines already holding those addresses have to be cleaned, or a dirty
 * one can be written back later on top of data the controller put there,
 * and invalidated, or a stale clean one can shadow the new mapping.
 *
 * That maintenance is what CacheClearE() is for, and calling it is what
 * this ought to be. On aarch64 CacheClearE() is currently a stub that
 * does nothing, so the work is done here instead. When exec grows a real
 * implementation this helper should give way to it.
 */
static void genet_cacheflush(APTR addr, ULONG length)
{
#if defined(__aarch64__)
    IPTR p = (IPTR)addr & ~63UL;
    IPTR end = ((IPTR)addr + length + 63) & ~63UL;

    asm volatile("dsb sy" ::: "memory");
    for (; p < end; p += 64)
        asm volatile("dc civac, %0" :: "r"(p) : "memory");
    asm volatile("dsb sy" ::: "memory");
#else
    CacheClearE(addr, length, CACRF_ClearD);
#endif
}

/* == small helpers ====================================================== */

static BOOL genet_sameaddr(const UBYTE *a, const UBYTE *b)
{
    ULONG i;

    for (i = 0; i < ETH_ADDRESSSIZE; i++)
    {
        if (a[i] != b[i])
            return FALSE;
    }
    return TRUE;
}

/*
 * A 48-bit address split for range comparisons: the first four bytes as
 * one long, the last two as a word.
 */
static void genet_splitaddr(const UBYTE *addr, ULONG *left, UWORD *right)
{
    *left = ((ULONG)addr[0] << 24) | ((ULONG)addr[1] << 16) |
            ((ULONG)addr[2] << 8) | addr[3];
    *right = ((UWORD)addr[4] << 8) | addr[5];
}

struct TypeStats *GENET_FindTypeStats(struct MinList *list, ULONG packet_type)
{
    struct TypeStats *stats;

    ForeachNode(list, stats)
    {
        if (stats->packet_type == packet_type)
            return stats;
    }

    return NULL;
}

void GENET_ReportEvents(struct GENETBase *base, struct GENETUnit *unit,
                        ULONG events)
{
    struct IOSana2Req *request, *tail, *next;
    struct List *list;

    list = &unit->gnu_RequestPorts[EVENT_QUEUE]->mp_MsgList;
    next = (APTR)list->lh_Head;
    tail = (APTR)&list->lh_Tail;

    Disable();
    while (next != tail)
    {
        request = next;
        next = (APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ;

        if ((request->ios2_WireError & events) != 0)
        {
            request->ios2_WireError = events;
            Remove((APTR)request);
            ReplyMsg((APTR)request);
        }
    }
    Enable();
}

/* == address filtering ================================================== */

static struct AddressRange *genet_findrange(struct GENETUnit *unit,
    ULONG lower_left, UWORD lower_right, ULONG upper_left, UWORD upper_right)
{
    struct AddressRange *range;

    ForeachNode(&unit->gnu_MulticastRanges, range)
    {
        if (range->lower_bound_left == lower_left &&
            range->lower_bound_right == lower_right &&
            range->upper_bound_left == upper_left &&
            range->upper_bound_right == upper_right)
            return range;
    }

    return NULL;
}

BOOL GENET_AddMulticastRange(struct GENETBase *base, struct GENETUnit *unit,
                             const UBYTE *lower, const UBYTE *upper)
{
    struct AddressRange *range;
    ULONG lower_left, upper_left;
    UWORD lower_right, upper_right;

    genet_splitaddr(lower, &lower_left, &lower_right);
    genet_splitaddr(upper, &upper_left, &upper_right);

    range = genet_findrange(unit, lower_left, lower_right,
                            upper_left, upper_right);
    if (range)
    {
        range->add_count++;
        return TRUE;
    }

    range = AllocMem(sizeof(struct AddressRange), MEMF_PUBLIC | MEMF_CLEAR);
    if (!range)
        return FALSE;

    range->add_count = 1;
    range->lower_bound_left = lower_left;
    range->lower_bound_right = lower_right;
    range->upper_bound_left = upper_left;
    range->upper_bound_right = upper_right;

    Disable();
    AddTail((struct List *)&unit->gnu_MulticastRanges, (struct Node *)range);
    Enable();
    unit->gnu_RangeCount++;

    /* The hardware filter has to learn about it, or promiscuity does */
    GENET_SetupRXFilter(base, unit);

    return TRUE;
}

BOOL GENET_RemMulticastRange(struct GENETBase *base, struct GENETUnit *unit,
                             const UBYTE *lower, const UBYTE *upper)
{
    struct AddressRange *range;
    ULONG lower_left, upper_left;
    UWORD lower_right, upper_right;

    genet_splitaddr(lower, &lower_left, &lower_right);
    genet_splitaddr(upper, &upper_left, &upper_right);

    range = genet_findrange(unit, lower_left, lower_right,
                            upper_left, upper_right);
    if (!range)
        return FALSE;

    if (--range->add_count == 0)
    {
        Disable();
        Remove((struct Node *)range);
        Enable();
        FreeMem(range, sizeof(struct AddressRange));
        unit->gnu_RangeCount--;
        GENET_SetupRXFilter(base, unit);
    }

    return TRUE;
}

BOOL GENET_AddressFilter(struct GENETBase *base, struct GENETUnit *unit,
                         UBYTE *address)
{
    struct AddressRange *range;
    ULONG address_left;
    UWORD address_right;

    if (unit->gnu_Flags & IFF_PROMISC)
        return TRUE;

    /* The hardware's perfect filter already threw out foreign unicast */
    if ((address[0] & 0x1) == 0)
        return TRUE;

    if (genet_sameaddr(address, genet_broadcast))
        return TRUE;

    genet_splitaddr(address, &address_left, &address_right);

    ForeachNode(&unit->gnu_MulticastRanges, range)
    {
        if ((address_left > range->lower_bound_left ||
             (address_left == range->lower_bound_left &&
              address_right >= range->lower_bound_right)) &&
            (address_left < range->upper_bound_left ||
             (address_left == range->upper_bound_left &&
              address_right <= range->upper_bound_right)))
            return TRUE;
    }

    unit->gnu_SpecialStats[S2SS_ETHERNET_BADMULTICAST & 0xffff]++;
    return FALSE;
}

/* == the hardware receive filter ======================================== */

static void genet_mdf(struct genet_hw *hw, ULONG n, const UBYTE *ea)
{
    GENET_Write(hw, GENET_UMAC_MDF_ADDR0(n),
                ((ULONG)ea[0] << 8) | ea[1]);
    GENET_Write(hw, GENET_UMAC_MDF_ADDR1(n),
                ((ULONG)ea[2] << 24) | ((ULONG)ea[3] << 16) |
                ((ULONG)ea[4] << 8) | ea[5]);
}

/*
 * Load the perfect-match filter: broadcast, our own address, and one slot
 * per multicast address asked for. The control register enables slots
 * from the top down, so the count decides the mask.
 *
 * The filter matches whole addresses, while SANA-II hands out ranges. A
 * range covering exactly one address fits a slot; a wider one does not,
 * and there is no filter setting that expresses it. Rather than pretend,
 * such a range - or more addresses than there are slots - puts the
 * receiver in promiscuous mode and leaves the sorting to
 * GENET_AddressFilter(), which does it properly in software.
 */
void GENET_SetupRXFilter(struct GENETBase *base, struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;
    struct AddressRange *range;
    ULONG cmd, mdf_ctrl, n = 2;
    BOOL software = FALSE;

    ForeachNode(&unit->gnu_MulticastRanges, range)
    {
        if (range->lower_bound_left != range->upper_bound_left ||
            range->lower_bound_right != range->upper_bound_right)
        {
            software = TRUE;
            break;
        }
        if (++n > GENET_MAX_MDF_FILTER)
        {
            software = TRUE;
            break;
        }
    }

    cmd = GENET_Read(hw, GENET_UMAC_CMD);

    if (software || (unit->gnu_Flags & IFF_PROMISC))
    {
        cmd |= GENET_UMAC_CMD_PROMISC;
        mdf_ctrl = 0;
    }
    else
    {
        n = 0;
        genet_mdf(hw, n++, genet_broadcast);
        genet_mdf(hw, n++, unit->gnu_DevAddr);

        ForeachNode(&unit->gnu_MulticastRanges, range)
        {
            UBYTE ea[ETH_ADDRESSSIZE];

            ea[0] = (range->lower_bound_left >> 24) & 0xff;
            ea[1] = (range->lower_bound_left >> 16) & 0xff;
            ea[2] = (range->lower_bound_left >> 8) & 0xff;
            ea[3] = range->lower_bound_left & 0xff;
            ea[4] = (range->lower_bound_right >> 8) & 0xff;
            ea[5] = range->lower_bound_right & 0xff;

            genet_mdf(hw, n++, ea);
        }

        cmd &= ~GENET_UMAC_CMD_PROMISC;
        mdf_ctrl = ((1UL << GENET_MAX_MDF_FILTER) - 1) &
                   ~((1UL << (GENET_MAX_MDF_FILTER - n)) - 1);
    }

    GENET_Write(hw, GENET_UMAC_CMD, cmd);
    GENET_Write(hw, GENET_UMAC_MDF_CTRL, mdf_ctrl);

    D(bug("[genet] filter: %s, %u slots\n",
          (cmd & GENET_UMAC_CMD_PROMISC) ? "promiscuous" : "perfect match",
          (cmd & GENET_UMAC_CMD_PROMISC) ? 0 : n);)
}

/* == receive delivery =================================================== */

void GENET_CopyPacket(struct GENETBase *base, struct GENETUnit *unit,
                      struct IOSana2Req *request, ULONG packet_size,
                      UWORD packet_type, struct eth_frame *frame)
{
    struct Library *UtilityBase = base->gnb_UtilityBase;
    struct Opener *opener;
    UBYTE *ptr;
    BOOL filtered = FALSE;

    request->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (genet_sameaddr(frame->eth_packet_dest, genet_broadcast))
        request->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if ((frame->eth_packet_dest[0] & 0x1) != 0)
        request->ios2_Req.io_Flags |= SANA2IOF_MCAST;

    CopyMem(frame->eth_packet_source, request->ios2_SrcAddr, ETH_ADDRESSSIZE);
    CopyMem(frame->eth_packet_dest, request->ios2_DstAddr, ETH_ADDRESSSIZE);
    request->ios2_PacketType = packet_type;

    if ((request->ios2_Req.io_Flags & SANA2IOF_RAW) == 0)
    {
        packet_size -= ETH_HEADERSIZE;
        ptr = frame->eth_packet_data;
    }
    else
        ptr = (UBYTE *)frame;

    request->ios2_DataLength = packet_size;

    opener = request->ios2_BufferManagement;
    if (request->ios2_Req.io_Command == CMD_READ &&
        opener->filter_hook != NULL)
    {
        if (!CallHookPkt(opener->filter_hook, request, ptr))
            filtered = TRUE;
    }

    /* A rejected packet leaves the request queued for the next one */
    if (filtered)
        return;

    if (!opener->rx_function(request->ios2_Data, ptr, packet_size))
    {
        request->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        request->ios2_WireError = S2WERR_BUFF_ERROR;
        GENET_ReportEvents(base, unit, S2EVENT_ERROR | S2EVENT_SOFTWARE |
                           S2EVENT_BUFF | S2EVENT_RX);
    }

    Disable();
    Remove((APTR)request);
    Enable();
    ReplyMsg((APTR)request);
}

static void GENET_RXPacket(struct GENETBase *base, struct GENETUnit *unit,
                           struct eth_frame *frame, ULONG length)
{
    struct Opener *opener, *opener_tail;
    struct IOSana2Req *request, *request_tail;
    struct TypeStats *tracker;
    BOOL accepted, is_orphan = TRUE;
    UWORD packet_type;

    if (!GENET_AddressFilter(base, unit, frame->eth_packet_dest))
        return;

    /*
     * Kept unsigned: an ethertype with the high bit set would otherwise
     * sign-extend and never match an opener's ULONG packet type.
     */
    packet_type = (UWORD)AROS_BE2WORD(frame->eth_packet_type);

    unit->gnu_Stats.PacketsReceived++;

    tracker = GENET_FindTypeStats(&unit->gnu_TypeTrackers, packet_type);
    if (tracker)
    {
        tracker->stats.PacketsReceived++;
        tracker->stats.BytesReceived += length;
    }

    opener = (APTR)unit->gnu_Openers.mlh_Head;
    opener_tail = (APTR)&unit->gnu_Openers.mlh_Tail;

    while (opener != opener_tail)
    {
        request = (APTR)opener->read_port.mp_MsgList.lh_Head;
        request_tail = (APTR)&opener->read_port.mp_MsgList.lh_Tail;
        accepted = FALSE;

        while (request != request_tail && !accepted)
        {
            struct IOSana2Req *next =
                (APTR)request->ios2_Req.io_Message.mn_Node.ln_Succ;

            /* An 802.3 length field matches any small-type request */
            if (request->ios2_PacketType == packet_type ||
                (request->ios2_PacketType <= ETH_MTU &&
                 packet_type <= ETH_MTU))
            {
                GENET_CopyPacket(base, unit, request, length, packet_type,
                                 frame);
                accepted = TRUE;
            }
            request = next;
        }

        if (accepted)
            is_orphan = FALSE;

        opener = (APTR)opener->node.mln_Succ;
    }

    if (is_orphan)
    {
        unit->gnu_Stats.UnknownTypesReceived++;

        if (!IsListEmpty(&unit->gnu_RequestPorts[ADOPT_QUEUE]->mp_MsgList))
        {
            GENET_CopyPacket(base, unit,
                (APTR)unit->gnu_RequestPorts[ADOPT_QUEUE]->mp_MsgList.lh_Head,
                length, packet_type, frame);
        }
    }
}

/* == the receive ring =================================================== */

/*
 * Walk the descriptors the block has filled since last time.
 *
 * There is no ownership bit to test and no descriptor to hand back: the
 * producer index says how far the block has got, and writing the
 * consumer index back is what releases the slots. The buffer address in
 * each descriptor is written once at ring setup and never changes, so
 * nothing about the descriptor needs touching here at all.
 */
static void GENET_RXProcess(struct GENETUnit *unit)
{
    struct GENETBase *base = unit->gnu_Base;
    struct genet_hw *hw = unit->gnu_HW;
    ULONG prod, total, n;

    prod = GENET_Read(hw, GENET_RX_DMA_PROD_INDEX(GENET_QID)) &
           GENET_DMA_PROD_CONS_MASK;
    total = (prod - unit->gnu_RXCons) & GENET_DMA_PROD_CONS_MASK;

    /*
     * The block stops when the ring is full, so it cannot legitimately
     * be more than a ring ahead. Clamping costs nothing and means a
     * consumer index that has somehow lost track walks the ring once
     * rather than round and round it.
     */
    if (total > GENET_DMA_DESC_COUNT)
        total = GENET_DMA_DESC_COUNT;

    for (n = 0; n < total; n++)
    {
        ULONG index = unit->gnu_RXCons & GENET_RING_MASK;
        ULONG status = GENET_Read(hw, GENET_RX_DESC_STATUS(index));
        ULONG length = (status & GENET_RX_DESC_STATUS_BUFLEN_MASK) >>
                       GENET_RX_DESC_STATUS_BUFLEN_SHIFT;
        UBYTE *buf = unit->gnu_RXBuf + index * GENET_BUFSIZE;

        /*
         * A good frame arrived whole and without complaint. Anything
         * else - a fragment of a frame too big for one buffer, or a
         * receive error - is counted and dropped.
         */
        if ((status & (GENET_RX_DESC_STATUS_SOP | GENET_RX_DESC_STATUS_EOP |
                       GENET_RX_DESC_STATUS_RX_ERROR)) ==
            (GENET_RX_DESC_STATUS_SOP | GENET_RX_DESC_STATUS_EOP))
        {
            if (length > GENET_BUFSIZE)
                length = GENET_BUFSIZE;

            /*
             * The two pad bytes the receiver puts in front of the frame
             * are counted in the reported length. The frame check
             * sequence is not: with CRC forwarding off, which is how
             * the reset leaves it, the MAC strips it.
             */
            if (length >= GENET_RX_PAD + ETH_HEADERSIZE)
                GENET_RXPacket(base, unit,
                               (struct eth_frame *)(buf + GENET_RX_PAD),
                               length - GENET_RX_PAD);
        }
        else
            unit->gnu_Stats.BadData++;

        /*
         * Released whatever the descriptor said. The reference driver
         * skips this step for a descriptor it rejected, which leaves the
         * slot in the block's hands and re-reads it on every following
         * pass; one bad frame is enough to stop the ring for good.
         */
        unit->gnu_RXCons = (unit->gnu_RXCons + 1) &
                           GENET_DMA_PROD_CONS_MASK;
        GENET_Write(hw, GENET_RX_DMA_CONS_INDEX(GENET_QID),
                    unit->gnu_RXCons);
    }
}

/* == the transmit ring ================================================== */

static AROS_INTH1(GENET_TXIntF, struct GENETUnit *, unit)
{
    AROS_INTFUNC_INIT

    struct GENETBase *base = unit->gnu_Base;
    struct genet_hw *hw = unit->gnu_HW;
    struct MsgPort *port = unit->gnu_RequestPorts[WRITE_QUEUE];

    for (;;)
    {
        /* Hand over as much as there are requests and room for */
        while (!IsListEmpty(&port->mp_MsgList) &&
               GENET_TX_INFLIGHT(unit) < GENET_DMA_DESC_COUNT)
        {
            ULONG index = unit->gnu_TXProd & GENET_RING_MASK;
            UBYTE *buffer = unit->gnu_TXBuf + index * GENET_BUFSIZE;
            struct IOSana2Req *request;
            struct Opener *opener;
            ULONG data_size, packet_size, wire_error = 0;
            BYTE error = 0;

            request = (APTR)port->mp_MsgList.lh_Head;
            data_size = packet_size = request->ios2_DataLength;
            opener = request->ios2_BufferManagement;

            if ((request->ios2_Req.io_Flags & SANA2IOF_RAW) == 0)
            {
                struct eth_frame *frame = (struct eth_frame *)buffer;

                packet_size += ETH_HEADERSIZE;
                CopyMem(request->ios2_DstAddr, frame->eth_packet_dest,
                        ETH_ADDRESSSIZE);
                CopyMem(unit->gnu_DevAddr, frame->eth_packet_source,
                        ETH_ADDRESSSIZE);
                frame->eth_packet_type =
                    AROS_WORD2BE((UWORD)request->ios2_PacketType);

                if (!opener->tx_function(frame->eth_packet_data,
                                         request->ios2_Data, data_size))
                    error = S2ERR_NO_RESOURCES;
            }
            else
            {
                if (!opener->tx_function(buffer, request->ios2_Data,
                                         data_size))
                    error = S2ERR_NO_RESOURCES;
            }

            if (error != 0)
            {
                wire_error = S2WERR_BUFF_ERROR;
                GENET_ReportEvents(base, unit, S2EVENT_ERROR |
                                   S2EVENT_SOFTWARE | S2EVENT_BUFF |
                                   S2EVENT_TX);
            }
            else
            {
                struct TypeStats *tracker;
                UWORD packet_type;
                IPTR phys = (IPTR)buffer;

                /* The wire has a minimum frame; pad to reach it */
                while (packet_size < ETH_ZLEN)
                    buffer[packet_size++] = 0;

                packet_type = ((UWORD)buffer[12] << 8) | buffer[13];

                /*
                 * One buffer holds the whole frame, so this descriptor
                 * is both the start and the end of the packet. QTAG
                 * carries no vlan tag here and is set to all ones, as
                 * the reference driver does; CRC tells the MAC to
                 * append the frame check sequence itself.
                 */
                GENET_Write(hw, GENET_TX_DESC_ADDRESS_LO(index),
                            (ULONG)phys);
                GENET_Write(hw, GENET_TX_DESC_ADDRESS_HI(index),
                            (ULONG)((UQUAD)phys >> 32));
                GENET_Write(hw, GENET_TX_DESC_STATUS(index),
                            GENET_TX_DESC_STATUS_QTAG_MASK |
                            GENET_TX_DESC_STATUS_SOP |
                            GENET_TX_DESC_STATUS_EOP |
                            GENET_TX_DESC_STATUS_CRC |
                            (packet_size <<
                             GENET_TX_DESC_STATUS_BUFLEN_SHIFT));

                /*
                 * The producer index is what sets the block going, so
                 * it is written after the descriptor is complete.
                 */
                unit->gnu_TXProd = (unit->gnu_TXProd + 1) &
                                   GENET_DMA_PROD_CONS_MASK;
                GENET_Write(hw, GENET_TX_DMA_PROD_INDEX(GENET_QID),
                            unit->gnu_TXProd);

                unit->gnu_Stats.PacketsSent++;
                tracker = GENET_FindTypeStats(&unit->gnu_TypeTrackers,
                                              packet_type);
                if (tracker)
                {
                    tracker->stats.PacketsSent++;
                    tracker->stats.BytesSent += packet_size;
                }
            }

            request->ios2_Req.io_Error = error;
            request->ios2_WireError = wire_error;
            Disable();
            Remove((APTR)request);
            Enable();
            ReplyMsg((APTR)request);
        }

        /* Nothing waiting: the queue stays open for whatever comes next */
        if (IsListEmpty(&port->mp_MsgList))
        {
            port->mp_Flags = PA_SOFTINT;
            break;
        }

        /*
         * Requests are left, so the ring is full. Close the queue and
         * let the completion interrupt reopen it.
         */
        port->mp_Flags = PA_IGNORE;

        /*
         * That interrupt may have freed a slot between the test at the
         * top of the loop and the line above, in which case it saw an
         * open queue and left the restart to this side. It writes the
         * consumer index before it reads the flag, so looking once more
         * here cannot miss it - and going round again is cheaper and
         * safer than causing this interrupt from inside itself.
         */
        if (GENET_TX_INFLIGHT(unit) >= GENET_DMA_DESC_COUNT)
            break;

        port->mp_Flags = PA_SOFTINT;
    }

    return FALSE;

    AROS_INTFUNC_EXIT
}

static void GENET_TXComplete(struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;
    struct MsgPort *port = unit->gnu_RequestPorts[WRITE_QUEUE];
    ULONG cons;

    cons = GENET_Read(hw, GENET_TX_DMA_CONS_INDEX(GENET_QID)) &
           GENET_DMA_PROD_CONS_MASK;

    unit->gnu_TXCons = cons;

    if (port->mp_Flags == PA_IGNORE &&
        GENET_TX_INFLIGHT(unit) < GENET_DMA_DESC_COUNT)
    {
        port->mp_Flags = PA_SOFTINT;
        Cause(&unit->gnu_TXInt);
    }
}

/* == the interrupt ====================================================== */

static AROS_INTH1(GENET_IntHandlerF, struct GENETUnit *, unit)
{
    AROS_INTFUNC_INIT

    struct genet_hw *hw = unit->gnu_HW;
    ULONG status;

    status = GENET_Read(hw, GENET_INTRL2_CPU_STAT);
    status &= ~GENET_Read(hw, GENET_INTRL2_CPU_STAT_MASK);

    if (status == 0)
        return FALSE;

    GENET_Write(hw, GENET_INTRL2_CPU_CLEAR, status);

    if (status & GENET_IRQ_RXDMA_DONE)
        GENET_RXProcess(unit);

    if (status & GENET_IRQ_TXDMA_DONE)
        GENET_TXComplete(unit);

    return FALSE;

    AROS_INTFUNC_EXIT
}

/* == the rings ========================================================== */

/*
 * Set up the transmit ring for the default queue. The start and end
 * addresses are offsets in 32-bit words into the descriptor area, and
 * this queue is given all of it.
 */
static void GENET_InitTXRing(struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;

    unit->gnu_TXProd = 0;
    unit->gnu_TXCons = 0;

    GENET_Write(hw, GENET_TX_SCB_BURST_SIZE, GENET_DMA_MAX_BURST_LENGTH);

    GENET_Write(hw, GENET_TX_DMA_READ_PTR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_READ_PTR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_CONS_INDEX(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_PROD_INDEX(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_RING_BUF_SIZE(GENET_QID),
                (GENET_DMA_DESC_COUNT <<
                 GENET_TX_DMA_RING_BUF_SIZE_DESC_SHIFT) |
                (GENET_BUFSIZE & GENET_TX_DMA_RING_BUF_SIZE_BUF_LEN_MASK));
    GENET_Write(hw, GENET_TX_DMA_START_ADDR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_START_ADDR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_END_ADDR_LO(GENET_QID),
                GENET_DESC_AREA_WORDS - 1);
    GENET_Write(hw, GENET_TX_DMA_END_ADDR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_MBUF_DONE_THRES(GENET_QID), 1);
    GENET_Write(hw, GENET_TX_DMA_FLOW_PERIOD(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_WRITE_PTR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_TX_DMA_WRITE_PTR_HI(GENET_QID), 0);

    GENET_Write(hw, GENET_TX_DMA_RING_CFG, 1UL << GENET_QID);

    GENET_Write(hw, GENET_TX_DMA_CTRL,
                GENET_Read(hw, GENET_TX_DMA_CTRL) | GENET_DMA_CTRL_EN |
                GENET_DMA_CTRL_RBUF_EN(GENET_QID));
}

/*
 * The receive ring, and the buffer each of its descriptors points at.
 * The addresses are written once here; the block never changes them and
 * neither does the receive path.
 */
static void GENET_InitRXRing(struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;
    ULONG i;

    unit->gnu_RXCons = 0;

    GENET_Write(hw, GENET_RX_SCB_BURST_SIZE, GENET_DMA_MAX_BURST_LENGTH);

    GENET_Write(hw, GENET_RX_DMA_WRITE_PTR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_WRITE_PTR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_PROD_INDEX(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_CONS_INDEX(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_RING_BUF_SIZE(GENET_QID),
                (GENET_DMA_DESC_COUNT <<
                 GENET_RX_DMA_RING_BUF_SIZE_DESC_SHIFT) |
                (GENET_BUFSIZE & GENET_RX_DMA_RING_BUF_SIZE_BUF_LEN_MASK));
    GENET_Write(hw, GENET_RX_DMA_START_ADDR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_START_ADDR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_END_ADDR_LO(GENET_QID),
                GENET_DESC_AREA_WORDS - 1);
    GENET_Write(hw, GENET_RX_DMA_END_ADDR_HI(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_XON_XOFF_THRES(GENET_QID),
                (5 << GENET_RX_DMA_XON_XOFF_THRES_LO_SHIFT) |
                (GENET_DMA_DESC_COUNT >> 4));
    GENET_Write(hw, GENET_RX_DMA_READ_PTR_LO(GENET_QID), 0);
    GENET_Write(hw, GENET_RX_DMA_READ_PTR_HI(GENET_QID), 0);

    GENET_Write(hw, GENET_RX_DMA_RING_CFG, 1UL << GENET_QID);

    for (i = 0; i < GENET_DMA_DESC_COUNT; i++)
    {
        IPTR phys = (IPTR)(unit->gnu_RXBuf + i * GENET_BUFSIZE);

        GENET_Write(hw, GENET_RX_DESC_ADDRESS_LO(i), (ULONG)phys);
        GENET_Write(hw, GENET_RX_DESC_ADDRESS_HI(i),
                    (ULONG)((UQUAD)phys >> 32));
    }

    GENET_Write(hw, GENET_RX_DMA_CTRL,
                GENET_Read(hw, GENET_RX_DMA_CTRL) | GENET_DMA_CTRL_EN |
                GENET_DMA_CTRL_RBUF_EN(GENET_QID));
}

/* == bringing the interface up and down ================================= */

void GENET_GoOnline(struct GENETBase *base, struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;
    ULONG val;

    unit->gnu_Stats.PacketsSent = 0;
    unit->gnu_Stats.PacketsReceived = 0;
    unit->gnu_Stats.BadData = 0;
    unit->gnu_Stats.Overruns = 0;
    unit->gnu_Stats.UnknownTypesReceived = 0;

    /* An external gigabit PHY on rgmii is the only wiring this sees */
    GENET_Write(hw, GENET_SYS_PORT_CTRL, GENET_SYS_PORT_MODE_EXT_GPHY);

    GENET_SetMACAddress(hw, unit->gnu_DevAddr);
    GENET_SetupRXFilter(base, unit);

    GENET_InitTXRing(unit);
    GENET_InitRXRing(unit);

    GENET_Write(hw, GENET_UMAC_MAX_FRAME_LEN, 1536);

    /*
     * Two pad bytes in front of every received frame, so the network
     * header behind the ethernet one is four-byte aligned.
     */
    GENET_Write(hw, GENET_RBUF_CTRL,
                GENET_Read(hw, GENET_RBUF_CTRL) | GENET_RBUF_ALIGN_2B);

    GENET_Write(hw, GENET_RBUF_TBUF_SIZE_CTRL, 1);

    /*
     * No checksum offload, in either direction. That keeps the 64-byte
     * status block off the front of every packet, which is the only
     * reason the receive path can read a length straight out of the
     * descriptor.
     */
    GENET_Write(hw, GENET_RBUF_CHECK_CTRL,
                GENET_Read(hw, GENET_RBUF_CHECK_CTRL) &
                ~GENET_RBUF_CHECK_CTRL_EN);
    GENET_Write(hw, GENET_RBUF_CTRL,
                GENET_Read(hw, GENET_RBUF_CTRL) & ~GENET_RBUF_64B_EN);
    GENET_Write(hw, GENET_TBUF_CTRL,
                GENET_Read(hw, GENET_TBUF_CTRL) & ~GENET_RBUF_64B_EN);

    GENET_SetSpeed(hw, unit->gnu_SpeedMbps, unit->gnu_FullDuplex);

    val = GENET_Read(hw, GENET_UMAC_CMD);
    val |= GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    GENET_Write(hw, GENET_UMAC_CMD, val);

    /* Clearing a mask bit is what enables the source */
    GENET_Write(hw, GENET_INTRL2_CPU_CLEAR_MASK,
                GENET_IRQ_TXDMA_DONE | GENET_IRQ_RXDMA_DONE);

    unit->gnu_Flags |= IFF_UP;

    /* Ring and MAC are live, so the queue may open */
    unit->gnu_RequestPorts[WRITE_QUEUE]->mp_Flags = PA_SOFTINT;

    D(bug("[genet] online, %u Mbit %s duplex%s\n", unit->gnu_SpeedMbps,
          unit->gnu_FullDuplex ? "full" : "half",
          unit->gnu_LinkUp ? "" : " (link still negotiating)");)

    GENET_ReportEvents(base, unit, S2EVENT_ONLINE);
}

void GENET_GoOffline(struct GENETBase *base, struct GENETUnit *unit)
{
    struct genet_hw *hw = unit->gnu_HW;
    struct IOSana2Req *request;

    unit->gnu_Flags &= ~IFF_UP;

    /*
     * Shut the transmit queue first. A soft interrupt already pending
     * would otherwise be free to program a descriptor into a ring that
     * is in the middle of being stopped.
     */
    unit->gnu_RequestPorts[WRITE_QUEUE]->mp_Flags = PA_IGNORE;

    /* Setting a mask bit disables the source; clear any that are pending */
    GENET_Write(hw, GENET_INTRL2_CPU_SET_MASK, 0xffffffff);
    GENET_Write(hw, GENET_INTRL2_CPU_CLEAR, 0xffffffff);

    GENET_Write(hw, GENET_UMAC_CMD,
                GENET_Read(hw, GENET_UMAC_CMD) &
                ~(GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN));

    GENET_DMADisable(hw);

    while ((request = (struct IOSana2Req *)
                GetMsg(unit->gnu_RequestPorts[WRITE_QUEUE])) != NULL)
    {
        request->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
        request->ios2_WireError = S2WERR_UNIT_OFFLINE;
        ReplyMsg((APTR)request);
    }

    GENET_ReportEvents(base, unit, S2EVENT_OFFLINE);
}

void GENET_FlushUnit(struct GENETBase *base, struct GENETUnit *unit,
                     UBYTE last_queue, BYTE error)
{
    struct IOSana2Req *request;
    struct Opener *opener;
    UBYTE i;

    for (i = 0; i <= last_queue; i++)
    {
        while ((request = (struct IOSana2Req *)
                    GetMsg(unit->gnu_RequestPorts[i])) != NULL)
        {
            request->ios2_Req.io_Error = error;
            ReplyMsg((APTR)request);
        }
    }

    ForeachNode(&unit->gnu_Openers, opener)
    {
        while ((request = (struct IOSana2Req *)
                    GetMsg(&opener->read_port)) != NULL)
        {
            request->ios2_Req.io_Error = error;
            ReplyMsg((APTR)request);
        }
    }
}

/* == the unit process =================================================== */

static void GENET_CheckLink(struct GENETBase *base, struct GENETUnit *unit)
{
    ULONG mbps = unit->gnu_SpeedMbps;
    BOOL fullduplex = unit->gnu_FullDuplex;
    BOOL up = GENET_PHYGetLink(unit->gnu_HW, &mbps, &fullduplex);

    if (up == unit->gnu_LinkUp &&
        (!up || (mbps == unit->gnu_SpeedMbps &&
                 fullduplex == unit->gnu_FullDuplex)))
        return;

    unit->gnu_LinkUp = up;

    if (up)
    {
        unit->gnu_SpeedMbps = mbps;
        unit->gnu_FullDuplex = fullduplex;
        unit->gnu_Sana2Info.BPS = mbps * 1000000UL;
        GENET_SetSpeed(unit->gnu_HW, mbps, fullduplex);
    }

    D(bug("[genet] link %s, %u Mbit %s duplex\n", up ? "up" : "down",
          mbps, fullduplex ? "full" : "half");)
}

static void GENET_UnitTask(void)
{
    struct GENETUnit *unit =
        (struct GENETUnit *)FindTask(NULL)->tc_UserData;
    struct GENETBase *base = unit->gnu_Base;
    struct MsgPort *input, *timerport = NULL;
    struct timerequest *timerreq = NULL;
    struct IOSana2Req *request;
    BOOL timeropen = FALSE, timerpending = FALSE, running = FALSE;
    ULONG inputsig = 0, timersig = 0, sigs;

    input = CreateMsgPort();
    timerport = CreateMsgPort();
    if (timerport)
        timerreq = (struct timerequest *)
            CreateIORequest(timerport, sizeof(struct timerequest));
    if (timerreq &&
        OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                   (struct IORequest *)timerreq, 0) == 0)
        timeropen = TRUE;

    if (input && timeropen)
    {
        unit->gnu_InputPort = input;
        inputsig = 1UL << input->mp_SigBit;
        timersig = 1UL << timerport->mp_SigBit;
        running = TRUE;

        timerreq->tr_node.io_Command = TR_ADDREQUEST;
        timerreq->tr_time.tv_secs = 1;
        timerreq->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)timerreq);
        timerpending = TRUE;

        Signal(unit->gnu_DeathWatch, SIGF_SINGLE);
    }

    while (running)
    {
        sigs = Wait(inputsig | timersig | SIGBREAKF_CTRL_C);

        if (sigs & SIGBREAKF_CTRL_C)
            running = FALSE;

        if (sigs & inputsig)
        {
            while ((request = (struct IOSana2Req *)GetMsg(input)) != NULL)
            {
                ObtainSemaphore(&unit->gnu_Lock);
                GENET_HandleRequest(base, request);
            }
        }

        if (sigs & timersig)
        {
            if (GetMsg(timerport))
            {
                timerpending = FALSE;

                ObtainSemaphore(&unit->gnu_Lock);
                GENET_CheckLink(base, unit);
                ReleaseSemaphore(&unit->gnu_Lock);

                if (running)
                {
                    timerreq->tr_node.io_Command = TR_ADDREQUEST;
                    timerreq->tr_time.tv_secs = 1;
                    timerreq->tr_time.tv_micro = 0;
                    SendIO((struct IORequest *)timerreq);
                    timerpending = TRUE;
                }
            }
        }
    }

    unit->gnu_InputPort = NULL;

    if (timeropen)
    {
        if (timerpending)
        {
            AbortIO((struct IORequest *)timerreq);
            WaitIO((struct IORequest *)timerreq);
        }
        CloseDevice((struct IORequest *)timerreq);
    }
    if (timerreq)
        DeleteIORequest((struct IORequest *)timerreq);
    if (timerport)
        DeleteMsgPort(timerport);
    if (input)
    {
        while ((request = (struct IOSana2Req *)GetMsg(input)) != NULL)
        {
            request->ios2_Req.io_Error = IOERR_ABORTED;
            ReplyMsg((APTR)request);
        }
        DeleteMsgPort(input);
    }

    /*
     * The stack and task structure are on tc_MemEntry, so returning -
     * which removes the task - also frees them. Nothing of the unit may
     * be touched after the signal.
     */
    Forbid();
    unit->gnu_Task = NULL;
    Signal(unit->gnu_DeathWatch, SIGF_SINGLE);
}

/* == packet buffers ===================================================== */

#define GENET_PAGE_SIZE 4096

/*
 * The buffers the controller reads and writes. Two things have to hold
 * for them, and neither comes for free.
 *
 * They have to be at addresses the controller can reach. AROS maps RAM
 * one to one on this platform, so an allocation's address is also its
 * physical address and no translation is needed - but both halves of
 * every descriptor address are written anyway, so RAM above four
 * gigabytes would work as well.
 *
 * They have to be memory the processor does not cache, because the
 * controller's accesses are not snooped. The mapping is changed a page
 * at a time, which is why the allocation is padded and aligned: making
 * a sub-page range uncacheable would drag its neighbours along.
 */
static BOOL genet_allocbuffers(struct GENETUnit *unit)
{
    APTR KernelBase = unit->gnu_Base->gnb_KernelBase;
    ULONG want = 2 * GENET_DMA_DESC_COUNT * GENET_BUFSIZE;

    unit->gnu_BufMapSize = (want + GENET_PAGE_SIZE - 1) &
                           ~(GENET_PAGE_SIZE - 1);
    unit->gnu_BufMemSize = unit->gnu_BufMapSize + GENET_PAGE_SIZE;

    unit->gnu_BufMem = AllocMem(unit->gnu_BufMemSize,
                                MEMF_PUBLIC | MEMF_CLEAR);
    if (!unit->gnu_BufMem)
        return FALSE;

    unit->gnu_BufBase = (APTR)(((IPTR)unit->gnu_BufMem +
                                GENET_PAGE_SIZE - 1) &
                               ~(IPTR)(GENET_PAGE_SIZE - 1));

    /*
     * Push out anything the allocation left dirty before the memory
     * stops being cacheable, then drop what is left afterwards so no
     * stale line can shadow the new mapping.
     */
    genet_cacheflush(unit->gnu_BufBase, unit->gnu_BufMapSize);

    if (!KrnMapGlobal(unit->gnu_BufBase, unit->gnu_BufBase,
                      unit->gnu_BufMapSize,
                      MAP_Readable | MAP_Writable | MAP_WriteThrough))
    {
        D(bug("[genet] could not make %p+%08x uncacheable\n",
              unit->gnu_BufBase, unit->gnu_BufMapSize);)
        FreeMem(unit->gnu_BufMem, unit->gnu_BufMemSize);
        unit->gnu_BufMem = NULL;
        return FALSE;
    }

    genet_cacheflush(unit->gnu_BufBase, unit->gnu_BufMapSize);

    unit->gnu_TXBuf = (UBYTE *)unit->gnu_BufBase;
    unit->gnu_RXBuf = unit->gnu_TXBuf +
                      GENET_DMA_DESC_COUNT * GENET_BUFSIZE;

    D(bug("[genet] buffers at %p+%08x, uncached; tx %p rx %p\n",
          unit->gnu_BufBase, unit->gnu_BufMapSize,
          unit->gnu_TXBuf, unit->gnu_RXBuf);)

    return TRUE;
}

static void genet_freebuffers(struct GENETUnit *unit)
{
    APTR KernelBase = unit->gnu_Base->gnb_KernelBase;

    if (!unit->gnu_BufMem)
        return;

    /*
     * Hand the pages back the way they were found. Leaving them
     * uncacheable would quietly slow down whatever allocates them next.
     */
    if (unit->gnu_BufBase)
    {
        KrnMapGlobal(unit->gnu_BufBase, unit->gnu_BufBase,
                     unit->gnu_BufMapSize, MAP_Readable | MAP_Writable);
        genet_cacheflush(unit->gnu_BufBase, unit->gnu_BufMapSize);
    }

    FreeMem(unit->gnu_BufMem, unit->gnu_BufMemSize);
    unit->gnu_BufMem = NULL;
    unit->gnu_BufBase = NULL;
}

/* == unit lifetime ====================================================== */

/* A station address must have bits and must not be a group address */
static BOOL genet_validmac(const UBYTE *addr)
{
    ULONG i, bits = 0;

    for (i = 0; i < ETH_ADDRESSSIZE; i++)
        bits |= addr[i];

    return (bits != 0) && ((addr[0] & 0x1) == 0);
}

struct GENETUnit *GENET_CreateUnit(struct GENETBase *base)
{
    struct GENETUnit *unit;
    struct genet_hw *hw = &base->gnb_HW;
    struct Task *task;
    struct MemList *ml;
    UBYTE *stack;
    ULONG i;

    unit = AllocMem(sizeof(struct GENETUnit), MEMF_PUBLIC | MEMF_CLEAR);
    if (!unit)
        return NULL;

    unit->gnu_Base = base;
    unit->gnu_HW = hw;

    InitSemaphore(&unit->gnu_Lock);
    NEWLIST(&unit->gnu_Openers);
    NEWLIST(&unit->gnu_MulticastRanges);
    NEWLIST(&unit->gnu_TypeTrackers);

    /* Until the PHY reports in, assume the fastest the part can do */
    unit->gnu_SpeedMbps = 1000;
    unit->gnu_FullDuplex = TRUE;

    unit->gnu_TXInt.is_Node.ln_Type = NT_INTERRUPT;
    unit->gnu_TXInt.is_Node.ln_Name = "genet tx";
    unit->gnu_TXInt.is_Code = (VOID_FUNC)GENET_TXIntF;
    unit->gnu_TXInt.is_Data = unit;

    for (i = 0; i < REQUEST_QUEUE_COUNT; i++)
    {
        struct MsgPort *port =
            AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);

        unit->gnu_RequestPorts[i] = port;
        if (!port)
        {
            GENET_DeleteUnit(base, unit);
            return NULL;
        }
        NEWLIST(&port->mp_MsgList);
        port->mp_Flags = PA_IGNORE;
        port->mp_SigTask = &unit->gnu_TXInt;
    }

    if (!genet_allocbuffers(unit))
    {
        GENET_DeleteUnit(base, unit);
        return NULL;
    }

    /*
     * The tree's word beats the address registers, which beat nothing -
     * but firmware leaves placeholders in both, so each source has to
     * earn its keep.
     */
    if (hw->haveMacAddr && genet_validmac(hw->macAddr))
        CopyMem(hw->macAddr, unit->gnu_OrgAddr, ETH_ADDRESSSIZE);
    else if (!(GENET_GetMACAddress(hw, unit->gnu_OrgAddr) &&
               genet_validmac(unit->gnu_OrgAddr)))
    {
        static const UBYTE fallback[ETH_ADDRESSSIZE] =
            { 0x02, 'A', 'R', 'O', 'S', 0x00 };

        D(bug("[genet] no usable MAC address anywhere, making one up\n");)
        CopyMem((APTR)fallback, unit->gnu_OrgAddr, ETH_ADDRESSSIZE);
    }
    CopyMem(unit->gnu_OrgAddr, unit->gnu_DevAddr, ETH_ADDRESSSIZE);

    D(bug("[genet] station address %02x:%02x:%02x:%02x:%02x:%02x\n",
          unit->gnu_OrgAddr[0], unit->gnu_OrgAddr[1], unit->gnu_OrgAddr[2],
          unit->gnu_OrgAddr[3], unit->gnu_OrgAddr[4], unit->gnu_OrgAddr[5]);)

    /*
     * Reset before anything is configured, and stop the DMA engines
     * before their rings are rebuilt - the firmware may well have left
     * them running from its own netboot.
     */
    GENET_Reset(hw);
    GENET_DMADisable(hw);
    GENET_Write(hw, GENET_INTRL2_CPU_SET_MASK, 0xffffffff);
    GENET_Write(hw, GENET_INTRL2_CPU_CLEAR, 0xffffffff);

    GENET_SetMACAddress(hw, unit->gnu_DevAddr);
    GENET_PHYInit(hw);

    unit->gnu_Sana2Info.SizeAvailable = sizeof(struct Sana2DeviceQuery);
    unit->gnu_Sana2Info.SizeSupplied = sizeof(struct Sana2DeviceQuery);
    unit->gnu_Sana2Info.DevQueryFormat = 0;
    unit->gnu_Sana2Info.DeviceLevel = 0;
    unit->gnu_Sana2Info.AddrFieldSize = 8 * ETH_ADDRESSSIZE;
    unit->gnu_Sana2Info.MTU = ETH_MTU;
    unit->gnu_Sana2Info.BPS = unit->gnu_SpeedMbps * 1000000UL;
    unit->gnu_Sana2Info.HardwareType = S2WireType_Ethernet;

    if (!hw->irq)
    {
        D(bug("[genet] the tree names no interrupt source\n");)
        GENET_DeleteUnit(base, unit);
        return NULL;
    }

    /*
     * Only the first interrupt is taken. The block raises the second for
     * PHY and link events, which this driver learns about by asking the
     * PHY once a second instead.
     */
    unit->gnu_IRQHandler.is_Node.ln_Type = NT_INTERRUPT;
    unit->gnu_IRQHandler.is_Node.ln_Name = "genet";
    unit->gnu_IRQHandler.is_Code = (VOID_FUNC)GENET_IntHandlerF;
    unit->gnu_IRQHandler.is_Data = unit;
    AddIntServer(INTB_KERNEL + hw->irq, &unit->gnu_IRQHandler);
    unit->gnu_IRQAdded = TRUE;
    D(bug("[genet] interrupt server on id %u\n", hw->irq);)

    unit->gnu_RequestPorts[WRITE_QUEUE]->mp_Flags = PA_SOFTINT;

    /*
     * The service task. Its stack and control block ride on
     * tc_MemEntry, so task exit frees them without help.
     */
    task = AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
    stack = AllocMem(GENET_TASK_STACK, MEMF_PUBLIC);
    ml = AllocMem(sizeof(struct MemList) + sizeof(struct MemEntry),
                  MEMF_PUBLIC | MEMF_CLEAR);
    if (!task || !stack || !ml)
    {
        if (task)
            FreeMem(task, sizeof(struct Task));
        if (stack)
            FreeMem(stack, GENET_TASK_STACK);
        if (ml)
            FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        GENET_DeleteUnit(base, unit);
        return NULL;
    }

    ml->ml_NumEntries = 2;
    ml->ml_ME[0].me_Addr = task;
    ml->ml_ME[0].me_Length = sizeof(struct Task);
    ml->ml_ME[1].me_Addr = stack;
    ml->ml_ME[1].me_Length = GENET_TASK_STACK;

    task->tc_Node.ln_Type = NT_TASK;
    task->tc_Node.ln_Pri = 0;
    task->tc_Node.ln_Name = "genet.device";
    task->tc_SPLower = stack;
    task->tc_SPUpper = stack + GENET_TASK_STACK;
    task->tc_SPReg = stack + GENET_TASK_STACK;
    NEWLIST(&task->tc_MemEntry);
    AddTail(&task->tc_MemEntry, (struct Node *)ml);
    task->tc_UserData = unit;

    unit->gnu_Task = task;
    unit->gnu_DeathWatch = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);

    if (AddTask(task, GENET_UnitTask, NULL) == NULL)
    {
        unit->gnu_Task = NULL;
        FreeMem(ml, sizeof(struct MemList) + sizeof(struct MemEntry));
        FreeMem(stack, GENET_TASK_STACK);
        FreeMem(task, sizeof(struct Task));
        GENET_DeleteUnit(base, unit);
        return NULL;
    }

    Wait(SIGF_SINGLE);
    if (!unit->gnu_InputPort)
    {
        GENET_DeleteUnit(base, unit);
        return NULL;
    }

    D(bug("[genet] unit ready\n");)

    return unit;
}

void GENET_DeleteUnit(struct GENETBase *base, struct GENETUnit *unit)
{
    BOOL alive;
    ULONG i;

    if (!unit)
        return;

    if (unit->gnu_Flags & IFF_UP)
        GENET_GoOffline(base, unit);

    unit->gnu_DeathWatch = FindTask(NULL);
    Forbid();
    alive = (unit->gnu_Task != NULL);
    if (alive)
        Signal(unit->gnu_Task, SIGBREAKF_CTRL_C);
    Permit();
    if (alive)
        Wait(SIGF_SINGLE);

    if (unit->gnu_IRQAdded)
        RemIntServer(INTB_KERNEL + unit->gnu_HW->irq,
                     &unit->gnu_IRQHandler);

    GENET_FlushUnit(base, unit, GENERAL_QUEUE, IOERR_ABORTED);

    for (i = 0; i < REQUEST_QUEUE_COUNT; i++)
    {
        if (unit->gnu_RequestPorts[i])
            FreeMem(unit->gnu_RequestPorts[i], sizeof(struct MsgPort));
    }

    /* Nothing may touch the buffers once the block has been silenced */
    genet_freebuffers(unit);

    while (!IsListEmpty((struct List *)&unit->gnu_MulticastRanges))
    {
        struct Node *n = RemHead((struct List *)&unit->gnu_MulticastRanges);

        FreeMem(n, sizeof(struct AddressRange));
    }

    while (!IsListEmpty((struct List *)&unit->gnu_TypeTrackers))
    {
        struct Node *n = RemHead((struct List *)&unit->gnu_TypeTrackers);

        FreeMem(n, sizeof(struct TypeTracker));
    }

    FreeMem(unit, sizeof(struct GENETUnit));
}
