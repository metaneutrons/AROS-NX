/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: External PSRAM bring-up, first stage: the controller's clock.
*/

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"
#include "psram.h"

/*
 * Why this is P4_SRAMCODE: with ldscript-xip.lds the image's code is
 * fetched through the cache that MSPI serves, and reconfiguring MSPI while
 * executing from it is a way to stop executing anything. Everything this
 * file touches has to be reachable without that cache.
 */

/*
 * Take the pair of MSPI controllers out of reset with their clocks on, and
 * set the bus clock as close to the requested rate as the divider allows.
 *
* XTAL as the source was the plan and does not work, see the note below. The MPLL has to
 * be brought up and told a frequency before it can be selected, and at the
 * clocks it makes possible the read timing has to be calibrated per board;
 * XTAL is running before this code does, and 40 MHz divided down needs no
 * calibration at all. The cost is the ceiling: 20 MHz here against 200 MHz
 * there, a tenth of the bandwidth and ten times the latency. That is the
 * trade this stage takes on purpose, and the divider is the only thing
 * that has to change when the MPLL and the calibration arrive.
 *
 * Returns the rate actually set, or 0 if the controller did not answer.
 */
P4_SRAMCODE unsigned long krnPSRAMClockUp(unsigned long target_hz)
{
    unsigned long div, n, clkval, readback;

    /* Module clocks first: the registers below do not answer without them */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL0,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL0) | P4_PSRAM_SYS_CLK_EN);
    p4_w32(P4_CLKRST_PERI_CLK_CTRL00,
           p4_r32(P4_CLKRST_PERI_CLK_CTRL00) | P4_PSRAM_PLL_CLK_EN
                                             | P4_PSRAM_CORE_CLK_EN);

    /* Reset both halves, AXI outermost, and release in the reverse order */
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) | P4_RST_EN_DUAL_MSPI_AXI
                                        | P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_APB);
    p4_w32(P4_CLKRST_HP_RST_EN0,
           p4_r32(P4_CLKRST_HP_RST_EN0) & ~P4_RST_EN_DUAL_MSPI_AXI);

    p4_w32(P4_CLKRST_PERI_CLK_CTRL01,
           (p4_r32(P4_CLKRST_PERI_CLK_CTRL01) & ~P4_PSRAM_CLK_SRC_MASK)
           | ((unsigned long)P4_PSRAM_CLK_SRC_XTAL << P4_PSRAM_CLK_SRC_SHIFT));

    /* Round the divider up, so the bus never runs faster than asked */
    if (target_hz == 0)
        target_hz = 20000000UL;
    div = (P4_XTAL_HZ + target_hz - 1) / target_hz;
    if (div < 1)
        div = 1;
    if (div > 64)
        div = 64;

    if (div == 1)
    {
        clkval = P4_SCLK_EQU_SYSCLK;
    }
    else
    {
        n = div - 1;
        clkval = (n << P4_SCLKCNT_N_SHIFT)
               | ((div / 2 - 1) << P4_SCLKCNT_H_SHIFT)
               | (n << P4_SCLKCNT_L_SHIFT);
    }

    p4_w32(P4_MSPI2_SRAM_CLK, clkval);
    p4_w32(P4_MSPI3_CLOCK, clkval);

    /*
     * The read-back is the test. An unclocked or still-reset controller
     * does not keep what was written to it, so a value that comes back
     * unchanged says the two steps above took effect - which is the whole
     * claim this stage makes.
     */
    readback = p4_r32(P4_MSPI2_SRAM_CLK);
    if (readback != clkval)
        return 0;

    return P4_XTAL_HZ / div;
}

/*
 * The analogue configuration: how long chip select is asserted around a
 * transfer, whether the controller may split a burst, how large a page is,
 * and the delay line.
 *
 * None of these values depend on the bus clock, which is why this stage
 * says nothing about speed. The two that do - the read and write latencies
 * - are not here: they are told to the chip through its mode registers,
 * and belong with that sequence.
 *
 * The DLL is enabled at every clock ESP-IDF supports, including the slowest,
 * so it is enabled here too rather than guessed about.
 */
P4_SRAMCODE void krnPSRAMConfigure(void)
{
    unsigned long ac;

    /* The strobe first: without it a DTR read never completes, and the
       controller waits rather than complaining */
    p4_w32(P4_IOMUX_PSRAM_DQS_0,
           p4_r32(P4_IOMUX_PSRAM_DQS_0) | P4_IOMUX_DQS_XPD);
    p4_w32(P4_IOMUX_PSRAM_DQS_1,
           p4_r32(P4_IOMUX_PSRAM_DQS_1) | P4_IOMUX_DQS_XPD);

    ac = p4_r32(P4_MSPI_SMEM_AC);
    ac &= ~(P4_SMEM_CS_SETUP_TIME_M | P4_SMEM_CS_HOLD_TIME_M
            | P4_SMEM_CS_HOLD_DELAY_M);
    ac |= P4_SMEM_CS_SETUP | P4_SMEM_CS_HOLD | P4_SMEM_SPLIT_TRANS_EN;
    /* each counter is held as value minus one */
    ac |= ((unsigned long)(P4_PSRAM_CS_SETUP_TIME - 1) << P4_SMEM_CS_SETUP_TIME_SH);
    ac |= ((unsigned long)(P4_PSRAM_CS_HOLD_TIME - 1) << P4_SMEM_CS_HOLD_TIME_SH);
    ac |= ((unsigned long)(P4_PSRAM_CS_HOLD_DELAY - 1) << P4_SMEM_CS_HOLD_DELAY_SH);
    p4_w32(P4_MSPI_SMEM_AC, ac);

    p4_w32(P4_MSPI_SMEM_ECC_CTRL,
           (p4_r32(P4_MSPI_SMEM_ECC_CTRL) & ~P4_SMEM_PAGE_SIZE_MASK)
           | ((unsigned long)P4_PSRAM_PAGE_SIZE_2048 << P4_SMEM_PAGE_SIZE_SHIFT));

    p4_w32(P4_MSPI_SMEM_TIMING_CALI,
           p4_r32(P4_MSPI_SMEM_TIMING_CALI) | P4_DLL_TIMING_CALI);
    p4_w32(P4_MSPI_TIMING_CALI,
           p4_r32(P4_MSPI_TIMING_CALI) | P4_DLL_TIMING_CALI);
}

/*
 * Ask the chip who it is.
 *
 * The transaction itself is done by three functions in the part's own mask
 * ROM, at fixed published addresses. Writing an MSPI transaction engine to
 * send sixteen bits of command and read back eight would be work for its
 * own sake, and the ROM is always mapped, so calling it needs no cache.
 *
 * Mode register 1 holds the vendor in its low five bits and mode register 2
 * the density. 0x0d there means Espressif's AP part, which is what this
 * board carries; anything else means the sequence above configured a
 * controller that has nothing on the other end.
 */
P4_SRAMCODE int krnPSRAMIdentify(unsigned char *vendor, unsigned char *density)
{
    void (*rom_set_op_mode)(int, int) =
        (void (*)(int, int))P4_ROM_SPI_SET_OP_MODE;
    void (*rom_cmd_config)(int, const struct p4_rom_spi_cmd *) =
        (void (*)(int, const struct p4_rom_spi_cmd *))P4_ROM_SPI_CMD_CONFIG;
    void (*rom_cmd_start)(int, unsigned char *, uint32_t, uint32_t, int) =
        (void (*)(int, unsigned char *, uint32_t, uint32_t, int))P4_ROM_SPI_CMD_START;

    uint32_t addr;
    uint32_t rx;
    struct p4_rom_spi_cmd cmd;
    unsigned char mr1, mr2;

    /* Which ROM call the machine stops in, if it stops */
    krnP4PutStr("[psram]  rom    table ");
    krnP4PutHex32(*(volatile uint32_t *)P4_ROM_SPI_SET_OP_MODE);
    krnP4PutStr(" ");
    krnP4PutHex32(*(volatile uint32_t *)P4_ROM_SPI_CMD_CONFIG);
    krnP4PutStr("\n[psram]  rom    set_op_mode\n");
    rom_set_op_mode(P4_MSPI_ID_REG, P4_ROM_OPI_DTR_MODE);
    krnP4PutStr("[psram]  rom    returned\n");

    /* mode register 1: the vendor */
    addr = 1;
    rx = 0;
    cmd.cmd = P4_PSRAM_REG_READ;
    cmd.cmd_bitlen = 16;
    cmd.addr = &addr;
    cmd.addr_bitlen = 32;
    cmd.tx_data = NULL;
    cmd.tx_data_bitlen = 0;
    cmd.rx_data = &rx;
    cmd.rx_data_bitlen = 8;
    cmd.dummy_bitlen = P4_PSRAM_RD_REG_DUMMY_SLOW;
    krnP4PutStr("[psram]  rom    cmd_config\n");
    rom_cmd_config(P4_MSPI_ID_REG, &cmd);
    krnP4PutStr("[psram]  rom    cmd_start\n");
    rom_cmd_start(P4_MSPI_ID_REG, (unsigned char *)&rx, 1, P4_PSRAM_CS_MASK, 0);
    krnP4PutStr("[psram]  rom    done\n");
    mr1 = (unsigned char)(rx & P4_PSRAM_MR1_VENDOR_MASK);

    /* mode register 2: the density */
    addr = 2;
    rx = 0;
    cmd.rx_data = &rx;
    rom_cmd_config(P4_MSPI_ID_REG, &cmd);
    rom_cmd_start(P4_MSPI_ID_REG, (unsigned char *)&rx, 1, P4_PSRAM_CS_MASK, 0);
    mr2 = (unsigned char)(rx & 0xFF);

    if (vendor)
        *vendor = mr1;
    if (density)
        *density = mr2;

    return (mr1 == P4_PSRAM_VENDOR_AP);
}

/*
 * Where this stands, and the mistake in the plan above.
 *
 * The chip does not answer yet. The mode register read reaches the ROM's
 * transaction call and never returns from it - set_op_mode and cmd_config
 * both return, cmd_start does not, which is a transaction that was started
 * and never completed rather than a rejected one.
 *
 * The strobe was the first suspect and is not the answer: enabling both
 * DQS pins changed nothing. The likelier one is the clock source. This file
 * takes the bus clock off XTAL to avoid bringing the MPLL up, on the
 * reasoning that 20 MHz needs no calibration - and the second half of that
 * is true while the first half is not. ESP-IDF's own 20 MHz configuration
 * still runs the MPLL at 400 MHz and divides it by twenty; the divider is
 * off the MPLL, not off XTAL. What the low clock avoids is the calibration,
 * not the PLL. The controller's own clock enable is named for a PLL, which
 * fits.
 *
 * So the next step is the MPLL after all, at 400 MHz, and then this
 * sequence unchanged behind it. That makes the 20 MHz stage a smaller
 * saving than it looked - the calibration only - and it makes the path to
 * 200 MHz shorter, because the MPLL will already be there.
 */
