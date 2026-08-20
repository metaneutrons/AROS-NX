/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Finding a GENET controller on a machine described by a device tree.

    Everything that knows how this platform describes its hardware lives
    here, so the rest of the driver never sees a device tree at all.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/utility.h>

#include <aros/kernel.h>
#include <utility/tagitem.h>

#include <fdt.h>

#include "genet.h"

/*
 * The v5 core is the only one this driver has the register map for. The
 * generic name is listed as well as the Pi 4's specific one, so a board
 * whose tree names only the generation is still recognised.
 */
static CONST_STRPTR const genet_compatible[] =
{
    "brcm,bcm2711-genet-v5",
    "brcm,genet-v5",
    NULL
};

/*
 * The GIC's interrupt specifiers are three cells: kind, number, flags.
 * A number is stated relative to its kind, while the controller and
 * AROS both count in raw interrupt ids, so the base of the kind has to
 * be added back on.
 */
#define DT_IRQ_KIND_SPI     0
#define DT_IRQ_KIND_PPI     1
#define GIC_FIRST_PPI       16
#define GIC_FIRST_SPI       32

static fdt_node_t genet_findphandle(ULONG phandle)
{
    fdt_node_t n;

    if (!phandle)
        return FDT_NONE;

    for (n = FDT_Root(); n != FDT_NONE; n = FDT_NextNode(n))
    {
        if (FDT_GetPropU32(n, "phandle", 0) == phandle)
            return n;
    }
    return FDT_NONE;
}

/*
 * Translate an address out of a child's "reg" into one the CPU can use.
 *
 * A "reg" is stated in the address space of the bus the node hangs off,
 * and each bus on the way to the root may remap what its children say
 * through a "ranges" property. On the BCM2711 that matters: the
 * controller's own tree entry reads 0x7d580000, which is where the block
 * sits as seen from inside the SoC, while the CPU reaches it at
 * 0xfd580000. Using the untranslated address gets a driver nothing but
 * a bus fault, or worse, silence.
 *
 * A bus with no "ranges" does not translate and stops the walk; an empty
 * "ranges" is an identity mapping and carries on.
 */
static UQUAD genet_translate(fdt_node_t node, UQUAD addr)
{
    fdt_node_t child = node;
    ULONG hops = 0;

    /* A malformed tree must not be able to spin this forever */
    while (hops++ < 16)
    {
        fdt_node_t bus = FDT_Parent(child);
        const ULONG *ranges;
        ULONG len = 0, child_ac, parent_ac, child_sc, stride, i;
        BOOL matched = FALSE;

        if (bus == FDT_NONE)
            break;

        ranges = FDT_GetProp(bus, "ranges", &len);
        if (!ranges)
        {
            D(bug("[genet] %s does not translate, stopping at %p\n",
                  FDT_NodeName(bus), (APTR)(IPTR)addr);)
            break;
        }

        /* Present but empty: the bus passes addresses through unchanged */
        if (len == 0)
        {
            child = bus;
            continue;
        }

        child_ac = FDT_GetPropU32(bus, "#address-cells", 2);
        child_sc = FDT_GetPropU32(bus, "#size-cells", 1);
        parent_ac = FDT_ParentAddressCells(bus);
        stride = child_ac + parent_ac + child_sc;

        if (!stride || !child_ac)
            break;

        for (i = 0; (i + 1) * stride * 4 <= len; i++)
        {
            const ULONG *e = &ranges[i * stride];
            UQUAD cbase = FDT_ReadCells(e, child_ac);
            UQUAD pbase = FDT_ReadCells(&e[child_ac], parent_ac);
            UQUAD size = child_sc ? FDT_ReadCells(&e[child_ac + parent_ac],
                                                 child_sc) : 0;

            if (addr >= cbase && (!size || addr < cbase + size))
            {
                addr = addr - cbase + pbase;
                matched = TRUE;
                break;
            }
        }

        if (!matched)
        {
            D(bug("[genet] no range on %s covers %p\n",
                  FDT_NodeName(bus), (APTR)(IPTR)addr);)
            break;
        }

        child = bus;
    }

    return addr;
}

/*
 * Read one interrupt specifier and turn it into the id the controller
 * knows it by. The cell count comes from the interrupt controller when
 * the tree names one, and is otherwise inferred from how much the
 * property holds - a three cell specifier is a GIC one.
 */
static ULONG genet_interrupt(fdt_node_t node, ULONG index)
{
    const ULONG *prop;
    ULONG len = 0, cells = 0, parent;
    fdt_node_t ic;

    prop = FDT_GetProp(node, "interrupts", &len);
    if (!prop)
        return 0;

    /*
     * "interrupt-parent" is inherited, so a node that does not name one
     * uses its bus's, and most do not name one. Walk up until something
     * does.
     */
    {
        fdt_node_t n = node;
        ULONG hops = 0;

        parent = 0;
        while (n != FDT_NONE && hops++ < 16)
        {
            parent = FDT_GetPropU32(n, "interrupt-parent", 0);
            if (parent)
                break;
            n = FDT_Parent(n);
        }
    }

    ic = genet_findphandle(parent);
    if (ic != FDT_NONE)
        cells = FDT_GetPropU32(ic, "#interrupt-cells", 0);

    /*
     * Nothing to ask: a specifier of three cells is a GIC one, which is
     * what this controller is always wired to.
     */
    if (!cells)
        cells = ((len % 12) == 0) ? 3 : 1;

    if ((index + 1) * cells * 4 > len)
        return 0;

    if (cells < 3)
        return (ULONG)FDT_ReadCells(&prop[index * cells], 1);

    {
        ULONG kind = (ULONG)FDT_ReadCells(&prop[index * cells], 1);
        ULONG number = (ULONG)FDT_ReadCells(&prop[index * cells + 1], 1);

        if (kind == DT_IRQ_KIND_PPI)
            return number + GIC_FIRST_PPI;
        if (kind == DT_IRQ_KIND_SPI)
            return number + GIC_FIRST_SPI;

        D(bug("[genet] interrupt kind %u is not one this knows\n", kind);)
        return 0;
    }
}

/*
 * The PHY's address on the mdio bus. "phy-handle" points at the node
 * describing it, and that node's "reg" is the address. Failing that, an
 * mdio child with a "reg" is looked for directly.
 */
static BOOL genet_phyaddress(fdt_node_t node, ULONG *addr)
{
    fdt_node_t phy = genet_findphandle(FDT_GetPropU32(node, "phy-handle", 0));
    UQUAD reg = 0;

    if (phy != FDT_NONE && FDT_GetReg(phy, 0, &reg, NULL))
    {
        *addr = (ULONG)reg;
        return TRUE;
    }

    {
        fdt_node_t n;
        BOOL inmdio = FALSE;

        for (n = FDT_NextNode(node); n != FDT_NONE; n = FDT_NextNode(n))
        {
            CONST_STRPTR name = FDT_NodeName(n);

            if (name && name[0] == 'm' && name[1] == 'd' && name[2] == 'i' &&
                name[3] == 'o')
            {
                inmdio = TRUE;
                continue;
            }

            if (inmdio && FDT_GetReg(n, 0, &reg, NULL))
            {
                *addr = (ULONG)reg;
                return TRUE;
            }

            if (inmdio)
                break;
        }
    }

    return FALSE;
}

static enum genet_phymode genet_mode(fdt_node_t node)
{
    CONST_STRPTR mode = FDT_GetPropStr(node, "phy-mode");

    if (!mode)
        mode = FDT_GetPropStr(node, "phy-connection-type");

    if (mode)
    {
        /* "rgmii", "rgmii-id", "rgmii-rxid" or "rgmii-txid" */
        if (mode[0] == 'r' && mode[1] == 'g' && mode[2] == 'm' &&
            mode[3] == 'i' && mode[4] == 'i')
        {
            if (mode[5] == '\0')
                return GENET_PHY_MODE_RGMII;
            if (mode[5] == '-')
            {
                if (mode[6] == 'i')
                    return GENET_PHY_MODE_RGMII_ID;
                if (mode[6] == 'r')
                    return GENET_PHY_MODE_RGMII_RXID;
                if (mode[6] == 't')
                    return GENET_PHY_MODE_RGMII_TXID;
            }
        }

        D(bug("[genet] phy-mode '%s' is not an rgmii one\n", mode);)
    }

    /*
     * What the Pi 4 says, and the only wiring this block is used with on
     * the boards it appears on.
     */
    return GENET_PHY_MODE_RGMII_RXID;
}

static BOOL genet_enabled(fdt_node_t node)
{
    CONST_STRPTR status = FDT_GetPropStr(node, "status");

    /* Absent means usable; anything but "ok"/"okay" means it is not */
    if (!status)
        return TRUE;

    return (status[0] == 'o' && status[1] == 'k');
}

#if DEBUG
static CONST_STRPTR const genet_modenames[] =
    { "rgmii", "rgmii-id", "rgmii-rxid", "rgmii-txid" };
#endif

#undef UtilityBase

BOOL GENET_Discover(struct GENETBase *base, struct genet_hw *hw)
{
    APTR KernelBase = base->gnb_KernelBase;
    struct Library *UtilityBase = base->gnb_UtilityBase;
    fdt_node_t node = FDT_NONE;
    UQUAD addr = 0, size = 0;
    const UBYTE *mac;
    ULONG len = 0, i;
    APTR dtb, bootinfo;

    bootinfo = KrnGetBootInfo();
    dtb = (APTR)GetTagData(KRN_FlattenedDeviceTree, 0,
                           (struct TagItem *)bootinfo);
    if (!dtb || !FDT_Open(dtb))
    {
        D(bug("[genet] no device tree to search\n");)
        return FALSE;
    }

    for (i = 0; genet_compatible[i]; i++)
    {
        node = FDT_FindCompatible(FDT_Root(), genet_compatible[i]);
        if (node != FDT_NONE)
            break;
    }

    if (node == FDT_NONE)
    {
        D(bug("[genet] no v5 controller in the tree\n");)
        return FALSE;
    }

    if (!genet_enabled(node))
    {
        D(bug("[genet] %s is disabled\n", FDT_NodeName(node));)
        return FALSE;
    }

    if (!FDT_GetReg(node, 0, &addr, &size) || !addr || !size)
    {
        D(bug("[genet] %s has no registers\n", FDT_NodeName(node));)
        return FALSE;
    }

    hw->phys = (IPTR)genet_translate(node, addr);
    hw->size = (IPTR)size;

    hw->irq = genet_interrupt(node, 0);
    hw->irq2 = genet_interrupt(node, 1);

    if (!genet_phyaddress(node, &hw->phyAddr))
    {
        /* The BCM54213PE on every Pi 4 answers here */
        hw->phyAddr = 1;
        D(bug("[genet] tree names no phy, trying address 1\n");)
    }

    hw->phyMode = genet_mode(node);

    /* The firmware patches the address it was given into the tree */
    hw->haveMacAddr = FALSE;
    mac = FDT_GetProp(node, "local-mac-address", &len);
    if (!mac || len != ETH_ADDRESSSIZE)
        mac = FDT_GetProp(node, "mac-address", &len);
    if (mac && len == ETH_ADDRESSSIZE)
    {
        CopyMem((APTR)mac, hw->macAddr, ETH_ADDRESSSIZE);
        hw->haveMacAddr = TRUE;
    }

    /*
     * The boot page tables cover RAM and nothing else, so the register
     * block has to be mapped before it can be touched at all. It is
     * mapped where it already lives, so the two addresses are the same.
     *
     * MAP_CacheInhibit is what makes this device memory rather than
     * ordinary cacheable RAM. Without it the mapping is write-back and
     * register writes sit in a cache line instead of reaching the block.
     */
    if (!KrnMapGlobal((APTR)hw->phys, (APTR)hw->phys, (ULONG)hw->size,
                      MAP_Readable | MAP_Writable | MAP_CacheInhibit))
    {
        D(bug("[genet] could not map %p+%p\n", (APTR)hw->phys,
              (APTR)hw->size);)
        return FALSE;
    }

    hw->base = hw->phys;

    D(bug("[genet] %s: tree says %p, cpu sees %p+%p\n", FDT_NodeName(node),
          (APTR)(IPTR)addr, (APTR)hw->phys, (APTR)hw->size);)
    D(bug("[genet] irq %u/%u, phy %u, mode %s%s\n", hw->irq, hw->irq2,
          hw->phyAddr, genet_modenames[hw->phyMode],
          hw->haveMacAddr ? ", address from tree" : "");)

    return TRUE;
}
