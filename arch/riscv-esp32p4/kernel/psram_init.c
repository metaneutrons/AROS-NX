/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: External PSRAM bring-up: the PLL, the controller and the chip.
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
 * Bring the MPLL up at 400 MHz, which is the bus clock's only usable
 * source.
 *
 * The PLL's dividers are not memory mapped; they sit on the internal
 * configuration bus described in psram.h, so most of this function is four
 * accesses to that bus with a bounded wait around each. Every wait here is
 * bounded on purpose: the failure this replaces was a hang, and a
 * bring-up step that can hang is not an improvement on one that reports
 * that it failed.
 *
 * Returns non-zero if the PLL reported its calibration done.
 */
/*
 * The clock-dependent parameters, chosen once from the target rate.
 *
 * These were compile-time constants while there was only one clock.  They
 * are variables now rather than a parameter threaded through six functions,
 * because every one of those functions runs before exec exists and is called
 * from exactly one place in one order; a second caller would be the bug, not
 * the shared state.  In SRAM, like the code that reads them.
 */
P4_SRAMDATA static uint32_t p4_rd_latency  = P4_PSRAM_RD_LATENCY_SLOW;
P4_SRAMDATA static uint32_t p4_wr_latency  = P4_PSRAM_WR_LATENCY_SLOW;
P4_SRAMDATA static uint32_t p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_SLOW;
P4_SRAMDATA static uint32_t p4_rd_dummy    = P4_PSRAM_RD_DUMMY_SLOW;
P4_SRAMDATA static uint32_t p4_wr_dummy    = P4_PSRAM_WR_DUMMY_SLOW;

/*
 * Above 80 MHz the chip needs the longer latencies.  The threshold is the
 * boundary ESP-IDF draws between its own parameter sets, and 200 MHz is the
 * only rate above it this port asks for.
 */
P4_SRAMCODE static void p4_psram_select_params(unsigned long target_hz)
{
    if (target_hz > 80000000UL)
    {
        p4_rd_latency   = P4_PSRAM_RD_LATENCY_FAST;
        p4_wr_latency   = P4_PSRAM_WR_LATENCY_FAST;
        p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_FAST;
        p4_rd_dummy     = P4_PSRAM_RD_DUMMY_FAST;
        p4_wr_dummy     = P4_PSRAM_WR_DUMMY_FAST;
    }
    else
    {
        p4_rd_latency   = P4_PSRAM_RD_LATENCY_SLOW;
        p4_wr_latency   = P4_PSRAM_WR_LATENCY_SLOW;
        p4_rd_reg_dummy = P4_PSRAM_RD_REG_DUMMY_SLOW;
        p4_rd_dummy     = P4_PSRAM_RD_DUMMY_SLOW;
        p4_wr_dummy     = P4_PSRAM_WR_DUMMY_SLOW;
    }
}

P4_SRAMCODE static int p4_regi2c_idle(void)
{
    int spin = 100000;

    while ((p4_r32(P4_I2C_ANA_MST_I2C0_CTRL) & P4_REGI2C_BUSY) && --spin)
        ;

    return spin != 0;
}

/* One slave block may be selected at a time, so both selection registers
   are cleared before the MPLL is named. */
P4_SRAMCODE static void p4_regi2c_select_mpll(void)
{
    p4_w32(P4_I2C_ANA_MST_ANA_CONF2,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF2) & ~P4_I2C_ANA_CONF_MASK);
    p4_w32(P4_I2C_ANA_MST_ANA_CONF1,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF1) & ~P4_I2C_ANA_CONF_MASK);
    p4_w32(P4_I2C_ANA_MST_ANA_CONF2,
           p4_r32(P4_I2C_ANA_MST_ANA_CONF2) | P4_REGI2C_MPLL_MST_SEL);
}

P4_SRAMCODE static int p4_regi2c_read(unsigned char reg, unsigned char *out)
{
    p4_regi2c_select_mpll();
    if (!p4_regi2c_idle())
        return 0;

    p4_w32(P4_I2C_ANA_MST_I2C0_CTRL,
           ((unsigned long)P4_REGI2C_MPLL << P4_REGI2C_SLAVE_SHIFT)
           | ((unsigned long)reg << P4_REGI2C_ADDR_SHIFT));

    if (!p4_regi2c_idle())
        return 0;

    *out = (unsigned char)((p4_r32(P4_I2C_ANA_MST_I2C0_CTRL)
                            >> P4_REGI2C_DATA_SHIFT) & 0xFF);
    return 1;
}

P4_SRAMCODE static int p4_regi2c_write(unsigned char reg, unsigned char val)
{
    p4_regi2c_select_mpll();
    if (!p4_regi2c_idle())
        return 0;

    p4_w32(P4_I2C_ANA_MST_I2C0_CTRL,
           ((unsigned long)P4_REGI2C_MPLL << P4_REGI2C_SLAVE_SHIFT)
           | ((unsigned long)reg << P4_REGI2C_ADDR_SHIFT)
           | ((unsigned long)val << P4_REGI2C_DATA_SHIFT)
           | P4_REGI2C_WRITE);

    return p4_regi2c_idle();
}

P4_SRAMCODE int krnPSRAMMPLLUp(void)
{
    unsigned char rstb, dhref;
    unsigned long div;
    int spin;

    /*
     * The configuration bus master's clock and source. The ESP-IDF
     * bootloader we boot behind leaves both set and says so in a comment,
     * but setting a bit that is already set costs nothing and depending on
     * another program's leftovers is the fragile choice.
     */
    p4_w32(P4_CLKRST_REF_CLK_CTRL2,
           p4_r32(P4_CLKRST_REF_CLK_CTRL2) | P4_REF_160M_CLK_EN);
    p4_w32(P4_LPPERI_CLK_EN, p4_r32(P4_LPPERI_CLK_EN) | P4_CK_EN_LP_I2CMST);
    p4_w32(P4_I2C_ANA_MST_CLK160M,
           p4_r32(P4_I2C_ANA_MST_CLK160M) | P4_CLK_I2C_MST_SEL_160M);

    /* Power the PLL up, and open the gate that lets its output reach the
       high power domain where MSPI lives */
    p4_w32(P4_PMU_RF_PWC, p4_r32(P4_PMU_RF_PWC) | P4_PMU_MSPI_PHY_XPD);
    p4_w32(P4_LP_CLKRST_HP_CLK_CTRL,
           p4_r32(P4_LP_CLKRST_HP_CLK_CTRL) | P4_HP_MPLL_500M_CLK_EN);

    /* The calibration runs while the stop bit is clear, so it has to be
       cleared before the dividers are written, not after */
    p4_w32(P4_CLKRST_ANA_PLL_CTRL0,
           p4_r32(P4_CLKRST_ANA_PLL_CTRL0) & ~P4_MSPI_CAL_STOP);

    /* Reference level to its maximum first */
    if (!p4_regi2c_read(P4_MPLL_DHREF_REG, &dhref))
        return 0;
    if (!p4_regi2c_write(P4_MPLL_DHREF_REG,
                         dhref | (3 << P4_MPLL_DHREF_SHIFT)))
        return 0;

    /* Then the calibration reset, low and back high */
    if (!p4_regi2c_read(P4_MPLL_IR_CAL_RSTB_REG, &rstb))
        return 0;
    if (!p4_regi2c_write(P4_MPLL_IR_CAL_RSTB_REG,
                         rstb & (unsigned char)~P4_MPLL_IR_CAL_RSTB))
        return 0;
    if (!p4_regi2c_write(P4_MPLL_IR_CAL_RSTB_REG,
                         rstb | P4_MPLL_IR_CAL_RSTB))
        return 0;

    /* And the multiplier. ref_div stays at one, so the PLL sees half of
       XTAL and the target divided by that, less one, is the field. */
    div = P4_PSRAM_MPLL_HZ / (P4_XTAL_HZ / 2) - 1;
    if (!p4_regi2c_write(P4_MPLL_DIV_REG,
                         (unsigned char)((div << P4_MPLL_DIV_SHIFT)
                                         | (1UL << P4_MPLL_REF_DIV_SHIFT))))
        return 0;

    spin = 1000000;
    while (!(p4_r32(P4_CLKRST_ANA_PLL_CTRL0) & P4_MSPI_CAL_END) && --spin)
        ;

    p4_w32(P4_CLKRST_ANA_PLL_CTRL0,
           p4_r32(P4_CLKRST_ANA_PLL_CTRL0) | P4_MSPI_CAL_STOP);

    return spin != 0;
}

/*
 * Read the PLL's three configuration bytes back off the configuration bus.
 *
 * A write there reports nothing, so this is the only way to tell a
 * configured PLL from a write that went somewhere else. Returns the three
 * bytes packed as rstb, div, dhref from the low byte up.
 */
P4_SRAMCODE unsigned long krnPSRAMMPLLState(void)
{
    unsigned char rstb = 0xFF, div = 0xFF, dhref = 0xFF;

    p4_regi2c_read(P4_MPLL_IR_CAL_RSTB_REG, &rstb);
    p4_regi2c_read(P4_MPLL_DIV_REG, &div);
    p4_regi2c_read(P4_MPLL_DHREF_REG, &dhref);

    return (unsigned long)rstb | ((unsigned long)div << 8)
           | ((unsigned long)dhref << 16);
}

/*
 * Take the pair of MSPI controllers out of reset with their clocks on, and
 * set the bus clock as close to the requested rate as the divider allows.
 *
 * The source is the MPLL, so krnPSRAMMPLLUp has to have run and reported
 * success first. The controller's core clock is the PLL's own rate,
 * undivided, and the counters written here divide that; a target that
 * divides 400 MHz evenly is therefore exact, and 20, 40, 50, 80, 100 and
 * 200 MHz all do.
 *
 * Returns the rate actually set, or 0 if the controller did not answer.
 */
/*
 * The divider alone, with no reset and nothing else touched.
 *
 * This exists because krnPSRAMClockUp() below is not a clock setter: it also
 * takes both controllers out of reset, which is exactly right the first time
 * and destroys everything the second.  Calling it again after the mode
 * registers were written left the controller at its reset defaults and the
 * next transaction never completed, which is a hang with no output at all.
 * The calibration changes the clock three times, so it needs this.
 *
 * ESP-IDF's own clock change is the same three register writes and no reset,
 * which is the confirmation that nothing else has to move with the divider.
 * The DLL in particular stays as krnPSRAMConfigure() left it.
 *
 * Returns the rate actually set, or 0 if the controller kept nothing.
 */
P4_SRAMCODE unsigned long krnPSRAMClockSet(unsigned long target_hz)
{
    unsigned long div, n, clkval;

    if (target_hz == 0)
        target_hz = 20000000UL;

    /* Round up, so the bus never runs faster than asked */
    div = (P4_PSRAM_MPLL_HZ + target_hz - 1) / target_hz;
    if (div < 1)
        div = 1;
    if (div > 256)              /* the counters are eight bits each */
        div = 256;

    if (div == 1)
        clkval = P4_SCLK_EQU_SYSCLK;
    else
    {
        n = div - 1;
        clkval = (n << P4_SCLKCNT_N_SHIFT)
               | ((div / 2 - 1) << P4_SCLKCNT_H_SHIFT)
               | (n << P4_SCLKCNT_L_SHIFT);
    }

    p4_w32(P4_MSPI2_SRAM_CLK, clkval);
    p4_w32(P4_MSPI3_CLOCK, clkval);

    if (p4_r32(P4_MSPI2_SRAM_CLK) != clkval)
        return 0;

    return P4_PSRAM_MPLL_HZ / div;
}

P4_SRAMCODE unsigned long krnPSRAMClockUp(unsigned long target_hz)
{
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

    p4_w32(P4_CLKRST_PERI_CLK_CTRL00,
           (p4_r32(P4_CLKRST_PERI_CLK_CTRL00) & ~P4_PSRAM_CLK_SRC_MASK)
           | ((unsigned long)P4_PSRAM_CLK_SRC_MPLL << P4_PSRAM_CLK_SRC_SHIFT));

    /*
     * The divider, and its read-back is the test for everything above.  An
     * unclocked or still-reset controller does not keep what was written to
     * it, so a value that comes back unchanged says the reset was released
     * and the module clocks are on, which is the whole claim this stage makes.
     */
    return krnPSRAMClockSet(target_hz);
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
/*
 * Raise every PSRAM pin's drive strength. Reset leaves them at zero, which
 * is not enough for the chip to see anything, so this is a precondition for
 * the bring-up rather than a tuning step.
 *
 * The table lives in SRAM, not in the read-only section, for the reason at
 * the top of this file: a P4_SRAMCODE function may not reach through the
 * cache that MSPI serves.
 */
P4_SRAMCODE static void p4_psram_pin_drive(unsigned long drv)
{
    P4_SRAMRODATA static const struct { unsigned short off; unsigned char sh; }
        pins[] = { P4_PSRAM_PIN_DRV_TABLE };
    unsigned int i;

    for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
    {
        unsigned long reg = P4_IOMUX_MSPI_PIN_BASE + pins[i].off;

        p4_w32(reg, (p4_r32(reg) & ~(3UL << pins[i].sh))
                    | ((drv & 3UL) << pins[i].sh));
    }
}

P4_SRAMCODE void krnPSRAMConfigure(void)
{
    unsigned long ac;

    /* Drive strength before anything is sent, then the strobe: without the
       strobe a DTR read never completes, and the controller waits rather
       than complaining */
    p4_psram_pin_drive(P4_PSRAM_PIN_DRV);

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
 * Talking to the chip, as opposed to the controller in front of it.
 *
 * The transaction is done by three functions in the part's own mask ROM, at
 * fixed published addresses. Writing an MSPI transaction engine to send
 * sixteen bits of command and move two bytes would be work for its own
 * sake, and the ROM is always mapped, so calling it needs no cache.
 *
 * The operating mode is set before every transaction rather than once.
 * ESP-IDF does the same, and it is the difference between the second
 * transaction in a row answering and not answering.
 */
P4_SRAMCODE static void p4_psram_cmd(uint32_t cmd, uint32_t reg_addr,
                                     uint32_t dummy,
                                     uint32_t *tx, uint32_t tx_bits,
                                     uint32_t *rx, uint32_t rx_bits)
{
    void (*rom_set_op_mode)(int, int) =
        (void (*)(int, int))P4_ROM_SPI_SET_OP_MODE;
    void (*rom_cmd_config)(int, const struct p4_rom_spi_cmd *) =
        (void (*)(int, const struct p4_rom_spi_cmd *))P4_ROM_SPI_CMD_CONFIG;
    void (*rom_cmd_start)(int, unsigned char *, uint32_t, uint32_t, int) =
        (void (*)(int, unsigned char *, uint32_t, uint32_t, int))P4_ROM_SPI_CMD_START;

    struct p4_rom_spi_cmd c;
    uint32_t addr = reg_addr;

    c.cmd = (uint16_t)cmd;
    c.cmd_bitlen = 16;
    c.addr = &addr;
    c.addr_bitlen = 32;
    c.tx_data = tx;
    c.tx_data_bitlen = tx_bits;
    c.rx_data = rx;
    c.rx_data_bitlen = rx_bits;
    c.dummy_bitlen = dummy;

    rom_set_op_mode(P4_MSPI_ID_REG, P4_ROM_OPI_DTR_MODE);
    rom_cmd_config(P4_MSPI_ID_REG, &c);
    rom_cmd_start(P4_MSPI_ID_REG, (unsigned char *)rx, rx_bits / 8,
                  P4_PSRAM_CS_INDEX, 0);
}

/*
 * Mode registers come in pairs at even addresses, because the bus is
 * sixteen bits wide and a transfer moves both halves. Address 0 carries
 * mode register 0 in its low byte and mode register 1 in its high byte,
 * address 4 carries 4 and 5, address 8 carries 8. A read at an odd address
 * is not a way to reach the odd-numbered register.
 */
P4_SRAMCODE static void p4_psram_reg_read(uint32_t addr, uint32_t *pair)
{
    *pair = 0;
    p4_psram_cmd(P4_PSRAM_REG_READ, addr, p4_rd_reg_dummy,
                 NULL, 0, pair, 16);
}

P4_SRAMCODE static void p4_psram_reg_write(uint32_t addr, uint32_t pair)
{
    uint32_t v = pair;

    p4_psram_cmd(P4_PSRAM_REG_WRITE, addr, 0, &v, 16, NULL, 0);
}

/*
 * Configure the chip through its mode registers, which has to happen before
 * anything can be read from it.
 *
 * This is not an optimisation step. Mode register 8 selects the bus width,
 * and until it is told otherwise the part does not drive all sixteen lanes;
 * reading its identity first, which is what this file did at first, asks a
 * chip in one configuration a question in another and gets an answer that
 * looks like a floating bus. Each register is read, the fields this port
 * owns are replaced, and the rest is written back untouched.
 *
 * The latencies are the pair for 80 MHz and below. They are the only values
 * here that depend on the clock, which is why raising the clock later means
 * revisiting this function and not just the divider.
 */
P4_SRAMCODE void krnPSRAMModeInit(void)
{
    /*
     * Absolute values, not read-modify-write.
     *
     * ESP-IDF reads each register, replaces the fields it owns and writes
     * the rest back, which is the right thing to do when the read can be
     * trusted. Here it cannot: before the chip is configured a read returns
     * a floating bus, and writing 0xff back sets every reserved bit along
     * with a partial-array-refresh and refresh-rate setting nobody asked
     * for. Writing the power-on configuration outright is both simpler and
     * the only version that does not depend on the thing being fixed.
     *
     * mode register 0: drive strength 0, read latency 2, fixed latency
     * mode register 4: write latency 2, no partial array refresh
     * mode register 8: burst length 3, linear bursts, row crossing, x16
     *
     * The two latencies are the pair for 80 MHz and below and are the only
     * values here that depend on the clock.
     */
    p4_psram_reg_write(0, (p4_rd_latency << 2) | (1UL << 5));
    p4_psram_reg_write(4, p4_wr_latency << 5);
    p4_psram_reg_write(8, 3UL | (1UL << 3) | (1UL << 6));
}

/*
 * Ask the chip who it is.
 *
 * Mode register 1 holds the vendor in its low five bits, mode register 2 the
 * density. 0x0d as the vendor means Espressif's AP part, which is what this
 * board carries; anything else means the sequence above configured a
 * controller with nothing on the other end.
 */
P4_SRAMCODE int krnPSRAMIdentify(unsigned char *vendor, unsigned char *density)
{
    uint32_t pair;
    unsigned char mr1, mr2;

    p4_psram_reg_read(0, &pair);
    mr1 = (unsigned char)((pair >> 8) & P4_PSRAM_MR1_VENDOR_MASK);

    p4_psram_reg_read(2, &pair);
    mr2 = (unsigned char)(pair & 0xFF);

    if (vendor)
        *vendor = mr1;
    if (density)
        *density = mr2;

    return (mr1 == P4_PSRAM_VENDOR_AP);
}

/*
 * A block through the command path, in transaction-sized pieces.
 *
 * The controller's FIFO takes 64 bytes, so a 128-byte reference block is two
 * transactions.  These exist for the tuning, which needs to write a known
 * block at a clock it trusts and read it back at one it does not.
 */
P4_SRAMCODE void krnPSRAMBlockWrite(uint32_t addr, const uint32_t *words,
                                    uint32_t count)
{
    while (count)
    {
        uint32_t n = count > P4_PSRAM_FIFO_WORDS ? P4_PSRAM_FIFO_WORDS : count;

        p4_psram_cmd(P4_PSRAM_SYNC_WRITE, addr, p4_wr_dummy,
                     (uint32_t *)words, n * 32, NULL, 0);
        words += n;
        addr += n * 4;
        count -= n;
    }
}

P4_SRAMCODE void krnPSRAMBlockRead(uint32_t addr, uint32_t *words,
                                   uint32_t count)
{
    while (count)
    {
        uint32_t n = count > P4_PSRAM_FIFO_WORDS ? P4_PSRAM_FIFO_WORDS : count;

        p4_psram_cmd(P4_PSRAM_SYNC_READ, addr, p4_rd_dummy,
                     NULL, 0, words, n * 32);
        words += n;
        addr += n * 4;
        count -= n;
    }
}

/*
 * Write a word to the chip and read it back.
 *
 * This answers a different question from the identity read: not whether the
 * mode registers are being addressed correctly, but whether the controller,
 * the pins, the clock and the chip carry data at all. ESP-IDF makes the same
 * test, at the same address, with the same pattern, as its check for whether
 * a chip is connected.
 */
P4_SRAMCODE int krnPSRAMRoundTrip(uint32_t *back)
{
    uint32_t out = P4_PSRAM_TEST_PATTERN;
    uint32_t in = 0;

    p4_psram_cmd(P4_PSRAM_SYNC_WRITE, 0, p4_wr_dummy,
                 &out, 32, NULL, 0);
    p4_psram_cmd(P4_PSRAM_SYNC_READ, 0, p4_rd_dummy,
                 NULL, 0, &in, 32);

    if (back)
        *back = in;

    return in == P4_PSRAM_TEST_PATTERN;
}

/*
 * The same read, with the transaction started by hand.
 *
 * Everything the mask ROM programs has been measured to match a working
 * ESP-IDF run register for register, and the data still arrives as ones.
 * That leaves the possibility that the ROM's start call is not doing what
 * its name says on this part, so this does the last step directly: set the
 * user transaction bit, wait for the controller to clear it, and read the
 * data out of the controller's own buffer rather than out of a pointer the
 * ROM copied into.
 *
 * Returns the data word. The state machine's two status fields, sampled
 * before and after, go into *state; a controller that never left its idle
 * state would say so there.
 */
/*
 * The whole bring-up, in the order it has to happen.
 *
 * The order is not a matter of taste. The PLL has to be running before a
 * divider off it means anything; the pins have to be able to drive before
 * a command can be seen; the mode registers have to be written before a
 * read returns anything, because until then the chip is not driving all of
 * its data lines.
 *
 * Interrupts are the caller's business. This runs from SRAM and does not
 * print, so that the sequence is not interleaved with a console that lives
 * in flash.
 */
P4_SRAMCODE int krnPSRAMBringUp(struct P4PSRAMInfo *info,
                                unsigned long target_hz)
{
    P4_SRAMRODATA static const unsigned char sizes[] = P4_PSRAM_SIZE_TABLE;
    unsigned char vendor = 0, density = 0;
    uint32_t back = 0;
    int fast;

    info->clock_hz = 0;
    info->size = 0;
    info->vendor = 0;
    info->density = 0;
    info->round_trip = 0;
    info->fell_back = 0;
    info->tuning.tuned = 0;

    fast = target_hz > 80000000UL;
    info->fast_requested = (unsigned char)fast;

    info->mpll_up = krnPSRAMMPLLUp() ? 1 : 0;
    if (!info->mpll_up)
        return 0;

    /*
     * The parameter set comes from the target, and the whole device
     * configuration then runs at 20 MHz regardless.
     *
     * ESP-IDF configures the chip at the target rate with the sampling
     * untuned, which works and is one risk this port has no reason to take:
     * a longer latency is harmless at a lower clock, so the same registers
     * can be written slowly and only the divider raised afterwards.  If the
     * identity read fails, it fails at a clock where the answer means the
     * chip, and not the sampling.
     */
    p4_psram_select_params(target_hz);
    krnPSRAMTuningClear();

    info->clock_hz = krnPSRAMClockUp(20000000UL);
    if (!info->clock_hz)
        return 0;

    krnPSRAMConfigure();
    krnPSRAMModeInit();

    if (!krnPSRAMIdentify(&vendor, &density))
    {
        info->vendor = vendor;
        info->density = density;
        return 0;
    }

    info->vendor = vendor;
    info->density = density;
    info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;

    if (fast && info->round_trip)
    {
        if (krnPSRAMTune(&info->tuning, target_hz))
        {
            info->clock_hz = target_hz;
            /* The same question again, now at the tuned clock.  A window
               found on 128 bytes that cannot round-trip one word is not a
               window. */
            info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;
        }
        if (!info->tuning.tuned || !info->round_trip)
        {
            /*
             * Back to a rate that has never failed, with the sampling
             * neutral again.  The parameter set stays as it was: it is
             * correct at any lower clock, and rewriting the mode registers
             * here would add a failure path to the recovery path.
             */
            info->fell_back = 1;
            krnPSRAMTuningClear();
            info->clock_hz = krnPSRAMClockSet(20000000UL);
            info->round_trip = krnPSRAMRoundTrip(&back) ? 1 : 0;
        }
    }

    krnPSRAMAxiConfigure();
    info->size = (unsigned long)sizes[density & P4_PSRAM_MR2_DENSITY_MASK]
                 * 1024UL * 1024UL;

    return info->size != 0 && info->round_trip;
}

/*
 * Tell MSPI2 how to serve a load or a store to the window.
 *
 * Everything before this configures the chip. This configures the path the
 * cache uses to reach it: the two commands, the address length, the two
 * dummy lengths, octal command and address with sixteen bit data, double
 * transfer rate, and finally permission to answer AXI requests at all.
 * The last of those is why a store to a correctly mapped window is a bus
 * error until this has run.
 *
 * The dummy lengths are the pair for 80 MHz and below, like the latencies
 * in the mode registers, and have to change with them when the clock rises.
 */
P4_SRAMCODE void krnPSRAMAxiConfigure(void)
{
    unsigned long v;

    /* The commands the controller issues for a cache line */
    p4_w32(P4_MSPI2_SRAM_DWR_CMD,
           ((unsigned long)(16 - 1) << P4_SRAM_CMD_BITLEN_SHIFT)
           | (P4_PSRAM_SYNC_WRITE & P4_SRAM_CMD_VALUE_MASK));
    p4_w32(P4_MSPI2_SRAM_DRD_CMD,
           ((unsigned long)(16 - 1) << P4_SRAM_CMD_BITLEN_SHIFT)
           | (P4_PSRAM_SYNC_READ & P4_SRAM_CMD_VALUE_MASK));

    v = p4_r32(P4_MSPI2_CACHE_SCTRL);
    v &= ~(P4_SRAM_ADDR_BITLEN_MASK | P4_SRAM_RDUMMY_MASK | P4_SRAM_WDUMMY_MASK);
    v |= P4_CACHE_SRAM_USR_WCMD | P4_CACHE_SRAM_USR_RCMD;
    v |= P4_CACHE_USR_SADDR_4BYTE;
    v |= P4_USR_WR_SRAM_DUMMY | P4_USR_RD_SRAM_DUMMY;
    v |= P4_SRAM_OCT;
    v |= (unsigned long)(P4_PSRAM_ADDR_BITLEN - 1) << P4_SRAM_ADDR_BITLEN_SHIFT;
    v |= (unsigned long)(p4_rd_dummy - 1) << P4_SRAM_RDUMMY_SHIFT;
    v |= (unsigned long)(p4_wr_dummy - 1) << P4_SRAM_WDUMMY_SHIFT;
    p4_w32(P4_MSPI2_CACHE_SCTRL, v);

    /* Octal for command and address, sixteen bits for data */
    v = p4_r32(P4_MSPI2_SRAM_CMD);
    v |= P4_MEM_SCMD_OCT | P4_MEM_SADDR_OCT | P4_MEM_SDOUT_OCT | P4_MEM_SDIN_OCT;
    v |= P4_MEM_SDIN_HEX | P4_MEM_SDOUT_HEX;
    v |= P4_MEM_SDUMMY_WOUT;
    p4_w32(P4_MSPI2_SRAM_CMD, v);

    /* Double transfer rate, no byte swapping, variable dummy on both
       controllers as ESP-IDF sets it */
    v = p4_r32(P4_MSPI2_SMEM_DDR);
    v &= ~(P4_DDR_RDAT_SWP | P4_DDR_WDAT_SWP);
    v |= P4_DDR_EN | P4_DDR_VAR_DUMMY;
    p4_w32(P4_MSPI2_SMEM_DDR, v);
    p4_w32(P4_MSPI3_DDR, p4_r32(P4_MSPI3_DDR) | P4_DDR_VAR_DUMMY);

    /* Splice adjacent AXI bursts */
    p4_w32(P4_MSPI2_CTRL1,
           p4_r32(P4_MSPI2_CTRL1) | P4_MEM_AW_SPLICE_EN | P4_MEM_AR_SPLICE_EN);

    /* And last, because until now there was nothing to answer with */
    p4_w32(P4_MSPI2_CACHE_FCTRL,
           (p4_r32(P4_MSPI2_CACHE_FCTRL) & ~P4_CLOSE_AXI_INF_EN)
           | P4_MEM_AXI_REQ_EN);
}

/*
 * Put the chip in the window at 0x48000000.
 *
 * Physical page n goes to virtual page n, which is the only arrangement
 * that makes the window look like memory. The two functions below are the
 * exception to this file's rule about running from SRAM: they touch the
 * translation table and the window, not the controller's configuration, so
 * the cache they are fetched through is not the one they are changing.
 */
void krnPSRAMMap(unsigned long size)
{
    unsigned long pages = size >> P4_MMU_PAGE_SHIFT;
    unsigned long i;

    if (pages > P4_MMU_ENTRIES)
        pages = P4_MMU_ENTRIES;

    for (i = 0; i < pages; i++)
    {
        p4_w32(P4_MMU_PSRAM_INDEX, i);
        p4_w32(P4_MMU_PSRAM_CONTENT, i | P4_MMU_VALID | P4_MMU_ACCESS_PSRAM);
    }
}

/*
 * Check that the window really is that much memory.
 *
 * One word per megabyte, each holding something derived from its own
 * address, all written before any is read back. Spreading the writes over
 * the whole range and reading afterwards is what makes this a test of the
 * memory rather than of the cache: 32 MB of writes cannot sit in 128 KB of
 * cache, so every read has to go to the chip. A part that aliases, or a
 * table with the wrong page in it, shows up as a word carrying another
 * address's value.
 *
 * Returns non-zero if every word came back. On failure *failed_at is the
 * address that did not.
 */
int krnPSRAMVerify(unsigned long size, unsigned long *failed_at)
{
    const unsigned long step = 1024UL * 1024UL;
    volatile unsigned long *p;
    unsigned long a;

    for (a = 0; a < size; a += step)
    {
        p = (volatile unsigned long *)(P4_PSRAM_WINDOW_BASE + a);
        *p = (P4_PSRAM_WINDOW_BASE + a) ^ 0xA5A5A5A5UL;
    }

    for (a = 0; a < size; a += step)
    {
        p = (volatile unsigned long *)(P4_PSRAM_WINDOW_BASE + a);
        if (*p != ((P4_PSRAM_WINDOW_BASE + a) ^ 0xA5A5A5A5UL))
        {
            if (failed_at)
                *failed_at = P4_PSRAM_WINDOW_BASE + a;
            return 0;
        }
    }

    return 1;
}

/*
 * Where this stands.
 *
 * Two things were wrong in the earlier version of this file and both are
 * fixed above. The first was the clock source: taking the bus clock off
 * XTAL to avoid bringing the MPLL up rests on the idea that 20 MHz needs
 * no PLL, and ESP-IDF's own 20 MHz configuration runs the MPLL at 400 MHz
 * and divides by twenty. The divider sits behind the PLL. What a low bus
 * clock saves is the read timing calibration, and nothing else - which
 * makes the slow bring-up a smaller saving than it looked, and the step to
 * 200 MHz shorter, because the PLL will already be there.
 *
 * The second was an address. The source select field is in PERI_CLK_CTRL00
 * and this file wrote PERI_CLK_CTRL01, so the two bits went into the wrong
 * register. It went unnoticed because XTAL is encoded as zero and zero is
 * the reset value, so the bus ran off XTAL either way and the read-back
 * test on the divider still passed. Whatever bits 12 and 13 of CTRL01 are,
 * they were being cleared for no reason.
 *
 * What is still not done, in the order it has to happen: the mode register
 * writes (MR0 latency and drive, MR4 write latency, MR8 burst length and
 * bus width), which ESP-IDF performs before it reads anything back rather
 * than after; mapping the window at 0x48000000, which the bootloader
 * unmaps on its way out; handing the range to exec; and then the clock,
 * where 200 MHz needs the per-board read timing calibration and the pin
 * drive strength that this file deliberately leaves at reset.
 */
