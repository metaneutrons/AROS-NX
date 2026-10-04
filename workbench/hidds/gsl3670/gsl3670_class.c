#define DEBUG 0
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Silead GSL3670 touch controller.

    The controller loses its program when it loses power, so Start loads
    it every time: reset, clear and reset the registers, write the image
    record by record, start, reset the registers again, start, and check
    that the status register reads 0x5A5A5A5A. The sequence and the
    register meanings follow the vendor driver.

    The image is a sequence of eight-byte records, a little-endian offset
    and value each. Offset 0xF0 selects a page and takes one byte; every
    other offset is a word-aligned register below 0x80 and takes four.
    Every record is checked before the first byte is sent, so a malformed
    file cannot leave the controller half programmed.

    A report is register 0x80, 44 bytes in one transaction: byte 0 the
    contact count, then four bytes per contact with 12-bit Y, 12-bit X and
    a 4-bit ID. A non-zero high nibble next to Y marks auxiliary data, not
    a contact.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>

#include <aros/symbolsets.h>

#include <devices/timer.h>
#include <exec/io.h>
#include <exec/types.h>
#include <hidd/hidd.h>
#include <hidd/i2c.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#include LC_LIBDEFS_FILE
#include "gsl3670_intern.h"

#define GSL_REG_REPORT          0x80
#define GSL_REG_STATUS          0xB0
#define GSL_STATUS_RUNNING      0x5A5A5A5AUL
#define GSL_REPORT_BYTES        44
#define GSL_CONTACT_OFFSET      4
#define GSL_CONTACT_BYTES       4
#define GSL_PAGE_OFFSET         0xF0

#define GSL_RESET_MS            20
#define GSL_SHORT_MS            10
#define GSL_STATUS_MS           30

static CONST_STRPTR gslChipName = "GSL3670";

struct gsl_timer
{
    struct MsgPort     *port;
    struct timerequest *tr;
};

static BOOL gsl_timer_open(struct gsl_timer *t)
{
    t->port = CreateMsgPort();
    t->tr = t->port
          ? (struct timerequest *)CreateIORequest(t->port, sizeof(*t->tr))
          : NULL;
    if (t->tr && !OpenDevice(TIMERNAME, UNIT_MICROHZ, &t->tr->tr_node, 0))
        return TRUE;
    if (t->tr)
        DeleteIORequest(&t->tr->tr_node);
    if (t->port)
        DeleteMsgPort(t->port);
    t->tr = NULL;
    t->port = NULL;
    return FALSE;
}

static void gsl_timer_close(struct gsl_timer *t)
{
    CloseDevice(&t->tr->tr_node);
    DeleteIORequest(&t->tr->tr_node);
    DeleteMsgPort(t->port);
}

/* At least ms milliseconds; the caller's task sleeps. */
static void gsl_delay(struct gsl_timer *t, ULONG ms)
{
    t->tr->tr_node.io_Command = TR_ADDREQUEST;
    t->tr->tr_time.tv_secs = ms / 1000;
    t->tr->tr_time.tv_micro = (ms % 1000) * 1000;
    DoIO(&t->tr->tr_node);
}

static BOOL gsl_xfer(OOP_Class *cl, OOP_Object *dev,
                     UBYTE *w, ULONG wlen, UBYTE *r, ULONG rlen)
{
    struct pHidd_I2CDevice_WriteRead msg;

    msg.mID = GSD(cl)->midWriteRead;
    msg.writeBuffer = w;
    msg.writeLength = wlen;
    msg.readBuffer = r;
    msg.readLength = rlen;
    return (BOOL)OOP_DoMethod(dev, (OOP_Msg)&msg);
}

static BOOL gsl_write(OOP_Class *cl, OOP_Object *dev, UBYTE reg,
                      ULONG value, ULONG bytes)
{
    UBYTE data[5];
    ULONG i;

    data[0] = reg;
    for (i = 0; i < bytes; ++i)
        data[i + 1] = (UBYTE)(value >> (i * 8));
    return gsl_xfer(cl, dev, data, bytes + 1, NULL, 0);
}

static BOOL gsl_read32(OOP_Class *cl, OOP_Object *dev, UBYTE reg,
                       ULONG *value)
{
    UBYTE data[4] = {0, 0, 0, 0};

    if (!gsl_xfer(cl, dev, &reg, 1, data, 4))
        return FALSE;
    *value = (ULONG)data[0] | ((ULONG)data[1] << 8)
           | ((ULONG)data[2] << 16) | ((ULONG)data[3] << 24);
    return TRUE;
}

static ULONG gsl_le32(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16)
         | ((ULONG)p[3] << 24);
}

/* The chip's reset line, through the board: low, then high. */
static BOOL gsl_hardware_reset(struct gsl_data *data, OOP_Object *o,
                               struct gsl_timer *t)
{
    struct HIDD_TouchLineMsg msg;

    if (!data->lines)
        return TRUE;
    msg.line = vHidd_TouchLine_Reset;
    msg.level = 0;
    if (!CALLHOOKPKT(data->lines, o, &msg))
        return FALSE;
    gsl_delay(t, GSL_RESET_MS);
    msg.level = 1;
    if (!CALLHOOKPKT(data->lines, o, &msg))
        return FALSE;
    gsl_delay(t, GSL_RESET_MS);
    return TRUE;
}

static BOOL gsl_clear_registers(OOP_Class *cl, struct gsl_data *data,
                                OOP_Object *o, struct gsl_timer *t)
{
    if (!gsl_hardware_reset(data, o, t)
        || !gsl_write(cl, data->dev, 0x88, 0x01, 1))
        return FALSE;
    gsl_delay(t, GSL_SHORT_MS);
    if (!gsl_write(cl, data->dev, 0xE4, 0x04, 1))
        return FALSE;
    gsl_delay(t, GSL_SHORT_MS);
    if (!gsl_write(cl, data->dev, 0xE0, 0x00, 1))
        return FALSE;
    gsl_delay(t, GSL_RESET_MS);
    return TRUE;
}

static BOOL gsl_reset_registers(OOP_Class *cl, struct gsl_data *data,
                                OOP_Object *o, struct gsl_timer *t)
{
    if (!gsl_hardware_reset(data, o, t)
        || !gsl_write(cl, data->dev, 0xE4, 0x04, 1))
        return FALSE;
    gsl_delay(t, GSL_SHORT_MS);
    if (!gsl_write(cl, data->dev, 0xBC, 0x00000000UL, 4))
        return FALSE;
    gsl_delay(t, GSL_SHORT_MS);
    return TRUE;
}

static BOOL gsl_start(OOP_Class *cl, struct gsl_data *data,
                      struct gsl_timer *t)
{
    if (!gsl_write(cl, data->dev, 0xE0, 0x00, 1))
        return FALSE;
    gsl_delay(t, GSL_SHORT_MS);
    return TRUE;
}

/* Every record valid and at least one page selected; FALSE names the
   first bad record in *bad. */
static BOOL gsl_check_image(const UBYTE *image, ULONG records, ULONG *bad)
{
    ULONG i, pages = 0;

    for (i = 0; i < records; ++i)
    {
        ULONG offset = gsl_le32(image + i * 8);
        ULONG value = gsl_le32(image + i * 8 + 4);

        if (offset == GSL_PAGE_OFFSET)
        {
            if (value > 0xFF)
                break;
            ++pages;
        }
        else if (offset > 0x7C || (offset & 3))
            break;
    }
    *bad = i;
    return i == records && pages;
}

OOP_Object *GSL__Root__New(OOP_Class *cl, OOP_Object *o,
                           struct pRoot_New *msg)
{
    OOP_Object *bus = (OOP_Object *)GetTagData(aHidd_TouchController_Bus, 0,
                                               msg->attrList);
    IPTR addr = GetTagData(aHidd_TouchController_Address, GSL_ADDR_DEFAULT,
                           msg->attrList);
    IPTR bytes = GetTagData(aHidd_TouchController_FirmwareBytes, 0,
                            msg->attrList);

    if (!bus || !addr || addr > 0x7F || !bytes || (bytes & 7))
    {
        bug("[GSL3670] refused: bus %p, address %02lx, firmware %lu bytes\n",
            bus, (ULONG)addr, (ULONG)bytes);
        return NULL;
    }
    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        struct gsl_data *data = OOP_INST_DATA(cl, o);
        struct TagItem tags[] =
        {
            {aHidd_I2CDevice_Driver,  (IPTR)bus},
            {aHidd_I2CDevice_Address, addr << 1},
            {aHidd_I2CDevice_Name,    (IPTR)gslChipName},
            {TAG_DONE,                0}
        };

        data->bus = bus;
        data->addr = (UBYTE)addr;
        data->firmware_bytes = bytes;
        data->lines = (struct Hook *)GetTagData(aHidd_TouchController_LineHook,
                                                0, msg->attrList);
        data->raw_x = GetTagData(aHidd_TouchController_RawWidth, 4096,
                                 msg->attrList);
        data->raw_y = GetTagData(aHidd_TouchController_RawHeight, 4096,
                                 msg->attrList);
        data->dev = OOP_NewObject(NULL, CLID_Hidd_I2CDevice, tags);
        if (!data->dev)
        {
            OOP_MethodID dispose = OOP_GetMethodID(IID_Root, moRoot_Dispose);

            OOP_DoSuperMethod(cl, o, (OOP_Msg)&dispose);
            return NULL;
        }
    }
    return o;
}

VOID GSL__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct gsl_data *data = OOP_INST_DATA(cl, o);

    if (data->dev)
        OOP_DisposeObject(data->dev);
    OOP_DoSuperMethod(cl, o, msg);
}

VOID GSL__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    struct gsl_data *data = OOP_INST_DATA(cl, o);
    ULONG idx;

    if (IS_TOUCHCONTROLLER_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_TouchController_Address:
            *msg->storage = data->addr;
            return;
        case aoHidd_TouchController_RawWidth:
            *msg->storage = data->raw_x;
            return;
        case aoHidd_TouchController_RawHeight:
            *msg->storage = data->raw_y;
            return;
        case aoHidd_TouchController_FirmwareBytes:
            *msg->storage = data->firmware_bytes;
            return;
        case aoHidd_TouchController_ChipName:
            *msg->storage = (IPTR)gslChipName;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

BOOL GSL__Hidd_TouchController__Start(OOP_Class *cl, OOP_Object *o,
                                      struct pHidd_TouchController_Start *msg)
{
    struct gsl_data *data = OOP_INST_DATA(cl, o);
    const UBYTE *image = msg->firmware;
    ULONG records = msg->bytes / 8, bad = 0, status = 0, i;
    struct gsl_timer timer;
    BOOL ok = FALSE;

    data->started = FALSE;
    if (!image || msg->bytes != data->firmware_bytes)
    {
        bug("[GSL3670] firmware of %lu bytes refused, %lu expected\n",
            msg->bytes, data->firmware_bytes);
        return FALSE;
    }
    if (!gsl_check_image(image, records, &bad))
    {
        bug("[GSL3670] firmware record %lu invalid; nothing sent\n", bad);
        return FALSE;
    }
    if (!gsl_timer_open(&timer))
        return FALSE;

    if (!gsl_clear_registers(cl, data, o, &timer)
        || !gsl_reset_registers(cl, data, o, &timer))
    {
        bug("[GSL3670] no answer at %02x during reset\n", data->addr);
        goto out;
    }
    for (i = 0; i < records; ++i)
    {
        ULONG offset = gsl_le32(image + i * 8);
        ULONG value = gsl_le32(image + i * 8 + 4);

        if (!gsl_write(cl, data->dev, (UBYTE)offset, value,
                       offset == GSL_PAGE_OFFSET ? 1 : 4))
        {
            bug("[GSL3670] firmware record %lu of %lu not accepted\n",
                i, records);
            goto out;
        }
    }
    if (!gsl_start(cl, data, &timer)
        || !gsl_reset_registers(cl, data, o, &timer)
        || !gsl_start(cl, data, &timer))
        goto out;
    gsl_delay(&timer, GSL_STATUS_MS);
    if (!gsl_read32(cl, data->dev, GSL_REG_STATUS, &status)
        || status != GSL_STATUS_RUNNING)
    {
        bug("[GSL3670] status %08lx after load, %08lx expected\n", status,
            GSL_STATUS_RUNNING);
        goto out;
    }
    data->started = TRUE;
    ok = TRUE;
    bug("[GSL3670] at %02x: %lu firmware records loaded, running\n",
        data->addr, records);

out:
    gsl_timer_close(&timer);
    return ok;
}

VOID GSL__Hidd_TouchController__Stop(OOP_Class *cl, OOP_Object *o,
                                     struct pHidd_TouchController_Stop *msg)
{
    struct gsl_data *data = OOP_INST_DATA(cl, o);

    data->started = FALSE;
}

/* One report, decoded; the 44 bytes come in one transaction, so they
   belong to one frame. */
BOOL GSL__Hidd_TouchController__ReadFrame(OOP_Class *cl, OOP_Object *o,
                                  struct pHidd_TouchController_ReadFrame *msg)
{
    struct gsl_data *data = OOP_INST_DATA(cl, o);
    struct HIDD_TouchFrame *frame = msg->frame;
    UBYTE reg = GSL_REG_REPORT, report[GSL_REPORT_BYTES];
    ULONG reported, bounded, i;

    if (!frame || !data->started)
        return FALSE;
    frame->reported_count = 0;
    frame->count = 0;
    for (i = 0; i < HIDD_TOUCH_MAX_CONTACTS; ++i)
    {
        frame->contact[i].id = 0;
        frame->contact[i].x = 0;
        frame->contact[i].y = 0;
    }
    if (!gsl_xfer(cl, data->dev, &reg, 1, report, sizeof(report)))
        return FALSE;

    reported = report[0];
    bounded = reported > HIDD_TOUCH_MAX_CONTACTS
            ? HIDD_TOUCH_MAX_CONTACTS : reported;
    frame->reported_count = reported;
    for (i = 0; i < bounded; ++i)
    {
        const UBYTE *c = &report[GSL_CONTACT_OFFSET + i * GSL_CONTACT_BYTES];
        struct HIDD_TouchContact *contact;

        if (c[1] >> 4)
            continue;
        contact = &frame->contact[frame->count++];
        contact->y = (ULONG)c[0] | (((ULONG)c[1] & 0x0F) << 8);
        contact->x = (ULONG)c[2] | (((ULONG)c[3] & 0x0F) << 8);
        contact->id = (ULONG)c[3] >> 4;
    }
    return TRUE;
}
