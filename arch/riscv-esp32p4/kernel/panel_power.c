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

    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_PU | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
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
