/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Broadcom GENETv5 SANA-II driver, private definitions.

    The register map and the bring-up sequences in this driver are ported
    from FreeBSD's sys/arm64/broadcom/genet/if_genet.c and if_genetreg.h,
    which derive in turn from NetBSD's bcmgenet.c. Those files are covered
    by the following notice, which condition 1 of their licence requires be
    kept with the code:

        Copyright (c) 2020 Michael J Karels
        Copyright (c) 2016, 2020 Jared McNeill <jmcneill@invisible.ca>

        Redistribution and use in source and binary forms, with or without
        modification, are permitted provided that the following conditions
        are met:
        1. Redistributions of source code must retain the above copyright
           notice, this list of conditions and the following disclaimer.
        2. Redistributions in binary form must reproduce the above copyright
           notice, this list of conditions and the following disclaimer in
           the documentation and/or other materials provided with the
           distribution.

        THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
        IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
        WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
        ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
        DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
        DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
        GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
        INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
        IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
        OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
        IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    No part of this driver derives from U-Boot's drivers/net/bcmgenet.c or
    from Linux's bcmgenet, both of which are GPL licensed and neither of
    which was consulted.
*/

#ifndef GENET_H
#define GENET_H

#include <exec/types.h>
#include <exec/devices.h>
#include <exec/semaphores.h>
#include <exec/interrupts.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <devices/sana2.h>
#include <devices/sana2specialstats.h>

/*
 * ==========================================================================
 * Register map, GENETv5 as found on the BCM2711
 * ==========================================================================
 *
 * Two things about this block are unusual enough to state up front.
 *
 * First, the DMA descriptors are not in memory: they are registers, 256 of
 * them per direction at GENET_RX_BASE and GENET_TX_BASE, three longs each.
 * Nothing about the descriptor rings needs cache maintenance or a physical
 * address, because there is no descriptor memory to be coherent about. Only
 * the packet buffers the descriptors point at live in RAM.
 *
 * Second, the queue this driver uses is queue 16, not queue 0. The block has
 * seventeen transmit and receive queues; sixteen of them are priority queues
 * and the seventeenth, index 16, is the default one that unclassified traffic
 * lands in. Driving queue 0 instead looks reasonable and receives nothing.
 */

#define GENET_SYS_REV_CTRL              0x000
#define  GENET_REV_MAJOR                0x0f000000
#define  GENET_REV_MAJOR_SHIFT          24
#define  GENET_REV_MAJOR_V5             6
#define  GENET_REV_MINOR                0x000f0000
#define  GENET_REV_MINOR_SHIFT          16
#define  GENET_REV_PHY                  0x0000ffff

#define GENET_SYS_PORT_CTRL             0x004
#define  GENET_SYS_PORT_MODE_EXT_GPHY   3

#define GENET_SYS_RBUF_FLUSH_CTRL       0x008
#define  GENET_SYS_RBUF_FLUSH_RESET     (1 << 1)
#define GENET_SYS_TBUF_FLUSH_CTRL       0x00c

#define GENET_EXT_RGMII_OOB_CTRL        0x08c
#define  GENET_EXT_RGMII_OOB_ID_MODE_DISABLE (1 << 16)
#define  GENET_EXT_RGMII_OOB_RGMII_MODE_EN   (1 << 6)
#define  GENET_EXT_RGMII_OOB_OOB_DISABLE     (1 << 5)
#define  GENET_EXT_RGMII_OOB_RGMII_LINK      (1 << 4)

/* Level 2 interrupt controller, the one the CPU sees */
#define GENET_INTRL2_CPU_STAT           0x200
#define GENET_INTRL2_CPU_CLEAR          0x208
#define GENET_INTRL2_CPU_STAT_MASK      0x20c
#define GENET_INTRL2_CPU_SET_MASK       0x210
#define GENET_INTRL2_CPU_CLEAR_MASK     0x214
#define  GENET_IRQ_MDIO_ERROR           (1 << 24)
#define  GENET_IRQ_MDIO_DONE            (1 << 23)
#define  GENET_IRQ_TXDMA_DONE           (1 << 16)
#define  GENET_IRQ_RXDMA_DONE           (1 << 13)

#define GENET_RBUF_CTRL                 0x300
#define  GENET_RBUF_BAD_DIS             (1 << 2)
#define  GENET_RBUF_ALIGN_2B            (1 << 1)
#define  GENET_RBUF_64B_EN              (1 << 0)
#define GENET_RBUF_CHECK_CTRL           0x314
#define  GENET_RBUF_CHECK_CTRL_EN       (1 << 0)
#define GENET_RBUF_TBUF_SIZE_CTRL       0x3b4

#define GENET_TBUF_CTRL                 0x600

#define GENET_UMAC_CMD                  0x808
#define  GENET_UMAC_CMD_LCL_LOOP_EN     (1 << 15)
#define  GENET_UMAC_CMD_SW_RESET        (1 << 13)
#define  GENET_UMAC_CMD_CRC_FWD         (1 << 6)
#define  GENET_UMAC_CMD_PROMISC         (1 << 4)
#define  GENET_UMAC_CMD_SPEED           (3 << 2)
#define   GENET_UMAC_CMD_SPEED_10       (0 << 2)
#define   GENET_UMAC_CMD_SPEED_100      (1 << 2)
#define   GENET_UMAC_CMD_SPEED_1000     (2 << 2)
#define  GENET_UMAC_CMD_RXEN            (1 << 1)
#define  GENET_UMAC_CMD_TXEN            (1 << 0)

#define GENET_UMAC_MAC0                 0x80c
#define GENET_UMAC_MAC1                 0x810
#define GENET_UMAC_MAX_FRAME_LEN        0x814
#define GENET_UMAC_TX_FLUSH             0xb34

#define GENET_UMAC_MIB_CTRL             0xd80
#define  GENET_UMAC_MIB_RESET_TX        (1 << 2)
#define  GENET_UMAC_MIB_RESET_RUNT      (1 << 1)
#define  GENET_UMAC_MIB_RESET_RX        (1 << 0)

#define GENET_MDIO_CMD                  0xe14
#define  GENET_MDIO_START_BUSY          (1UL << 29)
#define  GENET_MDIO_READ_FAILED         (1 << 28)
#define  GENET_MDIO_READ                (1 << 27)
#define  GENET_MDIO_WRITE               (1 << 26)
#define  GENET_MDIO_ADDR_SHIFT          21
#define  GENET_MDIO_REG_SHIFT           16
#define  GENET_MDIO_VAL_MASK            0xffff

/*
 * The perfect-match filter. Seventeen slots, and the control register
 * enables them from the top down: slot 0 is the highest bit.
 */
#define GENET_UMAC_MDF_CTRL             0xe50
#define GENET_UMAC_MDF_ADDR0(n)         (0xe54 + (n) * 0x8)
#define GENET_UMAC_MDF_ADDR1(n)         (0xe58 + (n) * 0x8)
#define GENET_MAX_MDF_FILTER            17

/* Ring geometry. The descriptor count is fixed by the register layout. */
#define GENET_DMA_DESC_COUNT            256
#define GENET_DMA_DESC_SIZE             12
#define GENET_DMA_DEFAULT_QUEUE         16

#define GENET_RX_BASE                   0x2000
#define GENET_TX_BASE                   0x4000

#define GENET_DMA_RING_SIZE             0x40

#define GENET_RX_DMA_RINGBASE(qid) \
    (GENET_RX_BASE + 0xc00 + GENET_DMA_RING_SIZE * (qid))
#define GENET_RX_DMA_WRITE_PTR_LO(qid)  (GENET_RX_DMA_RINGBASE(qid) + 0x00)
#define GENET_RX_DMA_WRITE_PTR_HI(qid)  (GENET_RX_DMA_RINGBASE(qid) + 0x04)
#define GENET_RX_DMA_PROD_INDEX(qid)    (GENET_RX_DMA_RINGBASE(qid) + 0x08)
#define GENET_RX_DMA_CONS_INDEX(qid)    (GENET_RX_DMA_RINGBASE(qid) + 0x0c)
#define GENET_RX_DMA_RING_BUF_SIZE(qid) (GENET_RX_DMA_RINGBASE(qid) + 0x10)
#define GENET_RX_DMA_START_ADDR_LO(qid) (GENET_RX_DMA_RINGBASE(qid) + 0x14)
#define GENET_RX_DMA_START_ADDR_HI(qid) (GENET_RX_DMA_RINGBASE(qid) + 0x18)
#define GENET_RX_DMA_END_ADDR_LO(qid)   (GENET_RX_DMA_RINGBASE(qid) + 0x1c)
#define GENET_RX_DMA_END_ADDR_HI(qid)   (GENET_RX_DMA_RINGBASE(qid) + 0x20)
#define GENET_RX_DMA_XON_XOFF_THRES(qid) (GENET_RX_DMA_RINGBASE(qid) + 0x28)
#define GENET_RX_DMA_READ_PTR_LO(qid)   (GENET_RX_DMA_RINGBASE(qid) + 0x2c)
#define GENET_RX_DMA_READ_PTR_HI(qid)   (GENET_RX_DMA_RINGBASE(qid) + 0x30)

#define GENET_TX_DMA_RINGBASE(qid) \
    (GENET_TX_BASE + 0xc00 + GENET_DMA_RING_SIZE * (qid))
#define GENET_TX_DMA_READ_PTR_LO(qid)   (GENET_TX_DMA_RINGBASE(qid) + 0x00)
#define GENET_TX_DMA_READ_PTR_HI(qid)   (GENET_TX_DMA_RINGBASE(qid) + 0x04)
#define GENET_TX_DMA_CONS_INDEX(qid)    (GENET_TX_DMA_RINGBASE(qid) + 0x08)
#define GENET_TX_DMA_PROD_INDEX(qid)    (GENET_TX_DMA_RINGBASE(qid) + 0x0c)
#define GENET_TX_DMA_RING_BUF_SIZE(qid) (GENET_TX_DMA_RINGBASE(qid) + 0x10)
#define GENET_TX_DMA_START_ADDR_LO(qid) (GENET_TX_DMA_RINGBASE(qid) + 0x14)
#define GENET_TX_DMA_START_ADDR_HI(qid) (GENET_TX_DMA_RINGBASE(qid) + 0x18)
#define GENET_TX_DMA_END_ADDR_LO(qid)   (GENET_TX_DMA_RINGBASE(qid) + 0x1c)
#define GENET_TX_DMA_END_ADDR_HI(qid)   (GENET_TX_DMA_RINGBASE(qid) + 0x20)
#define GENET_TX_DMA_MBUF_DONE_THRES(qid) (GENET_TX_DMA_RINGBASE(qid) + 0x24)
#define GENET_TX_DMA_FLOW_PERIOD(qid)   (GENET_TX_DMA_RINGBASE(qid) + 0x28)
#define GENET_TX_DMA_WRITE_PTR_LO(qid)  (GENET_TX_DMA_RINGBASE(qid) + 0x2c)
#define GENET_TX_DMA_WRITE_PTR_HI(qid)  (GENET_TX_DMA_RINGBASE(qid) + 0x30)

/* Producer and consumer indices are 16-bit and free-running */
#define GENET_DMA_PROD_CONS_MASK        0xffff

#define GENET_RX_DMA_RING_BUF_SIZE_DESC_SHIFT   16
#define GENET_RX_DMA_RING_BUF_SIZE_BUF_LEN_MASK 0xffff
#define GENET_TX_DMA_RING_BUF_SIZE_DESC_SHIFT   16
#define GENET_TX_DMA_RING_BUF_SIZE_BUF_LEN_MASK 0xffff
#define GENET_RX_DMA_XON_XOFF_THRES_LO_SHIFT    16

/* The descriptors themselves, addressed by index */
#define GENET_RX_DESC_STATUS(idx) \
    (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x00)
#define GENET_RX_DESC_ADDRESS_LO(idx) \
    (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x04)
#define GENET_RX_DESC_ADDRESS_HI(idx) \
    (GENET_RX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x08)

#define  GENET_RX_DESC_STATUS_BUFLEN_MASK  0x0fff0000
#define  GENET_RX_DESC_STATUS_BUFLEN_SHIFT 16
#define  GENET_RX_DESC_STATUS_CKSUM_OK     (1 << 15)
#define  GENET_RX_DESC_STATUS_EOP          (1 << 14)
#define  GENET_RX_DESC_STATUS_SOP          (1 << 13)
#define  GENET_RX_DESC_STATUS_RX_ERROR     (1 << 2)

#define GENET_TX_DESC_STATUS(idx) \
    (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x00)
#define GENET_TX_DESC_ADDRESS_LO(idx) \
    (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x04)
#define GENET_TX_DESC_ADDRESS_HI(idx) \
    (GENET_TX_BASE + GENET_DMA_DESC_SIZE * (idx) + 0x08)

#define  GENET_TX_DESC_STATUS_BUFLEN_SHIFT 16
#define  GENET_TX_DESC_STATUS_EOP          (1 << 14)
#define  GENET_TX_DESC_STATUS_SOP          (1 << 13)
#define  GENET_TX_DESC_STATUS_QTAG_MASK    0x1f80
#define  GENET_TX_DESC_STATUS_CRC          (1 << 6)
#define  GENET_TX_DESC_STATUS_CKSUM        (1 << 4)

/* Ring configuration and the DMA enables */
#define GENET_RX_DMA_RING_CFG           (GENET_RX_BASE + 0x1040 + 0x00)
#define GENET_RX_DMA_CTRL               (GENET_RX_BASE + 0x1040 + 0x04)
#define GENET_RX_SCB_BURST_SIZE         (GENET_RX_BASE + 0x1040 + 0x0c)
#define GENET_TX_DMA_RING_CFG           (GENET_TX_BASE + 0x1040 + 0x00)
#define GENET_TX_DMA_CTRL               (GENET_TX_BASE + 0x1040 + 0x04)
#define GENET_TX_SCB_BURST_SIZE         (GENET_TX_BASE + 0x1040 + 0x0c)

#define GENET_DMA_CTRL_EN               (1 << 0)
#define GENET_DMA_CTRL_RBUF_EN(qid)     (1UL << ((qid) + 1))

#define GENET_DMA_MAX_BURST_LENGTH      0x08

/*
 * ==========================================================================
 * Standard clause-22 MDIO registers
 * ==========================================================================
 * The RPi4 carries a BCM54213PE, which answers the generic set; nothing
 * here needs a vendor register.
 */
#define MII_BMCR                        0x00
#define MII_BMSR                        0x01
#define MII_PHYSID1                     0x02
#define MII_PHYSID2                     0x03
#define MII_ANAR                        0x04
#define MII_ANLPAR                      0x05
#define MII_GBCR                        0x09
#define MII_GBSR                        0x0a

#define BMCR_RESET                      0x8000
#define BMCR_ANENABLE                   0x1000
#define BMCR_ANRESTART                  0x0200

#define BMSR_LSTATUS                    0x0004
#define BMSR_ANEGCOMPLETE               0x0020
#define BMSR_ESTATEN                    0x0100

#define ANAR_CSMA                       0x0001
#define ANAR_10HD                       0x0020
#define ANAR_10FD                       0x0040
#define ANAR_100HD                      0x0080
#define ANAR_100FD                      0x0100

#define GBCR_1000HD                     0x0100
#define GBCR_1000FD                     0x0200
#define GBSR_LP1000HD                   0x0400
#define GBSR_LP1000FD                   0x0800

/* How long to wait for the mdio bus, in polling rounds */
#define GENET_MDIO_RETRIES              10000

/*
 * ==========================================================================
 * Ethernet on the wire
 * ==========================================================================
 */
#define ETH_ADDRESSSIZE                 6
#define ETH_HEADERSIZE                  14
#define ETH_MTU                         1500
#define ETH_MAXPACKETSIZE               (ETH_HEADERSIZE + ETH_MTU)
#define ETH_ZLEN                        60

/* Frame check sequence, which the receiver leaves on the frame */
#define ETH_FCSLEN                      4

/*
 * With GENET_RBUF_ALIGN_2B set, the receiver puts two pad bytes in front
 * of every frame so the IP header lands on a four-byte boundary. The
 * length the descriptor reports counts them.
 */
#define GENET_RX_PAD                    2

/*
 * One buffer holds one whole frame. 2048 is what the ring size register
 * is told and what the reference driver uses; the frame, its pad and its
 * FCS all fit with room to spare.
 */
#define GENET_BUFSIZE                   2048

struct eth_frame
{
    UBYTE               eth_packet_dest[ETH_ADDRESSSIZE];
    UBYTE               eth_packet_source[ETH_ADDRESSSIZE];
    UWORD               eth_packet_type;
    UBYTE               eth_packet_data[];
};

/*
 * ==========================================================================
 * What the device tree told us about the controller
 * ==========================================================================
 */

/* phy-mode, which decides the RGMII delay bits */
enum genet_phymode
{
    GENET_PHY_MODE_RGMII,       /* no internal delay either way */
    GENET_PHY_MODE_RGMII_ID,    /* delay on both               */
    GENET_PHY_MODE_RGMII_RXID,  /* delay on receive - the Pi 4  */
    GENET_PHY_MODE_RGMII_TXID   /* delay on transmit            */
};

struct genet_hw
{
    IPTR                base;           /* register block, as mapped        */
    IPTR                phys;           /* and where it really lives        */
    IPTR                size;
    ULONG               irq;            /* GIC interrupt id, ring queue     */
    ULONG               irq2;           /* GIC interrupt id, PHY events     */
    ULONG               phyAddr;        /* address on the mdio bus          */
    enum genet_phymode  phyMode;
    UBYTE               macAddr[ETH_ADDRESSSIZE];
    BOOL                haveMacAddr;
};

/*
 * ==========================================================================
 * SANA-II bookkeeping
 * ==========================================================================
 */

/* Deferred request queues, indices into gnu_RequestPorts */
enum
{
    WRITE_QUEUE,
    ADOPT_QUEUE,
    EVENT_QUEUE,
    GENERAL_QUEUE,
    REQUEST_QUEUE_COUNT
};

struct Opener
{
    struct MinNode      node;
    struct MsgPort      read_port;
    BOOL                (*rx_function)(APTR, APTR, ULONG);
    BOOL                (*tx_function)(APTR, APTR, ULONG);
    struct Hook        *filter_hook;
    struct MinList      initial_stats;
};

struct TypeStats
{
    struct MinNode      node;
    ULONG               packet_type;
    struct Sana2PacketTypeStats stats;
};

struct TypeTracker
{
    struct MinNode      node;
    ULONG               packet_type;
    struct Sana2PacketTypeStats stats;
    ULONG               user_count;
};

struct AddressRange
{
    struct MinNode      node;
    ULONG               add_count;
    ULONG               lower_bound_left;
    ULONG               upper_bound_left;
    UWORD               lower_bound_right;
    UWORD               upper_bound_right;
};

#define STAT_COUNT                      3

/* gnu_Flags */
#define IFF_UP                          0x0001
#define IFF_CONFIGURED                  0x0002
#define IFF_PROMISC                     0x0004

struct GENETUnit
{
    struct GENETBase   *gnu_Base;
    struct genet_hw    *gnu_HW;

    struct SignalSemaphore gnu_Lock;
    struct MinList      gnu_Openers;
    struct MinList      gnu_MulticastRanges;
    struct MinList      gnu_TypeTrackers;
    LONG                gnu_RangeCount;
    ULONG               gnu_OpenCount;
    ULONG               gnu_Flags;

    struct MsgPort     *gnu_InputPort;
    struct MsgPort     *gnu_RequestPorts[REQUEST_QUEUE_COUNT];
    struct Task        *gnu_Task;
    struct Task        *gnu_DeathWatch;

    struct Interrupt    gnu_IRQHandler;
    struct Interrupt    gnu_TXInt;
    BOOL                gnu_IRQAdded;

    UBYTE               gnu_DevAddr[ETH_ADDRESSSIZE];
    UBYTE               gnu_OrgAddr[ETH_ADDRESSSIZE];

    ULONG               gnu_SpeedMbps;
    BOOL                gnu_FullDuplex;
    BOOL                gnu_LinkUp;

    struct Sana2DeviceQuery gnu_Sana2Info;
    struct Sana2DeviceStats gnu_Stats;
    ULONG               gnu_SpecialStats[STAT_COUNT];

    /*
     * Packet buffers. One allocation, mapped uncached - see genet_unit.c
     * for why that is necessary on this SoC rather than merely tidy.
     */
    APTR                gnu_BufMem;         /* as allocated, for freeing   */
    ULONG               gnu_BufMemSize;
    APTR                gnu_BufBase;        /* page aligned, uncached      */
    ULONG               gnu_BufMapSize;
    UBYTE              *gnu_TXBuf;
    UBYTE              *gnu_RXBuf;

    /*
     * Ring state. The hardware counts in free-running 16-bit producer and
     * consumer indices; the descriptor a given index refers to is the
     * index modulo the ring length.
     *
     * Each of these has exactly one writer - gnu_TXProd the transmit soft
     * interrupt, gnu_TXCons and gnu_RXCons the hardware interrupt - so
     * the other side may read it without a lock and the difference
     * between two of them is always a sound answer.
     */
    ULONG               gnu_TXProd;         /* what we have handed over    */
    ULONG               gnu_TXCons;         /* what it has finished        */
    ULONG               gnu_RXCons;         /* what we have taken back     */
};

struct GENETBase
{
    struct Device       gnb_Device;
    APTR                gnb_KernelBase;
    APTR                gnb_UtilityBase;
    struct genet_hw     gnb_HW;
    BOOL                gnb_Found;
    struct GENETUnit   *gnb_Unit;
    struct SignalSemaphore gnb_UnitSem;     /* serialises unit creation   */
};

/* genet_dt.c - everything that knows how the platform describes hardware */
BOOL GENET_Discover(struct GENETBase *base, struct genet_hw *hw);

/* genet_hw.c */
ULONG GENET_Read(struct genet_hw *hw, ULONG reg);
void GENET_Write(struct genet_hw *hw, ULONG reg, ULONG val);
LONG GENET_MDIORead(struct genet_hw *hw, ULONG phy, ULONG reg);
BOOL GENET_MDIOWrite(struct genet_hw *hw, ULONG phy, ULONG reg, UWORD val);
void GENET_Reset(struct genet_hw *hw);
void GENET_DMADisable(struct genet_hw *hw);
void GENET_SetMACAddress(struct genet_hw *hw, const UBYTE *addr);
BOOL GENET_GetMACAddress(struct genet_hw *hw, UBYTE *addr);
void GENET_SetSpeed(struct genet_hw *hw, ULONG mbps, BOOL fullduplex);
BOOL GENET_PHYInit(struct genet_hw *hw);
BOOL GENET_PHYGetLink(struct genet_hw *hw, ULONG *mbps, BOOL *fullduplex);

/* genet_unit.c */
struct GENETUnit *GENET_CreateUnit(struct GENETBase *base);
void GENET_DeleteUnit(struct GENETBase *base, struct GENETUnit *unit);
void GENET_FlushUnit(struct GENETBase *base, struct GENETUnit *unit,
                     UBYTE last_queue, BYTE error);
void GENET_GoOnline(struct GENETBase *base, struct GENETUnit *unit);
void GENET_GoOffline(struct GENETBase *base, struct GENETUnit *unit);
void GENET_SetupRXFilter(struct GENETBase *base, struct GENETUnit *unit);
BOOL GENET_AddressFilter(struct GENETBase *base, struct GENETUnit *unit,
                         UBYTE *address);
void GENET_CopyPacket(struct GENETBase *base, struct GENETUnit *unit,
                      struct IOSana2Req *request, ULONG packet_size,
                      UWORD packet_type, struct eth_frame *frame);
void GENET_ReportEvents(struct GENETBase *base, struct GENETUnit *unit,
                        ULONG events);
struct TypeStats *GENET_FindTypeStats(struct MinList *list, ULONG packet_type);
BOOL GENET_AddMulticastRange(struct GENETBase *base, struct GENETUnit *unit,
                             const UBYTE *lower, const UBYTE *upper);
BOOL GENET_RemMulticastRange(struct GENETBase *base, struct GENETUnit *unit,
                             const UBYTE *lower, const UBYTE *upper);

/* genet_handler.c */
void GENET_HandleRequest(struct GENETBase *base, struct IOSana2Req *request);

#endif /* GENET_H */
