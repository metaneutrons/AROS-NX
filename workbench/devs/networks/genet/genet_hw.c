/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: GENETv5 register, mdio and PHY access, and the reset sequence.

    The register sequences here are ported from FreeBSD's if_genet.c; see
    genet.h for the copyright notice that travels with them.
*/

#include <aros/debug.h>

#include <proto/exec.h>

#include "genet.h"

/*
 * Registers are reached through a plain volatile long. A struct laid
 * over the block would have to be packed to pin the offsets down, and
 * packing tells the compiler to assume nothing about alignment - on a
 * strict alignment target it then splits every access into bytes, which
 * a register does not accept.
 *
 * The block is mapped Device-nGnRnE, so accesses are neither reordered
 * nor merged and no explicit barrier is needed between them.
 */
ULONG GENET_Read(struct genet_hw *hw, ULONG reg)
{
    return *(volatile ULONG *)(hw->base + reg);
}

void GENET_Write(struct genet_hw *hw, ULONG reg, ULONG val)
{
    *(volatile ULONG *)(hw->base + reg) = val;
}

static void genet_clr(struct genet_hw *hw, ULONG reg, ULONG bits)
{
    GENET_Write(hw, reg, GENET_Read(hw, reg) & ~bits);
}

/*
 * A short spin, used where the reference driver waits ten microseconds.
 * Reading a register off a non-posted device mapping is itself slow
 * enough that the count only has to be a floor, not a measurement.
 */
static void genet_pause(struct genet_hw *hw)
{
    ULONG i;

    for (i = 0; i < 16; i++)
        (void)GENET_Read(hw, GENET_SYS_REV_CTRL);
}

/* == mdio =============================================================== */

/*
 * The command register is loaded, read back, and only then started. The
 * read back is not decorative: the write of the command and the write of
 * the start bit have to be separate transactions or the block may latch
 * a half-built command.
 */
LONG GENET_MDIORead(struct genet_hw *hw, ULONG phy, ULONG reg)
{
    ULONG val;
    ULONG retry;

    GENET_Write(hw, GENET_MDIO_CMD, GENET_MDIO_READ |
                ((phy & 0x1f) << GENET_MDIO_ADDR_SHIFT) |
                ((reg & 0x1f) << GENET_MDIO_REG_SHIFT));
    val = GENET_Read(hw, GENET_MDIO_CMD);
    GENET_Write(hw, GENET_MDIO_CMD, val | GENET_MDIO_START_BUSY);

    for (retry = GENET_MDIO_RETRIES; retry > 0; retry--)
    {
        val = GENET_Read(hw, GENET_MDIO_CMD);
        if ((val & GENET_MDIO_START_BUSY) == 0)
        {
            if (val & GENET_MDIO_READ_FAILED)
                return -1;
            return (LONG)(val & GENET_MDIO_VAL_MASK);
        }
    }

    D(bug("[genet] mdio read timed out, phy %u reg %u\n", phy, reg);)

    return -1;
}

BOOL GENET_MDIOWrite(struct genet_hw *hw, ULONG phy, ULONG reg, UWORD data)
{
    ULONG val;
    ULONG retry;

    GENET_Write(hw, GENET_MDIO_CMD, GENET_MDIO_WRITE |
                ((phy & 0x1f) << GENET_MDIO_ADDR_SHIFT) |
                ((reg & 0x1f) << GENET_MDIO_REG_SHIFT) |
                (data & GENET_MDIO_VAL_MASK));
    val = GENET_Read(hw, GENET_MDIO_CMD);
    GENET_Write(hw, GENET_MDIO_CMD, val | GENET_MDIO_START_BUSY);

    for (retry = GENET_MDIO_RETRIES; retry > 0; retry--)
    {
        if ((GENET_Read(hw, GENET_MDIO_CMD) & GENET_MDIO_START_BUSY) == 0)
            return TRUE;
    }

    D(bug("[genet] mdio write timed out, phy %u reg %u\n", phy, reg);)

    return FALSE;
}

/* == reset ============================================================== */

/*
 * Put the block back to a known state. The receive buffer is flushed
 * first, then the MAC is taken through its software reset, then the
 * statistics counters are cleared. Every register the driver cares about
 * is written afterwards, so nothing here has to preserve anything.
 */
void GENET_Reset(struct genet_hw *hw)
{
    ULONG val;

    val = GENET_Read(hw, GENET_SYS_RBUF_FLUSH_CTRL);
    GENET_Write(hw, GENET_SYS_RBUF_FLUSH_CTRL,
                val | GENET_SYS_RBUF_FLUSH_RESET);
    genet_pause(hw);

    GENET_Write(hw, GENET_SYS_RBUF_FLUSH_CTRL,
                val & ~GENET_SYS_RBUF_FLUSH_RESET);
    genet_pause(hw);

    GENET_Write(hw, GENET_SYS_RBUF_FLUSH_CTRL, 0);
    genet_pause(hw);

    GENET_Write(hw, GENET_UMAC_CMD, 0);
    GENET_Write(hw, GENET_UMAC_CMD,
                GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    genet_pause(hw);
    GENET_Write(hw, GENET_UMAC_CMD, 0);

    GENET_Write(hw, GENET_UMAC_MIB_CTRL, GENET_UMAC_MIB_RESET_RUNT |
                GENET_UMAC_MIB_RESET_RX | GENET_UMAC_MIB_RESET_TX);
    GENET_Write(hw, GENET_UMAC_MIB_CTRL, 0);
}

/* Stop both DMA engines and detach the default queue from its buffer */
void GENET_DMADisable(struct genet_hw *hw)
{
    genet_clr(hw, GENET_TX_DMA_CTRL, GENET_DMA_CTRL_EN |
              GENET_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE));
    genet_clr(hw, GENET_RX_DMA_CTRL, GENET_DMA_CTRL_EN |
              GENET_DMA_CTRL_RBUF_EN(GENET_DMA_DEFAULT_QUEUE));
}

/* == station address ==================================================== */

void GENET_SetMACAddress(struct genet_hw *hw, const UBYTE *addr)
{
    GENET_Write(hw, GENET_UMAC_MAC0,
                ((ULONG)addr[0] << 24) | ((ULONG)addr[1] << 16) |
                ((ULONG)addr[2] << 8) | addr[3]);
    GENET_Write(hw, GENET_UMAC_MAC1,
                ((ULONG)addr[4] << 8) | addr[5]);
}

/*
 * What the firmware left in the address registers, if it left anything.
 * A block still held in receive-buffer reset has not been set up by
 * anyone, so its registers say nothing.
 */
BOOL GENET_GetMACAddress(struct genet_hw *hw, UBYTE *addr)
{
    ULONG mac0, mac1;

    if (GENET_Read(hw, GENET_SYS_RBUF_FLUSH_CTRL) &
        GENET_SYS_RBUF_FLUSH_RESET)
        return FALSE;

    mac0 = GENET_Read(hw, GENET_UMAC_MAC0);
    mac1 = GENET_Read(hw, GENET_UMAC_MAC1) & 0xffff;

    if (mac0 == 0 && mac1 == 0)
        return FALSE;

    addr[0] = (mac0 >> 24) & 0xff;
    addr[1] = (mac0 >> 16) & 0xff;
    addr[2] = (mac0 >> 8) & 0xff;
    addr[3] = mac0 & 0xff;
    addr[4] = (mac1 >> 8) & 0xff;
    addr[5] = mac1 & 0xff;

    return TRUE;
}

/* == link ============================================================== */

/*
 * Tell the MAC what the PHY negotiated. Two registers are involved: the
 * RGMII out-of-band block, which has to be told there is a link and
 * whether the internal delay applies, and the command register, which
 * carries the line rate.
 *
 * Note what is not here. This map has no duplex bit that the reference
 * driver sets, and none is written; at gigabit the link is full duplex
 * in any case, and a half duplex 10/100 link is left to the PHY's own
 * collision handling. The duplex the caller passes is recorded for
 * reporting and otherwise unused.
 */
void GENET_SetSpeed(struct genet_hw *hw, ULONG mbps, BOOL fullduplex)
{
    ULONG val, speed;

    (void)fullduplex;

    switch (mbps)
    {
    case 1000:
        speed = GENET_UMAC_CMD_SPEED_1000;
        break;
    case 100:
        speed = GENET_UMAC_CMD_SPEED_100;
        break;
    default:
        speed = GENET_UMAC_CMD_SPEED_10;
        break;
    }

    val = GENET_Read(hw, GENET_EXT_RGMII_OOB_CTRL);
    val &= ~GENET_EXT_RGMII_OOB_OOB_DISABLE;
    val |= GENET_EXT_RGMII_OOB_RGMII_LINK;
    val |= GENET_EXT_RGMII_OOB_RGMII_MODE_EN;

    /*
     * Plain "rgmii" means the board provides the clock skew itself, so
     * the internal delay is switched off. Every -id variant wants it on.
     */
    if (hw->phyMode == GENET_PHY_MODE_RGMII)
        val |= GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;
    else
        val &= ~GENET_EXT_RGMII_OOB_ID_MODE_DISABLE;

    GENET_Write(hw, GENET_EXT_RGMII_OOB_CTRL, val);

    val = GENET_Read(hw, GENET_UMAC_CMD);
    val &= ~GENET_UMAC_CMD_SPEED;
    val |= speed;
    GENET_Write(hw, GENET_UMAC_CMD, val);
}

/* == phy =============================================================== */

/*
 * Advertise everything the part can do and start negotiating. Whether
 * the link comes up is asked separately, once a second, by the unit
 * task - a gigabit negotiation takes longer than it is reasonable to
 * block a device open for.
 */
BOOL GENET_PHYInit(struct genet_hw *hw)
{
    ULONG phy = hw->phyAddr;
    LONG bmsr;
    UWORD anar;

    bmsr = GENET_MDIORead(hw, phy, MII_BMSR);
    if (bmsr < 0)
        return FALSE;

    anar = ANAR_CSMA | ANAR_10HD | ANAR_10FD | ANAR_100HD | ANAR_100FD;
    if (!GENET_MDIOWrite(hw, phy, MII_ANAR, anar))
        return FALSE;

    /* Gigabit is advertised in its own register, when the part has one */
    if (bmsr & BMSR_ESTATEN)
        GENET_MDIOWrite(hw, phy, MII_GBCR, GBCR_1000FD | GBCR_1000HD);

    return GENET_MDIOWrite(hw, phy, MII_BMCR,
                           BMCR_ANENABLE | BMCR_ANRESTART);
}

/*
 * The current link state. Speed comes from what both ends advertised,
 * which is where a clause-22 PHY keeps the negotiated result.
 */
BOOL GENET_PHYGetLink(struct genet_hw *hw, ULONG *mbps, BOOL *fullduplex)
{
    ULONG phy = hw->phyAddr;
    LONG bmsr, anar, anlpar, gbcr, gbsr, common;

    bmsr = GENET_MDIORead(hw, phy, MII_BMSR);
    if (bmsr < 0)
        return FALSE;

    /*
     * Link status latches low, so a single read can report a link that
     * has since come back. Read it twice and believe the second.
     */
    bmsr = GENET_MDIORead(hw, phy, MII_BMSR);
    if (bmsr < 0 || !(bmsr & BMSR_LSTATUS))
        return FALSE;

    if (!(bmsr & BMSR_ANEGCOMPLETE))
        return FALSE;

    gbcr = GENET_MDIORead(hw, phy, MII_GBCR);
    gbsr = GENET_MDIORead(hw, phy, MII_GBSR);

    if (gbcr >= 0 && gbsr >= 0)
    {
        if ((gbcr & GBCR_1000FD) && (gbsr & GBSR_LP1000FD))
        {
            *mbps = 1000;
            *fullduplex = TRUE;
            return TRUE;
        }
        if ((gbcr & GBCR_1000HD) && (gbsr & GBSR_LP1000HD))
        {
            *mbps = 1000;
            *fullduplex = FALSE;
            return TRUE;
        }
    }

    anar = GENET_MDIORead(hw, phy, MII_ANAR);
    anlpar = GENET_MDIORead(hw, phy, MII_ANLPAR);
    if (anar < 0 || anlpar < 0)
        return FALSE;

    common = anar & anlpar;

    if (common & ANAR_100FD)
    {
        *mbps = 100;
        *fullduplex = TRUE;
    }
    else if (common & ANAR_100HD)
    {
        *mbps = 100;
        *fullduplex = FALSE;
    }
    else if (common & ANAR_10FD)
    {
        *mbps = 10;
        *fullduplex = TRUE;
    }
    else if (common & ANAR_10HD)
    {
        *mbps = 10;
        *fullduplex = FALSE;
    }
    else
        return FALSE;

    return TRUE;
}
