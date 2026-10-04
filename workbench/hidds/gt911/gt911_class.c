#define DEBUG 0
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: Goodix GT911 touch controller.

    The GT911 runs from its own ROM, so there is no firmware to load:
    reset it, read its identity and configured resolution, then poll its
    report buffer. Register addresses are 16 bit, sent high byte first;
    multi-byte values in the registers are little endian.

      0x8140  product ID, four ASCII bytes ("911\0")
      0x8144  firmware version, 16 bit
      0x8146  X resolution, 0x8148 Y resolution (the active configuration)
      0x814E  status: bit 7 a fresh report is ready, bits 0-3 contacts (<= 5)
      0x814F  first contact, eight bytes each: track ID, X, Y, size, reserved

    After a report is read the status register has to be written back to
    zero, or the controller never fills the buffer again.

    The address is chosen by the level of INT while RST rises: low gives
    0x5D, high 0x14. With a line hook this driver drives INT to select the
    preferred address and then releases it; it tries the other address
    only if the preferred one does not answer.

    Contacts arrive in panel pixels on the boards seen so far, whatever the
    configured resolution says (1085 x 600 on a 1024 x 600 panel; scaling
    by it left the pointer behind the finger). So they are reported as they
    come; the touchscreen driver's calibration maps them.
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
#include "gt911_intern.h"

#define GT911_REG_PRODUCT_ID    0x8140
#define GT911_REG_STATUS        0x814E
#define GT911_STATUS_READY      0x80
#define GT911_STATUS_COUNT      0x0F
#define GT911_MAX_CONTACTS      5
#define GT911_CONTACT_BYTES     8
#define GT911_REPORT_BYTES      (1 + GT911_MAX_CONTACTS * GT911_CONTACT_BYTES)

static CONST_STRPTR gt911ChipName = "GT911";

static BOOL gt911_xfer(OOP_Class *cl, OOP_Object *dev,
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

static BOOL gt911_read(OOP_Class *cl, OOP_Object *dev, UWORD reg,
                       UBYTE *out, ULONG bytes)
{
    UBYTE r[2];

    r[0] = (UBYTE)(reg >> 8);
    r[1] = (UBYTE)reg;
    return gt911_xfer(cl, dev, r, 2, out, bytes);
}

static BOOL gt911_write8(OOP_Class *cl, OOP_Object *dev, UWORD reg,
                         UBYTE value)
{
    UBYTE w[3];

    w[0] = (UBYTE)(reg >> 8);
    w[1] = (UBYTE)reg;
    w[2] = value;
    return gt911_xfer(cl, dev, w, 3, NULL, 0);
}

static OOP_Object *gt911_device(OOP_Class *cl, struct gt911_data *data,
                                UBYTE addr)
{
    struct TagItem tags[] =
    {
        {aHidd_I2CDevice_Driver,  (IPTR)data->bus},
        {aHidd_I2CDevice_Address, (IPTR)addr << 1},
        {aHidd_I2CDevice_Name,    (IPTR)gt911ChipName},
        {TAG_DONE,                0}
    };

    return OOP_NewObject(NULL, CLID_Hidd_I2CDevice, tags);
}

static BOOL gt911_line(struct gt911_data *data, OOP_Object *o, ULONG line,
                       LONG level)
{
    struct HIDD_TouchLineMsg msg;

    msg.line = line;
    msg.level = level;
    return (BOOL)CALLHOOKPKT(data->lines, o, &msg);
}

/* At least ms milliseconds; the caller's task sleeps. */
static void gt911_delay(struct timerequest *tr, ULONG ms)
{
    tr->tr_node.io_Command = TR_ADDREQUEST;
    tr->tr_time.tv_secs = ms / 1000;
    tr->tr_time.tv_micro = (ms % 1000) * 1000;
    DoIO(&tr->tr_node);
}

/*
 * The reset that selects the preferred address: RST and INT low, RST high
 * with INT held at the selecting level, INT released, then a pause before
 * the first transfer. The programming guide's minimums are 100 us, 5 ms
 * and 50 ms; these are rounded up generously.
 */
static BOOL gt911_reset(struct gt911_data *data, OOP_Object *o)
{
    struct MsgPort *port = CreateMsgPort();
    struct timerequest *tr = port
        ? (struct timerequest *)CreateIORequest(port, sizeof(*tr)) : NULL;
    LONG select = data->addr_pref == GT911_ADDR_ALT ? 1 : 0;
    BOOL ok = FALSE;

    if (tr && !OpenDevice(TIMERNAME, UNIT_MICROHZ, &tr->tr_node, 0))
    {
        ok = gt911_line(data, o, vHidd_TouchLine_Int, select)
          && gt911_line(data, o, vHidd_TouchLine_Reset, 0);
        gt911_delay(tr, 10);
        ok = ok && gt911_line(data, o, vHidd_TouchLine_Reset, 1);
        gt911_delay(tr, 10);
        ok = ok && gt911_line(data, o, vHidd_TouchLine_Int,
                              vHidd_TouchLine_Release);
        gt911_delay(tr, 60);
        CloseDevice(&tr->tr_node);
    }
    if (tr)
        DeleteIORequest(&tr->tr_node);
    if (port)
        DeleteMsgPort(port);
    return ok;
}

OOP_Object *GT911__Root__New(OOP_Class *cl, OOP_Object *o,
                             struct pRoot_New *msg)
{
    OOP_Object *bus = (OOP_Object *)GetTagData(aHidd_TouchController_Bus, 0,
                                               msg->attrList);
    IPTR pref = GetTagData(aHidd_TouchController_Address, GT911_ADDR_DEFAULT,
                           msg->attrList);
    IPTR alt = GetTagData(aHidd_TouchController_AltAddress,
                          pref == GT911_ADDR_ALT ? GT911_ADDR_DEFAULT
                                                 : GT911_ADDR_ALT,
                          msg->attrList);

    if (!bus || !pref || pref > 0x7F || alt > 0x7F)
    {
        bug("[GT911] refused: bus %p, address %02lx/%02lx\n", bus,
            (ULONG)pref, (ULONG)alt);
        return NULL;
    }
    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
    if (o)
    {
        struct gt911_data *data = OOP_INST_DATA(cl, o);

        data->bus = bus;
        data->addr_pref = (UBYTE)pref;
        data->addr_alt = (UBYTE)alt;
        data->lines = (struct Hook *)GetTagData(aHidd_TouchController_LineHook,
                                                0, msg->attrList);
    }
    return o;
}

VOID GT911__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct gt911_data *data = OOP_INST_DATA(cl, o);

    if (data->dev)
        OOP_DisposeObject(data->dev);
    OOP_DoSuperMethod(cl, o, msg);
}

VOID GT911__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    struct gt911_data *data = OOP_INST_DATA(cl, o);
    ULONG idx;

    if (IS_TOUCHCONTROLLER_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_TouchController_Address:
            *msg->storage = data->started ? data->addr : data->addr_pref;
            return;
        case aoHidd_TouchController_RawWidth:
            *msg->storage = data->res_x;
            return;
        case aoHidd_TouchController_RawHeight:
            *msg->storage = data->res_y;
            return;
        case aoHidd_TouchController_FirmwareBytes:
            *msg->storage = 0;
            return;
        case aoHidd_TouchController_ChipName:
            *msg->storage = (IPTR)gt911ChipName;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/* Reset (if the board gave a hook), find the chip and read its identity. */
BOOL GT911__Hidd_TouchController__Start(OOP_Class *cl, OOP_Object *o,
                                        struct pHidd_TouchController_Start *msg)
{
    struct gt911_data *data = OOP_INST_DATA(cl, o);
    UBYTE addrs[2], id[11];
    ULONG i;

    data->started = FALSE;
    if (data->dev)
    {
        OOP_DisposeObject(data->dev);
        data->dev = NULL;
    }
    if (data->lines && !gt911_reset(data, o))
        bug("[GT911] reset lines not driven; trying the chip as it is\n");

    addrs[0] = data->addr_pref;
    addrs[1] = data->addr_alt;
    for (i = 0; i < 2 && !data->dev; ++i)
    {
        OOP_Object *dev;

        if (!addrs[i] || (i == 1 && addrs[1] == addrs[0]))
            continue;
        dev = gt911_device(cl, data, addrs[i]);
        if (!dev)
            continue;
        if (gt911_read(cl, dev, GT911_REG_PRODUCT_ID, id, sizeof(id)))
        {
            data->dev = dev;
            data->addr = addrs[i];
        }
        else
            OOP_DisposeObject(dev);
    }
    if (!data->dev)
    {
        bug("[GT911] no answer at %02x or %02x\n", data->addr_pref,
            data->addr_alt);
        return FALSE;
    }

    data->firmware = (UWORD)(id[4] | (id[5] << 8));
    data->res_x = (ULONG)id[6] | ((ULONG)id[7] << 8);
    data->res_y = (ULONG)id[8] | ((ULONG)id[9] << 8);
    /* A report left from before the reset must not count as new. */
    gt911_write8(cl, data->dev, GT911_REG_STATUS, 0);
    data->last.reported_count = 0;
    data->last.count = 0;
    data->started = TRUE;
    bug("[GT911] at %02x: product %c%c%c%c, firmware %04x, "
        "configured %lux%lu\n", data->addr,
        id[0] >= 0x20 && id[0] < 0x7F ? id[0] : '?',
        id[1] >= 0x20 && id[1] < 0x7F ? id[1] : '?',
        id[2] >= 0x20 && id[2] < 0x7F ? id[2] : '?',
        id[3] >= 0x20 && id[3] < 0x7F ? id[3] : '.',
        data->firmware, data->res_x, data->res_y);
    return TRUE;
}

/* Nothing to power down; the chip idles between reports. */
VOID GT911__Hidd_TouchController__Stop(OOP_Class *cl, OOP_Object *o,
                                       struct pHidd_TouchController_Stop *msg)
{
    struct gt911_data *data = OOP_INST_DATA(cl, o);

    data->started = FALSE;
}

/*
 * One report. A buffer that is not ready yet means no new frame since the
 * last read, not "all fingers lifted", so the previous frame is repeated;
 * an empty frame there would end every hold after one poll.
 */
BOOL GT911__Hidd_TouchController__ReadFrame(OOP_Class *cl, OOP_Object *o,
                                  struct pHidd_TouchController_ReadFrame *msg)
{
    struct gt911_data *data = OOP_INST_DATA(cl, o);
    struct HIDD_TouchFrame *frame = msg->frame;
    UBYTE report[GT911_REPORT_BYTES];
    ULONG reported, i;

    if (!frame || !data->started)
        return FALSE;
    if (!gt911_read(cl, data->dev, GT911_REG_STATUS, report, sizeof(report)))
        return FALSE;

    if (!(report[0] & GT911_STATUS_READY))
    {
        *frame = data->last;
        return TRUE;
    }

    reported = report[0] & GT911_STATUS_COUNT;
    frame->reported_count = reported;
    frame->count = 0;
    if (reported > GT911_MAX_CONTACTS)
        reported = GT911_MAX_CONTACTS;
    for (i = 0; i < reported; ++i)
    {
        const UBYTE *c = &report[1 + i * GT911_CONTACT_BYTES];
        struct HIDD_TouchContact *contact = &frame->contact[frame->count++];

        contact->id = c[0];
        contact->x = (ULONG)c[1] | ((ULONG)c[2] << 8);
        contact->y = (ULONG)c[3] | ((ULONG)c[4] << 8);
    }
    for (i = frame->count; i < HIDD_TOUCH_MAX_CONTACTS; ++i)
    {
        frame->contact[i].id = 0;
        frame->contact[i].x = 0;
        frame->contact[i].y = 0;
    }
    data->last = *frame;

    return gt911_write8(cl, data->dev, GT911_REG_STATUS, 0);
}
