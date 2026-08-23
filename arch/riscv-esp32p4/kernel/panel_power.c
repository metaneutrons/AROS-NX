/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: D1001 panel power, reset and backlight, through the port expander.
*/

/*
 * The safety rule this file exists to obey.
 *
 * A cold PCA9535 has every pin an input and its output register all ones.
 * Nothing is driven, which is the safe state, and the moment a pin is made an
 * output it starts driving whatever that register holds - all ones being the
 * backlight enabled, the audio amplifier on and the panel out of reset.  So
 * the output latch is written before the direction, always, and only the four
 * pins this port needs become outputs at all.
 *
 * The reference driver does the opposite - `set_dir(0xffff, OUTPUT)` and then
 * the levels one at a time - and gets away with it because it wants the
 * backlight on a moment later anyway.  Here it would flash the panel during a
 * bring-up whose whole point is that it stays dark until B4 has something
 * worth showing.
 *
 * The board is rarely cold, and that is the part this file got wrong first.
 * The expander is not reset by a CPU reset: it keeps its direction and output
 * registers across every reboot, so on this board it is found with all sixteen
 * pins already outputs, left that way by whatever firmware ran last.  Writing
 * a whole-register value then drives eleven pins nobody asked about - the
 * first version of this file pulled the battery-charge enable low that way and
 * the run said so.  Every write here is therefore read-modify-write against
 * the device's own register, and the four owned bits are the only ones any
 * value in this file can change.
 *
 * Every value written is read back and compared, because a write to an I2C
 * device that nobody checked is a hope.  And every failure path leaves reset
 * asserted and the backlight dark, which is the state a retry is safe from.
 */

#include <inttypes.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"

/*
 * The pins this port drives, and the state they start in.
 *
 * PWR_HOLD high because on battery it is what keeps the board alive; on USB it
 * is redundant and harmless, and depending on which of the two is true would
 * make the sequence depend on how the board is powered.
 *
 * LCD_RST low is reset asserted - the line is active low - and it is the value
 * the panel has to see before its supply comes up rather than after.
 */
#define PANEL_OUTPUTS   (P4_EXP_LCD_PWR_EN | P4_EXP_LCD_RST \
                         | P4_EXP_LCD_BL_EN | P4_EXP_PWR_HOLD)
#define PANEL_SAFE      (P4_EXP_PWR_HOLD)

/* The expander's own latch, so a change to one bit does not guess at the
   others.  Only the bits in PANEL_OUTPUTS are ever ours to set. */
static UWORD panel_latch;
static int panel_have_latch;
/* What the device held before this port touched anything, so the report can
   show that nothing outside PANEL_OUTPUTS moved. */
static UWORD panel_found_output, panel_found_config;

static int panel_write16(unsigned char reg, UWORD value)
{
    unsigned char buf[3];

    buf[0] = reg;
    buf[1] = (unsigned char)(value & 0xFF);
    buf[2] = (unsigned char)(value >> 8);

    return krnP4I2CTransfer(P4_PCA9535_ADDR, buf, 3, NULL, 0);
}

static int panel_read16(unsigned char reg, UWORD *value)
{
    unsigned char in[2] = { 0, 0 };
    int r = krnP4I2CTransfer(P4_PCA9535_ADDR, &reg, 1, in, 2);

    if (r == P4_I2C_OK)
        *value = (UWORD)(in[0] | ((UWORD)in[1] << 8));
    return r;
}

/*
 * Write the output register and read it back.
 *
 * The read is of the output register, not the input port: the input port
 * reports the pin, which for an output that something else is also driving is
 * not what was asked for.  Reading back what was written proves the transfer,
 * and the pin's actual level is a separate question this cannot answer.
 */
static int panel_set_outputs(UWORD value)
{
    UWORD back = 0;
    int r;

    r = panel_write16(P4_PCA9535_OUTPUT, value);
    if (r != P4_I2C_OK)
        return r;

    r = panel_read16(P4_PCA9535_OUTPUT, &back);
    if (r != P4_I2C_OK)
        return r;

    if (back != value)
        return P4_I2C_MISMATCH;

    panel_latch = value;
    panel_have_latch = 1;
    return P4_I2C_OK;
}

/* Change only the bits this port owns, leaving the rest of the latch alone. */
static int panel_modify(UWORD set, UWORD clear)
{
    UWORD v;

    if (!panel_have_latch)
        return P4_I2C_NOTREADY;

    v = (UWORD)((panel_latch & ~(clear & PANEL_OUTPUTS))
                | (set & PANEL_OUTPUTS));
    return panel_set_outputs(v);
}

/*
 * The backlight pin, held at zero without enabling its PWM path.
 *
 * B2 must not light the panel, and the honest way to guarantee that is to
 * leave GPIO14 a plain output driven low rather than to configure LEDC with a
 * duty of zero.  A timer that exists can be started by a later mistake; a pin
 * with no timer attached cannot.  B4 attaches LEDC when there is something to
 * show, and until then this is the whole backlight driver.
 */
static void panel_backlight_dark(void)
{
    unsigned long iomux = P4_IOMUX_BASE + P4_IOMUX_PIN(P4_D1001_BACKLIGHT_GPIO);
    unsigned long v;

    /* Drive low before enabling the output, for the same reason the expander's
       latch is written before its direction. */
    p4_w32(P4_GPIO_BASE + P4_GPIO_OUT_W1TC,
           1UL << P4_D1001_BACKLIGHT_GPIO);

    /*
     * The input path is enabled as well as the output, which is not how a
     * plain output pin has to be configured and is how this one is going to
     * be: without it the GPIO input register reads zero whatever the pin is
     * doing, and the first attempt to diagnose a dark backlight spent a run
     * reading a blind register.
     */
    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_PU | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
    v |= P4_IOMUX_FUN_IE;
    p4_w32(iomux, v);

    /* The GPIO register owns the pin, not a peripheral. */
    v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(P4_D1001_BACKLIGHT_GPIO));
    v &= ~P4_GPIO_OUT_SEL_MASK;
    v |= P4_GPIO_OUT_SEL_GPIO | P4_GPIO_OEN_SEL;
    p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(P4_D1001_BACKLIGHT_GPIO), v);

    p4_w32(P4_GPIO_BASE + P4_GPIO_ENABLE_W1TS,
           1UL << P4_D1001_BACKLIGHT_GPIO);
}

/*
 * Claim the four pins, in the order that never drives an unasked-for level.
 *
 * Returns P4_I2C_OK with the expander in the safe state, or a transport error
 * with nothing claimed.  On failure the direction register is untouched, so
 * every pin is still an input and the board is exactly as it was.
 */
int krnP4PanelClaim(struct P4PanelState *out)
{
    UWORD config = 0, back = 0;
    int r;

    if (out)
    {
        out->claimed = 0;
        out->powered = 0;
        out->reset_released = 0;
        out->config = 0;
        out->output = 0;
        out->input = 0;
    }

    panel_have_latch = 0;
    panel_backlight_dark();

    /* What the board looks like before anything is touched, for the record and
       because a direction register that is not all-ones means something else
       has already claimed pins here. */
    r = panel_read16(P4_PCA9535_CONFIG, &config);
    if (r != P4_I2C_OK)
        return r;
    if (out)
        out->config = config;

    r = panel_read16(P4_PCA9535_INPUT, &back);
    if (r == P4_I2C_OK && out)
        out->input = back;

    /*
     * The latch first, so the pins have somewhere safe to go - and built from
     * what the device already holds, so the eleven bits this port does not own
     * keep their levels whether they are outputs or not.
     */
    r = panel_read16(P4_PCA9535_OUTPUT, &panel_found_output);
    if (r != P4_I2C_OK)
        return r;
    panel_found_config = config;

    r = panel_set_outputs((UWORD)((panel_found_output & ~PANEL_OUTPUTS)
                                  | PANEL_SAFE));
    if (r != P4_I2C_OK)
        return r;

    /* Then the direction, for our four pins only.  A zero bit is an output in
       this device, which is why this clears rather than sets. */
    r = panel_write16(P4_PCA9535_CONFIG, (UWORD)(config & ~PANEL_OUTPUTS));
    if (r != P4_I2C_OK)
        return r;

    r = panel_read16(P4_PCA9535_CONFIG, &back);
    if (r != P4_I2C_OK)
        return r;
    if (back != (UWORD)(config & ~PANEL_OUTPUTS))
        return P4_I2C_MISMATCH;

    if (out)
    {
        out->config = back;
        out->output = panel_latch;
        out->found_output = panel_found_output;
        out->found_config = panel_found_config;
        out->claimed = 1;
    }
    return P4_I2C_OK;
}

/* Did anything outside the four owned bits move?  Zero if not. */
UWORD krnP4PanelStrayBits(void)
{
    if (!panel_have_latch)
        return 0;
    return (UWORD)((panel_latch ^ panel_found_output) & ~PANEL_OUTPUTS);
}

/*
 * Panel supply on, then the reset pulse the reference uses.
 *
 * The 50 ms before the pulse is the supply settling; the 5/10/120 pattern is
 * what the JD9365 sees on a working board and is recorded in the display
 * contract as a reference fact rather than a datasheet one.  The last 120 ms
 * is the panel's own initialisation and a command sent inside it is a command
 * to a controller that is not listening yet.
 *
 * Every wait is bounded by construction because krnTimerWait counts a timer
 * that is already running; none of them can become a spin.
 */
int krnP4PanelPowerUp(struct P4PanelState *out)
{
    int r;

    r = panel_modify(P4_EXP_LCD_PWR_EN, 0);
    if (r != P4_I2C_OK)
        return r;
    if (out)
    {
        out->powered = 1;
        out->output = panel_latch;
    }
    krnTimerWait(P4_TICK_HZ / 20);              /* 50 ms */

    r = panel_modify(P4_EXP_LCD_RST, 0);        /* released */
    if (r != P4_I2C_OK)
        return r;
    krnTimerWait(1);                            /* 5 ms, one tick at 100 Hz */

    r = panel_modify(0, P4_EXP_LCD_RST);        /* asserted */
    if (r != P4_I2C_OK)
        return r;
    krnTimerWait(1);                            /* 10 ms */

    r = panel_modify(P4_EXP_LCD_RST, 0);        /* released for good */
    if (r != P4_I2C_OK)
        return r;
    krnTimerWait(P4_TICK_HZ * 12 / 100);        /* 120 ms */

    if (out)
    {
        out->reset_released = 1;
        out->output = panel_latch;
    }
    return P4_I2C_OK;
}

/*
 * The backlight on, at full brightness, and only when asked.
 *
 * B4 wants a low duty and this gives none: GPIO14 is the brightness input and
 * driving it high is 100 per cent.  A low duty needs LEDC, which this port has
 * not brought up, and bringing it up to dim a first test pattern would be work
 * ahead of its purpose.  What matters for safety is the ordering rather than
 * the level - nothing here can light the panel until a caller asks, and no
 * failure path calls it - so the ordering is exact and the level is recorded as
 * met differently.
 *
 * Called only after a pattern is already on the link.  Lighting a panel that is
 * being driven with nothing shows whatever its controller happens to hold.
 */
/*
 * LEDC on GPIO14, and only ever at a modest duty.
 *
 * The first version drove the pin high from the GPIO register and the panel
 * stayed dark with everything reading back correct.  The reference puts a 5 kHz
 * PWM there, and that difference is the whole explanation: a backlight driver
 * whose dimming input wants a switching signal treats a DC level as no signal.
 * So this is not the low duty B4 asks for being added for comfort - the
 * switching is what makes it work at all, and the low duty comes free.
 *
 * The pin is re-routed from the GPIO register to the LEDC output here, and
 * panel_backlight_dark() routes it back and drives it low.  That is why both
 * functions touch the matrix rather than only the level.
 */
static void panel_backlight_pwm(unsigned int percent)
{
    unsigned long v;
    unsigned long duty;

    if (percent > 100)
        percent = 100;
    duty = ((1UL << P4_LEDC_BL_DUTY_RES) * percent) / 100;

    /* Module clocks, then out of reset, then the crystal as the source. */
    p4_w32(P4_CLKRST_SOC_CLK_CTRL3,
           p4_r32(P4_CLKRST_SOC_CLK_CTRL3) | P4_LEDC_APB_CLK_EN);
    v = p4_r32(P4_CLKRST_PERI_CLK_CTRL22);
    v |= P4_LEDC_CLK_EN;
    v &= ~P4_LEDC_CLK_SRC_MASK;             /* 0: the crystal */
    p4_w32(P4_CLKRST_PERI_CLK_CTRL22, v);
    p4_w32(P4_CLKRST_HP_RST_EN1,
           p4_r32(P4_CLKRST_HP_RST_EN1) | P4_RST_EN_LEDC);
    p4_w32(P4_CLKRST_HP_RST_EN1,
           p4_r32(P4_CLKRST_HP_RST_EN1) & ~P4_RST_EN_LEDC);

    /* The peripheral's own clock gate. */
    p4_w32(P4_LEDC_BASE + P4_LEDC_CONF, P4_LEDC_GLOBAL_CLK_EN);

    /* Timer 0: ten bits at 5 kHz from a 40 MHz crystal. */
    p4_w32(P4_LEDC_BASE + P4_LEDC_TIMER0_CONF,
           (unsigned long)P4_LEDC_BL_DUTY_RES
           | ((unsigned long)P4_LEDC_BL_CLK_DIV << P4_LEDC_CLK_DIV_SHIFT)
           | P4_LEDC_TIMER_RST);
    p4_w32(P4_LEDC_BASE + P4_LEDC_TIMER0_CONF,
           (unsigned long)P4_LEDC_BL_DUTY_RES
           | ((unsigned long)P4_LEDC_BL_CLK_DIV << P4_LEDC_CLK_DIV_SHIFT)
           | P4_LEDC_TIMER_PARA_UP);

    /* Channel 0 on timer 0.  The duty register is Q4. */
    p4_w32(P4_LEDC_BASE + P4_LEDC_CH0_HPOINT, 0);
    p4_w32(P4_LEDC_BASE + P4_LEDC_CH0_DUTY, duty << 4);
    p4_w32(P4_LEDC_BASE + P4_LEDC_CH0_CONF0,
           P4_LEDC_SIG_OUT_EN | P4_LEDC_PARA_UP);
    p4_w32(P4_LEDC_BASE + P4_LEDC_CH0_CONF1, P4_LEDC_DUTY_START);

    /* And the pin follows LEDC rather than the GPIO register. */
    v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(P4_D1001_BACKLIGHT_GPIO));
    v &= ~(P4_GPIO_OUT_SEL_MASK | P4_GPIO_OEN_SEL);
    v |= (unsigned long)P4_SIG_LEDC_CH0_OUT;
    p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(P4_D1001_BACKLIGHT_GPIO), v);
    p4_w32(P4_GPIO_BASE + P4_GPIO_ENABLE_W1TS,
           1UL << P4_D1001_BACKLIGHT_GPIO);
}

int krnP4PanelBacklightOn(void)
{
    int r = panel_modify(P4_EXP_LCD_BL_EN, 0);

    if (r != P4_I2C_OK)
        return r;

    panel_backlight_pwm(P4_LEDC_BL_PERCENT);
    return P4_I2C_OK;
}

/*
 * What the backlight path actually looks like, read back rather than assumed.
 *
 * The first attempt reported success and the panel stayed unlit, which means
 * the expander accepted the enable bit - it is read back - and something after
 * that did not happen.  So this reports the expander's latch, the pin's own
 * level from the input register, and the two matrix registers that decide
 * whether the GPIO register drives the pin at all.
 */
void krnP4PanelBacklightState(struct P4BacklightState *out)
{
    out->latch = panel_latch;
    out->pin_level = (p4_r32(P4_GPIO_BASE + P4_GPIO_IN)
                      >> P4_D1001_BACKLIGHT_GPIO) & 1;
    out->out_level = (p4_r32(P4_GPIO_BASE + P4_GPIO_OUT)
                      >> P4_D1001_BACKLIGHT_GPIO) & 1;
    out->out_sel = p4_r32(P4_GPIO_BASE
                          + P4_GPIO_FUNC_OUT_SEL(P4_D1001_BACKLIGHT_GPIO));
    out->iomux = p4_r32(P4_IOMUX_BASE
                        + P4_IOMUX_PIN(P4_D1001_BACKLIGHT_GPIO));
    (void)panel_read16(P4_PCA9535_INPUT, &out->expander_pins);

    /*
     * Is a waveform actually there?
     *
     * One read of the input register cannot say: at a twenty per cent duty it
     * returns zero four times out of five, which is indistinguishable from a
     * pin that is simply low.  Counting many samples can say, and the count
     * that comes back is the duty - which makes this a measurement of the thing
     * that was in doubt rather than a check that a register was written.
     */
    {
        unsigned long i, high = 0;

        for (i = 0; i < 20000; ++i)
            if (p4_r32(P4_GPIO_BASE + P4_GPIO_IN)
                & (1UL << P4_D1001_BACKLIGHT_GPIO))
                ++high;
        out->samples = 20000;
        out->samples_high = high;
    }
}

/*
 * Back to the state a failure should leave behind: reset asserted, backlight
 * dark, panel supply off.  Called on any error and safe to call at any point,
 * including before the pins were claimed.
 */
int krnP4PanelSafe(void)
{
    panel_backlight_dark();

    if (!panel_have_latch)
        return P4_I2C_OK;

    return panel_modify(P4_EXP_PWR_HOLD,
                        (UWORD)(P4_EXP_LCD_PWR_EN | P4_EXP_LCD_RST
                                | P4_EXP_LCD_BL_EN));
}
