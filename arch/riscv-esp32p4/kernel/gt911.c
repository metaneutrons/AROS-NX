/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Goodix GT911 touch controller (JC1060P470C).
*/

/*
 * The GT911 runs from its own ROM, so unlike the D1001's GSL3670 there is no
 * firmware to load: reset it, read its identity and configured resolution,
 * then poll its report buffer. Register addresses are 16 bit, sent high byte
 * first; multi-byte values in the registers are little endian.
 *
 *   0x8140  product ID, four ASCII bytes ("911\0")
 *   0x8144  firmware version, 16 bit
 *   0x8146  X resolution, 0x8148 Y resolution (the active configuration)
 *   0x814E  status: bit 7 a fresh report is ready, bits 0-3 contacts (<= 5)
 *   0x814F  first contact, eight bytes each: track ID, X, Y, size, reserved
 *
 * After a report is read the status register has to be written back to zero,
 * or the controller never fills the buffer again.
 *
 * The address is chosen by the level of INT while RST rises: low gives 0x5D,
 * high 0x14. This driver drives INT low through the reset and then releases
 * it, so it expects 0x5D and tries 0x14 only if that does not answer.
 *
 * All of this is from the GT911 programming guide as the vendor demos use it,
 * not yet measured on the board; see ROADMAP D1/J for the open gate.
 */

#include <inttypes.h>
#include <aros/touchscreen.h>
#include <exec/types.h>

#include "hardware.h"
#include "kernel_intern.h"

#if defined(P4_BOARD_TOUCH_GT911) \
    && (defined(P4_C4_TOUCH_HIDD) || defined(P4_C4_TOUCH_PROBE))

#define GT911_REG_PRODUCT_ID    0x8140
#define GT911_REG_STATUS        0x814E
#define GT911_STATUS_READY      0x80
#define GT911_STATUS_COUNT      0x0F
#define GT911_MAX_CONTACTS      5
#define GT911_CONTACT_BYTES     8
#define GT911_REPORT_BYTES      (1 + GT911_MAX_CONTACTS * GT911_CONTACT_BYTES)

/*
 * 100 kHz first. The D1001's I2C0 only answers reliably at 10 kHz, which is
 * recorded there as a defect of this transport, not of the device; if the
 * GT911 shows the same, the slower rate is the fallback rather than a guess.
 */
static const unsigned long gt911_rates[] = { 100000UL, 10000UL };

static unsigned int gt911_addr;
static unsigned long gt911_hz;
static unsigned int gt911_res_x, gt911_res_y;
static int gt911_started;
static struct KrnTouchScreenFrame gt911_last;

static void gt911_pin_output(unsigned int gpio, int high)
{
    unsigned long iomux = P4_IOMUX_BASE + P4_IOMUX_PIN(gpio);
    unsigned long v;

    /* Level first, then the driver, so the pin never shows the other one. */
    p4_w32(P4_GPIO_BASE + (high ? P4_GPIO_OUT_W1TS : P4_GPIO_OUT_W1TC),
           1UL << gpio);
    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_PU | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
    v |= P4_IOMUX_FUN_IE;
    p4_w32(iomux, v);
    v = p4_r32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio));
    v &= ~P4_GPIO_OUT_SEL_MASK;
    v |= P4_GPIO_OUT_SEL_GPIO | P4_GPIO_OEN_SEL;
    p4_w32(P4_GPIO_BASE + P4_GPIO_FUNC_OUT_SEL(gpio), v);
    p4_w32(P4_GPIO_BASE + P4_GPIO_ENABLE_W1TS, 1UL << gpio);
}

/* Floating input: the controller drives INT once it is running. */
static void gt911_pin_input(unsigned int gpio)
{
    unsigned long iomux = P4_IOMUX_BASE + P4_IOMUX_PIN(gpio);
    unsigned long v;

    p4_w32(P4_GPIO_BASE + P4_GPIO_ENABLE_W1TC, 1UL << gpio);
    v = p4_r32(iomux);
    v &= ~(P4_IOMUX_MCU_SEL_M | P4_IOMUX_FUN_PU | P4_IOMUX_FUN_PD);
    v |= (unsigned long)P4_IOMUX_FUNC_GPIO << P4_IOMUX_MCU_SEL_S;
    v |= P4_IOMUX_FUN_IE;
    p4_w32(iomux, v);
}

/*
 * The reset that selects 0x5D: RST and INT low, RST high after 10 ms with
 * INT still low, INT released 10 ms later, then 50 ms before the first
 * transfer. The guide's minimums are 100 us, 5 ms and 50 ms; the 100-Hz
 * timer rounds the first two up.
 */
static void gt911_reset(void)
{
    gt911_pin_output(P4_BOARD_TOUCH_IRQ_GPIO, 0);
    gt911_pin_output(P4_BOARD_TOUCH_RST_GPIO, 0);
    krnTimerWait(1);
    gt911_pin_output(P4_BOARD_TOUCH_RST_GPIO, 1);
    krnTimerWait(1);
    gt911_pin_input(P4_BOARD_TOUCH_IRQ_GPIO);
    krnTimerWait(P4_TICK_HZ / 20);
}

static int gt911_read(unsigned int addr, unsigned int reg,
                      unsigned char *out, unsigned int bytes)
{
    unsigned char r[2];

    r[0] = (unsigned char)(reg >> 8);
    r[1] = (unsigned char)reg;
    return krnP4I2CTransfer(addr, r, 2, out, bytes);
}

static int gt911_write8(unsigned int reg, unsigned char value)
{
    unsigned char w[3];

    w[0] = (unsigned char)(reg >> 8);
    w[1] = (unsigned char)reg;
    w[2] = value;
    return krnP4I2CTransfer(gt911_addr, w, 3, NULL, 0);
}

static int gt911_select_bus(unsigned long hz)
{
    return krnP4I2CInit(0, P4_BOARD_I2C0_SDA_GPIO, P4_BOARD_I2C0_SCL_GPIO, hz)
         ? P4_I2C_OK : P4_I2C_NOTREADY;
}

/*
 * Reset, find the controller at either address and rate, and read its
 * identity block. Fills id[11] (product ID, firmware, resolution, vendor).
 */
static int gt911_start(unsigned char id[11])
{
    static const unsigned int addrs[] =
        { P4_BOARD_TOUCH_ADDR, P4_BOARD_TOUCH_ADDR_ALT };
    unsigned int a, s;
    int r = P4_I2C_NOTREADY;

    gt911_started = 0;
    gt911_reset();
    for (s = 0; s < sizeof(gt911_rates) / sizeof(gt911_rates[0]); ++s)
    {
        if (gt911_select_bus(gt911_rates[s]) != P4_I2C_OK)
            return P4_I2C_NOTREADY;
        for (a = 0; a < sizeof(addrs) / sizeof(addrs[0]); ++a)
        {
            r = gt911_read(addrs[a], GT911_REG_PRODUCT_ID, id, 11);
            if (r == P4_I2C_OK)
            {
                gt911_addr = addrs[a];
                gt911_hz = gt911_rates[s];
                gt911_res_x = (unsigned int)id[6] | ((unsigned int)id[7] << 8);
                gt911_res_y = (unsigned int)id[8] | ((unsigned int)id[9] << 8);
                /* Clear a report left from before the reset. */
                (void)gt911_write8(GT911_REG_STATUS, 0);
                gt911_started = 1;
                return P4_I2C_OK;
            }
        }
    }
    return r;
}

/*
 * The controller reports in its configured resolution; the vendor demo notes
 * one batch configured for 800 x 480 on this 1024 x 600 panel. Scaling here
 * keeps the public range fixed at the panel size whatever the configuration.
 */
static ULONG gt911_scale(ULONG v, unsigned int res, unsigned int size)
{
    if (!res || res == size)
        return v < size ? v : size - 1;
    v = (v * size + res / 2) / res;
    return v < size ? v : size - 1;
}

#ifdef P4_C4_TOUCH_HIDD
static BOOL gt911_touch_acquire(void)
{
    unsigned char id[11];

    if (gt911_started)
        return gt911_select_bus(gt911_hz) == P4_I2C_OK;
    return gt911_start(id) == P4_I2C_OK;
}

/* The bus is the GT911's alone in this port; nothing to hand back. */
static VOID gt911_touch_release(void)
{
}

/*
 * One report. A buffer that is not ready yet means no new frame since the
 * last read, not "all fingers lifted", so the previous frame is repeated;
 * returning an empty frame there would end every hold after one poll.
 */
static BOOL gt911_read_contacts(struct KrnTouchScreenFrame *frame)
{
    unsigned char data[GT911_REPORT_BYTES];
    ULONG reported, i;
    int r;

    if (!frame)
        return FALSE;
    r = gt911_read(gt911_addr, GT911_REG_STATUS, data, GT911_REPORT_BYTES);
    if (r != P4_I2C_OK)
        return FALSE;

    if (!(data[0] & GT911_STATUS_READY))
    {
        *frame = gt911_last;
        return TRUE;
    }

    reported = data[0] & GT911_STATUS_COUNT;
    frame->reported_count = reported;
    frame->count = 0;
    if (reported > GT911_MAX_CONTACTS)
        reported = GT911_MAX_CONTACTS;
    for (i = 0; i < reported; ++i)
    {
        const unsigned char *c = &data[1 + i * GT911_CONTACT_BYTES];
        struct KrnTouchScreenContact *contact = &frame->contact[frame->count++];

        contact->id = c[0];
        contact->x = gt911_scale((ULONG)c[1] | ((ULONG)c[2] << 8),
                                 gt911_res_x, P4_LOGICAL_W);
        contact->y = gt911_scale((ULONG)c[3] | ((ULONG)c[4] << 8),
                                 gt911_res_y, P4_LOGICAL_H);
    }
    for (i = frame->count; i < KRN_TOUCHSCREEN_MAX_CONTACTS; ++i)
    {
        frame->contact[i].id = 0;
        frame->contact[i].x = 0;
        frame->contact[i].y = 0;
    }
    gt911_last = *frame;

    return gt911_write8(GT911_REG_STATUS, 0) == P4_I2C_OK;
}

static struct KrnTouchScreenOps gt911_touchscreen_ops =
{
    KRN_TOUCHSCREEN_OPS_VERSION,
    P4_LOGICAL_W,
    P4_LOGICAL_H,
    P4_LOGICAL_W,
    P4_LOGICAL_H,
    NULL,
    gt911_touch_acquire,
    gt911_touch_release,
    gt911_read_contacts
};

struct KrnTouchScreenOps *krnP4GT911TouchScreenOps(void)
{
    return &gt911_touchscreen_ops;
}
#endif /* P4_C4_TOUCH_HIDD */

/* Boot-time identification for the log; the HIDD starts it again later. */
void krnP4GT911Probe(void)
{
    unsigned char id[11];
    int r;

    krnP4PutStr("[touch]  GT911 reset on GPIO");
    krnP4PutDec(P4_BOARD_TOUCH_RST_GPIO);
    krnP4PutStr(", I2C0 sda ");
    krnP4PutDec(P4_BOARD_I2C0_SDA_GPIO);
    krnP4PutStr(" scl ");
    krnP4PutDec(P4_BOARD_I2C0_SCL_GPIO);
    krnP4PutStr("\n");

    r = gt911_start(id);
    if (r != P4_I2C_OK)
    {
        krnP4PutStr("[touch]  GT911 did not answer at 0x5d or 0x14: ");
        krnP4PutStr(r == P4_I2C_NACK      ? "no answer"
                  : r == P4_I2C_TIMEOUT   ? "bus timeout"
                  : r == P4_I2C_NOTREADY  ? "controller not initialised"
                                          : "transfer error");
        krnP4PutStr("; no touch device\n");
        return;
    }
    krnP4PutStr("[touch]  GT911 at ");
    krnP4PutHex32(gt911_addr);
    krnP4PutStr(", ");
    krnP4PutDec((uint32_t)(gt911_hz / 1000));
    krnP4PutStr(" kHz, product '");
    for (r = 0; r < 4 && id[r] >= 0x20 && id[r] < 0x7F; ++r)
        krnP4PutC((char)id[r]);
    krnP4PutStr("' firmware ");
    krnP4PutHex32((uint32_t)id[4] | ((uint32_t)id[5] << 8));
    krnP4PutStr(", resolution ");
    krnP4PutDec(gt911_res_x);
    krnP4PutStr("x");
    krnP4PutDec(gt911_res_y);
    krnP4PutStr(gt911_res_x == P4_LOGICAL_W && gt911_res_y == P4_LOGICAL_H
                ? "\n" : ", scaled to the panel\n");
}

#endif /* P4_BOARD_TOUCH_GT911 && (C4 touch) */
