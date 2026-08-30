/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: I2C master transport for the ESP32-P4, free of any AROS object model.
*/

/*
 * Why this file has no OOP in it.
 *
 * AROS already has an I2C subsystem: `workbench/hidds/i2c` provides
 * CLID_Hidd_I2C under CLID_Hidd_Bus, and `arch/riscv64-opensbi/hidd/dwi2c`
 * is the precedent for putting a real controller under it rather than a
 * bit-banged line pair.  That driver splits itself exactly as this file and a
 * later class will: a hardware layer whose entry point is
 * `DWI2C_HWTransfer(ctrl, address, ...)` with no object anywhere in the
 * signature, and a thin class file above it.
 *
 * This is that hardware layer, written first because the panel power sequence
 * has to be provable before there is a graphics stack to hang a HIDD from,
 * and written to that shape so the class can call it unchanged.  When
 * `i2c-esp32p4.hidd` arrives it should compile this file rather than reimplement
 * it, and this file should move to `arch/riscv-esp32p4/i2c/` at that point.
 *
 * Why the byte-level methods of that class will refuse: Espressif's I2C is not
 * a shift register a caller can drive one byte at a time.  It takes a list of
 * up to eight commands with the payload in a 32-byte FIFO and runs the whole
 * list on one trigger.  There is no way to hold the bus between calls, so
 * Start, Address, PutByte and GetByte have no honest implementation and
 * WriteRead has an exact one.  The DesignWare driver reaches the same
 * conclusion for the same reason.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"

#define I2C_MAX_WRITE   30      /* the TX FIFO also carries both address bytes */
#define I2C_FIFO_CHUNK  30      /* bounded room for one RX command segment */
#define I2C_MAX_READ    255     /* one hardware command byte-count domain */

static unsigned long i2c_base;
static unsigned long i2c_div_reg;
static unsigned int  i2c_ready;
static unsigned long i2c_last_raw, i2c_last_sr;

static void i2c_wr(unsigned long off, unsigned long v)
{
    p4_w32(i2c_base + off, v);
}

static unsigned long i2c_rd(unsigned long off)
{
    return p4_r32(i2c_base + off);
}

/* Every configuration write needs this before the hardware sees it. */
static void i2c_commit(void)
{
    i2c_wr(P4_I2C_CTR, i2c_rd(P4_I2C_CTR) | P4_I2C_CONF_UPGATE);
}

/*
 * Route one pin to the matrix as a bidirectional peripheral line.
 *
 * Both directions are set for both pins, SCL included.  A master drives SCL
 * and would seem to need only the output path, but the controller reads SCL
 * back to detect a slave stretching the clock, and without the input path it
 * would see its own idea of the line rather than the line.
 *
 * The pad keeps its pull-up enabled and is left in open-drain by the
 * peripheral itself; nothing here forces a level, which matters because a
 * driven high on an I2C bus is a short against any device pulling low.
 */
static void i2c_route_pin(unsigned int gpio, unsigned int signal)
{
    unsigned long iomux = P4_IOMUX_BASE + P4_IOMUX_PIN(gpio);
    unsigned long v;

    /* GPIO function on the pad, input enabled, pull-up on, weakest drive
       that works: the bus has external pull-ups and this only ever pulls
       down. */
    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_DRV_M | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
    v |= P4_IOMUX_FUN_IE | P4_IOMUX_FUN_PU;
    v |= 2UL << P4_IOMUX_FUN_DRV_S;
    p4_w32(iomux, v);

    /* The signal drives the pin, and the peripheral owns the output enable
       rather than the GPIO register: that is what P4_GPIO_OEN_SEL clear
       means, and it is how the line becomes open-drain. */
    v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio));
    v &= ~(P4_GPIO_OUT_SEL_MASK | P4_GPIO_OEN_SEL);
    v |= (unsigned long)signal & P4_GPIO_OUT_SEL_MASK;
    p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio), v);

    /* And the signal is read back from that same pin. */
    v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_IN_SEL(signal));
    v &= ~P4_GPIO_IN_SEL_MASK;
    v |= (unsigned long)gpio & P4_GPIO_IN_SEL_MASK;
    p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_IN_SEL(signal), v);
}

/*
 * Bus timing, computed rather than tabulated.
 *
 * Source is the 40 MHz crystal, deliberately: it is the one clock on this
 * chip that does not move when the CPU, memory or peripheral dividers do, and
 * B1 has just demonstrated how easily those turn out to be somewhere
 * unexpected.  An I2C bus whose speed depends on a divider nobody configured
 * is a bus that works until something else changes.
 *
 * The arithmetic is ESP-IDF's, and the three ordering constraints it asserts
 * are checked here rather than assumed: wait_high < sda_sample < scl_high.
 * Those are what make the controller sample the data line inside the high
 * half of the clock rather than at its edges.
 */
static int i2c_set_timing(unsigned long source_hz, unsigned long bus_hz)
{
    unsigned long clkm_div = source_hz / (bus_hz * 1024) + 1;
    unsigned long sclk = source_hz / clkm_div;
    unsigned long half = sclk / bus_hz / 2;
    unsigned long wait_high, high, sda_sample, tout, v;

    wait_high = (bus_hz >= 80000UL) ? (half / 2 - 2) : (half / 4);
    high = half - wait_high;
    sda_sample = half / 2;

    if (!(wait_high < sda_sample && sda_sample < high))
        return 0;
    if (half < 2 || high > 0x1FF || wait_high > 0x7F)
        return 0;

    v = p4_r32(i2c_div_reg);
    v &= ~(P4_I2C_CLK_DIV_NUM_M << P4_I2C_CLK_DIV_NUM_S);
    v |= (clkm_div - 1) << P4_I2C_CLK_DIV_NUM_S;
    p4_w32(i2c_div_reg, v);

    i2c_wr(P4_I2C_SCL_LOW_PERIOD, half - 1);
    i2c_wr(P4_I2C_SCL_HIGH_PERIOD,
           high | (wait_high << P4_I2C_SCL_WAIT_HIGH_S));
    i2c_wr(P4_I2C_SDA_HOLD, half / 4 - 1);
    i2c_wr(P4_I2C_SDA_SAMPLE, sda_sample - 1);
    i2c_wr(P4_I2C_SCL_RSTART_SETUP, half - 1);
    i2c_wr(P4_I2C_SCL_STOP_SETUP, half - 1);
    i2c_wr(P4_I2C_SCL_START_HOLD, half - 1);
    i2c_wr(P4_I2C_SCL_STOP_HOLD, half - 1);

    /* About ten bus cycles, expressed as a power of two the way the register
       wants it. */
    tout = (unsigned long)(32 - __builtin_clz((unsigned int)(5 * half))) + 2;
    if (tout > 0x1F)
        tout = 0x1F;
    i2c_wr(P4_I2C_TO, tout | P4_I2C_TIME_OUT_EN);

    return 1;
}

/*
 * Bring I2C1 up as a master at the requested rate.
 *
 * Returns non-zero on success.  Idempotent: a second call reconfigures from
 * the same starting point, because the reset below discards whatever state a
 * previous one left.
 */
int krnP4I2CInit(unsigned int port, unsigned int sda_gpio,
                 unsigned int scl_gpio, unsigned long bus_hz)
{
    unsigned long v, apb_gate, gate, src, rst, div_reg;
    unsigned int sig_sda, sig_scl;

    i2c_ready = 0;

    if (port == 0)
    {
        i2c_base = P4_I2C0_BASE;
        apb_gate = P4_I2C0_APB_CLK_EN;
        gate     = P4_I2C0_CLK_EN;
        src      = P4_I2C0_CLK_SRC_SEL;
        rst      = P4_RST_EN_I2C0;
        div_reg  = P4_CLKRST_PERI_CLK_CTRL10;
        sig_sda  = P4_SIG_I2C0_SDA;
        sig_scl  = P4_SIG_I2C0_SCL;
    }
    else if (port == 1)
    {
        i2c_base = P4_I2C1_BASE;
        apb_gate = P4_I2C1_APB_CLK_EN;
        gate     = P4_I2C1_CLK_EN;
        src      = P4_I2C1_CLK_SRC_SEL;
        rst      = P4_RST_EN_I2C1;
        div_reg  = P4_CLKRST_PERI_CLK_CTRL11;
        sig_sda  = P4_SIG_I2C1_SDA;
        sig_scl  = P4_SIG_I2C1_SCL;
    }
    else
        return 0;

    i2c_div_reg = div_reg;

    /* Gates on, then out of reset.  The registers do not answer without the
       APB clock, so a reset released first would be released into nothing. */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL2,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL2) | apb_gate);

    v = p4_r32(P4_CLKRST_PERI_CLK_CTRL10);
    v |= gate;
    v &= ~src;                          /* the crystal, see i2c_set_timing */
    p4_w32(P4_CLKRST_PERI_CLK_CTRL10, v);

    p4_w32(P4_CLKRST_HP_RST_EN1, p4_r32(P4_CLKRST_HP_RST_EN1) | rst);
    p4_w32(P4_CLKRST_HP_RST_EN1, p4_r32(P4_CLKRST_HP_RST_EN1) & ~rst);

    /*
     * Pins after the peripheral, not before.
     *
     * Routing a pin to a peripheral that is still in reset connects the pad
     * to an output whose level is undefined, and on a shared bus that is a
     * pulse other devices see.  With the controller already configured as a
     * master the line it presents is idle-high through the pull-up.
     */
    /*
     * Open drain, which is what leaving SDA_FORCE_OUT and SCL_FORCE_OUT clear
     * means.  Setting them is direct output, and this driver did until the bus
     * scan found exactly one device where four were expected: with the master
     * driving SDA high through the acknowledge slot, a slave pulling it low is
     * fighting a push-pull driver, and only the device that happens to win
     * that contest is ever seen.  ESP-IDF never touches these two bits and so
     * runs on the reset default, which is open drain.
     */
    i2c_wr(P4_I2C_CTR, P4_I2C_MS_MODE | P4_I2C_CLK_EN | P4_I2C_ARBITRATION_EN);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST
                           | P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_FILTER_CFG, 0x707);   /* both lines filtered, 7 cycles */
    i2c_wr(P4_I2C_INT_ENA, 0);
    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);

    if (!i2c_set_timing(P4_XTAL_HZ, bus_hz))
        return 0;

    i2c_commit();

    i2c_route_pin(sda_gpio, sig_sda);
    i2c_route_pin(scl_gpio, sig_scl);

    i2c_ready = 1;
    return 1;
}

/* Reset the state machine and both FIFOs, for use after any failure. */
static void i2c_recover(void)
{
    i2c_wr(P4_I2C_CTR, i2c_rd(P4_I2C_CTR) | P4_I2C_FSM_RST);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST
                           | P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);
    i2c_commit();
}

/*
 * Wait for the command list to finish.
 *
 * Bounded by the system timer rather than a loop count.  The first version
 * counted iterations, and on a bus with nothing on it - where neither a NACK
 * nor the controller's own timeout ever arrives - the full count took long
 * enough that scanning 112 addresses looked like a hang.  A loop count is also
 * a bound that changes meaning whenever the CPU clock does, which B1 has just
 * shown can happen without anyone noticing.
 *
 * 50 ms is generous for one command segment: every segment is at most 30
 * bytes, which at the slowest rate used here takes about 30 ms.  A longer read
 * is split at END commands and receives a fresh bounded wait per segment.
 *
 * Returns the transport result, distinguishing a slave that said no from a bus
 * that stopped answering, because the two need different responses from a
 * caller: a NACK at an address means nothing is there, and a timeout means
 * something is holding the line.
 */
#define I2C_WAIT_TICKS  (P4_SYSTIMER_HZ / 20)   /* 50 ms */

static int i2c_wait_for(unsigned long complete_mask)
{
    uint64_t deadline = krnTimerCount() + I2C_WAIT_TICKS;
    unsigned long raw;

    for (;;)
    {
        raw = i2c_rd(P4_I2C_INT_RAW);
        i2c_last_raw = raw;
        i2c_last_sr = i2c_rd(P4_I2C_SR);

        if (raw & P4_I2C_NACK_INT)
        {
            i2c_recover();
            return P4_I2C_NACK;
        }
        if (raw & P4_I2C_ARB_LOST_INT)
        {
            i2c_recover();
            return P4_I2C_ARBLOST;
        }
        if (raw & P4_I2C_TIME_OUT_INT)
        {
            i2c_recover();
            return P4_I2C_TIMEOUT;
        }
        if (raw & complete_mask)
            return P4_I2C_OK;

        if (krnTimerCount() > deadline)
        {
            i2c_recover();
            return P4_I2C_STUCK;
        }
    }
}

static int i2c_wait(void)
{
    return i2c_wait_for(P4_I2C_TRANS_COMPLETE_INT);
}

/*
 * One transfer: write wlen bytes, then optionally read rlen bytes after a
 * repeated start, then stop.
 *
 * This is the whole transport, and it is deliberately the only entry point
 * that moves data.  Everything a device driver needs is a special case of it,
 * and the shape matches the WriteRead method of AROS's i2c HIDD class so the
 * eventual wrapper is a translation and not a reimplementation.
 */
int krnP4I2CTransfer(unsigned int address,
                     const unsigned char *wbuf, unsigned int wlen,
                     unsigned char *rbuf, unsigned int rlen)
{
    unsigned int cmd = 0, i, remaining = rlen, copied = 0;
    unsigned int chunk = remaining > I2C_FIFO_CHUNK
                       ? I2C_FIFO_CHUNK : remaining;
    unsigned long complete_mask = remaining > chunk
                                ? P4_I2C_END_DETECT_INT
                                : P4_I2C_TRANS_COMPLETE_INT;
    int result;

    if (!i2c_ready)
        return P4_I2C_NOTREADY;
    if (wlen > I2C_MAX_WRITE || rlen > I2C_MAX_READ)
        return P4_I2C_TOOLONG;
    if (wlen == 0 && rlen == 0)
        return P4_I2C_OK;

    if (i2c_rd(P4_I2C_SR) & P4_I2C_BUS_BUSY)
    {
        i2c_recover();
        if (i2c_rd(P4_I2C_SR) & P4_I2C_BUS_BUSY)
            return P4_I2C_BUSY;
    }

    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST
                           | P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);

    /*
     * The write half.  The address byte is part of the payload, not a
     * separate register: the controller sends whatever the FIFO holds and
     * only the ack-check flag on the command tells it to care about the
     * response.
     */
    i2c_wr(P4_I2C_COMD(cmd++),
           ((unsigned long)P4_I2C_CMD_RSTART << P4_I2C_CMD_OP_S));

    i2c_wr(P4_I2C_DATA, (address << 1) | 0);
    for (i = 0; i < wlen; ++i)
        i2c_wr(P4_I2C_DATA, wbuf[i]);

    i2c_wr(P4_I2C_COMD(cmd++),
           ((unsigned long)P4_I2C_CMD_WRITE << P4_I2C_CMD_OP_S)
           | P4_I2C_CMD_ACK_CHECK_EN | (wlen + 1));

    if (remaining)
    {
        i2c_wr(P4_I2C_COMD(cmd++),
               ((unsigned long)P4_I2C_CMD_RSTART << P4_I2C_CMD_OP_S));
        i2c_wr(P4_I2C_DATA, (address << 1) | 1);
        i2c_wr(P4_I2C_COMD(cmd++),
               ((unsigned long)P4_I2C_CMD_WRITE << P4_I2C_CMD_OP_S)
               | P4_I2C_CMD_ACK_CHECK_EN | 1);

        /*
         * Every byte but the last is acknowledged and the last is not, which
         * is how the slave is told to release the bus.  Acknowledging the
         * last byte too leaves the device expecting to send another and the
         * following start condition then arrives mid-transfer.
         */
        if (remaining > chunk)
        {
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                   | chunk);
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_END << P4_I2C_CMD_OP_S));
        }
        else
        {
            if (chunk > 1)
                i2c_wr(P4_I2C_COMD(cmd++),
                       ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                       | (chunk - 1));
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                   | P4_I2C_CMD_ACK_VALUE | 1);
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_STOP << P4_I2C_CMD_OP_S));
        }
    }
    else
        i2c_wr(P4_I2C_COMD(cmd++),
               ((unsigned long)P4_I2C_CMD_STOP << P4_I2C_CMD_OP_S));

    i2c_commit();
    i2c_wr(P4_I2C_CTR, i2c_rd(P4_I2C_CTR) | P4_I2C_TRANS_START);

    result = i2c_wait_for(complete_mask);
    if (result != P4_I2C_OK)
        return result;

    for (i = 0; i < chunk; ++i)
        rbuf[copied + i] = (unsigned char)(i2c_rd(P4_I2C_DATA) & 0xFF);
    copied += chunk;
    remaining -= chunk;

    /*
     * END pauses the command engine without putting a STOP on the wire.  This
     * is the P4 controller's native way to drain a full RX FIFO while keeping
     * one slave read transaction coherent.  Continue with READ commands only:
     * the repeated-start and read address were already sent by the first
     * segment.  The final byte is NACKed before STOP, exactly as in a short
     * read.  No FIFO reset is permitted between segments because that would
     * discard unread data and reset the transaction state being preserved.
     */
    while (remaining)
    {
        cmd = 0;
        chunk = remaining > I2C_FIFO_CHUNK
              ? I2C_FIFO_CHUNK : remaining;
        complete_mask = remaining > chunk
                      ? P4_I2C_END_DETECT_INT
                      : P4_I2C_TRANS_COMPLETE_INT;
        i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);

        if (remaining > chunk)
        {
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                   | chunk);
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_END << P4_I2C_CMD_OP_S));
        }
        else
        {
            if (chunk > 1)
                i2c_wr(P4_I2C_COMD(cmd++),
                       ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                       | (chunk - 1));
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_READ << P4_I2C_CMD_OP_S)
                   | P4_I2C_CMD_ACK_VALUE | 1);
            i2c_wr(P4_I2C_COMD(cmd++),
                   ((unsigned long)P4_I2C_CMD_STOP << P4_I2C_CMD_OP_S));
        }

        i2c_commit();
        i2c_wr(P4_I2C_CTR, i2c_rd(P4_I2C_CTR) | P4_I2C_TRANS_START);
        result = i2c_wait_for(complete_mask);
        if (result != P4_I2C_OK)
            return result;
        for (i = 0; i < chunk; ++i)
            rbuf[copied + i] =
                (unsigned char)(i2c_rd(P4_I2C_DATA) & 0xFF);
        copied += chunk;
        remaining -= chunk;
    }

    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);
    return P4_I2C_OK;
}

/*
 * Is anything at this address?
 *
 * A zero-length write, which is a start, the address, and a stop.  The device
 * has to acknowledge the address byte and nothing else happens, so this
 * cannot disturb a device that is there and cannot hang on one that is not.
 */
/* The last transfer's raw interrupt and status words, for diagnosis only. */
void krnP4I2CLastStatus(unsigned long *raw, unsigned long *sr)
{
    if (raw)
        *raw = i2c_last_raw;
    if (sr)
        *sr = i2c_last_sr;
}

int krnP4I2CProbe(unsigned int address)
{
    unsigned int cmd = 0;
    int result;

    if (!i2c_ready)
        return P4_I2C_NOTREADY;

    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_TX_FIFO_RST | P4_I2C_RX_FIFO_RST
                           | P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_FIFO_CONF, P4_I2C_FIFO_PRT_EN);
    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);

    i2c_wr(P4_I2C_COMD(cmd++),
           ((unsigned long)P4_I2C_CMD_RSTART << P4_I2C_CMD_OP_S));
    i2c_wr(P4_I2C_DATA, (address << 1) | 0);
    i2c_wr(P4_I2C_COMD(cmd++),
           ((unsigned long)P4_I2C_CMD_WRITE << P4_I2C_CMD_OP_S)
           | P4_I2C_CMD_ACK_CHECK_EN | 1);
    i2c_wr(P4_I2C_COMD(cmd++),
           ((unsigned long)P4_I2C_CMD_STOP << P4_I2C_CMD_OP_S));

    i2c_commit();
    i2c_wr(P4_I2C_CTR, i2c_rd(P4_I2C_CTR) | P4_I2C_TRANS_START);

    result = i2c_wait();
    i2c_wr(P4_I2C_INT_CLR, 0xFFFFFFFFUL);
    return result;
}
