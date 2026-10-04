#define DEBUG 0
/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The bus class - a hidd.i2c driven by an ESP32-P4 I2C controller.

    hidd.i2c is built around two wires its driver wiggles (PutBits and
    GetBits at the bottom, WriteRead at the top).  This controller runs a
    whole transaction from a command list, so the class overrides the top
    of that stack: WriteRead and ProbeAddress carry out the transfer, and
    the byte-level methods refuse, as in dwi2c, because the controller
    cannot hold the bus between calls.
*/

#include <aros/debug.h>

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>

#include <aros/symbolsets.h>

#include <exec/types.h>
#include <hidd/hidd.h>
#include <hidd/i2c.h>
#include <utility/tagitem.h>

#include LC_LIBDEFS_FILE
#include "p4i2c_intern.h"

static CONST_STRPTR p4i2cHWName = "ESP32-P4 I2C controller";

OOP_Object *P4I2C__Root__New(OOP_Class *cl, OOP_Object *o,
                             struct pRoot_New *msg)
{
    struct p4i2c_staticdata *psd = PSD(cl);
    struct pRoot_New superMsg;
    struct TagItem superTags[2];
    struct p4i2c_ctrl *ctrl;
    OOP_Object *busObj;
    IPTR port, sda, scl, hz;
    BOOL taken = FALSE;

    port = GetTagData(aHidd_I2C_ESP32P4_Port, (IPTR)-1, msg->attrList);
    sda = GetTagData(aHidd_I2C_ESP32P4_SDA, (IPTR)-1, msg->attrList);
    scl = GetTagData(aHidd_I2C_ESP32P4_SCL, (IPTR)-1, msg->attrList);
    hz = GetTagData(aHidd_I2C_ESP32P4_Speed, 100000, msg->attrList);

    if (port >= P4I2C_PORTS || sda > P4I2C_MAX_GPIO || scl > P4I2C_MAX_GPIO
        || sda == scl || hz < 1000 || hz > 400000)
    {
        bug("[P4I2C] refused: port %ld, SDA %ld, SCL %ld, %ld Hz\n",
            (LONG)port, (LONG)sda, (LONG)scl, (LONG)hz);
        return NULL;
    }
    ctrl = &psd->ctrl[port];

    /* The matrix routes one controller to one pair of pads; a second pair
       would leave the first still wired to the controller's output. */
    ObtainSemaphore(&psd->objLock);
    if (ctrl->users && (ctrl->sda != sda || ctrl->scl != scl))
        taken = TRUE;
    else
    {
        ctrl->sda = (UBYTE)sda;
        ctrl->scl = (UBYTE)scl;
        ++ctrl->users;
    }
    ReleaseSemaphore(&psd->objLock);
    if (taken)
    {
        bug("[P4I2C] %s is on pads %u/%u; refused %ld/%ld\n", ctrl->name,
            ctrl->sda, ctrl->scl, (LONG)sda, (LONG)scl);
        return NULL;
    }

    /* The base class keeps the name pointer, so hand it the driver's. */
    superTags[0].ti_Tag  = aHidd_I2C_Name;
    superTags[0].ti_Data = (IPTR)ctrl->name;
    superTags[1].ti_Tag  = TAG_MORE;
    superTags[1].ti_Data = (IPTR)msg->attrList;
    superMsg.mID      = msg->mID;
    superMsg.attrList = superTags;

    busObj = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&superMsg);
    if (busObj)
    {
        struct p4i2c_busdata *data = OOP_INST_DATA(cl, busObj);

        data->ctrl = ctrl;
        data->port = port;
        data->hz = hz;
        data->last = P4_I2C_NOTREADY;
        D(bug("[P4I2C] %s object %p, pads %ld/%ld, %ld Hz\n", ctrl->name,
              busObj, (LONG)sda, (LONG)scl, (LONG)hz);)
    }
    else
    {
        ObtainSemaphore(&psd->objLock);
        --ctrl->users;
        ReleaseSemaphore(&psd->objLock);
    }
    return busObj;
}

VOID P4I2C__Root__Dispose(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    struct p4i2c_staticdata *psd = PSD(cl);
    struct p4i2c_busdata *data = OOP_INST_DATA(cl, o);

    ObtainSemaphore(&psd->objLock);
    if (data->ctrl && data->ctrl->users)
        --data->ctrl->users;
    ReleaseSemaphore(&psd->objLock);
    OOP_DoSuperMethod(cl, o, msg);
}

VOID P4I2C__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    struct p4i2c_busdata *data = OOP_INST_DATA(cl, o);
    ULONG idx;

    if (IS_HIDD_ATTR(msg->attrID, idx) && idx == aoHidd_HardwareName)
    {
        *msg->storage = (IPTR)p4i2cHWName;
        return;
    }
    if (IS_I2CESP32P4_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_I2C_ESP32P4_Port:
            *msg->storage = data->port;
            return;
        case aoHidd_I2C_ESP32P4_SDA:
            *msg->storage = data->ctrl->sda;
            return;
        case aoHidd_I2C_ESP32P4_SCL:
            *msg->storage = data->ctrl->scl;
            return;
        case aoHidd_I2C_ESP32P4_Speed:
            *msg->storage = data->hz;
            return;
        case aoHidd_I2C_ESP32P4_LastResult:
            *msg->storage = (IPTR)data->last;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/* With ctrl->lock held: program the controller for this object if the
   last transfer left it for another rate, or never programmed it. */
static BOOL p4i2c_select(struct p4i2c_busdata *data)
{
    struct p4i2c_ctrl *ctrl = data->ctrl;

    if (ctrl->hw.ready && ctrl->hz == data->hz)
        return TRUE;
    if (!p4i2c_init(&ctrl->hw, data->port, ctrl->sda, ctrl->scl, data->hz))
    {
        bug("[P4I2C] %s: cannot program %lu Hz\n", ctrl->name,
            (ULONG)data->hz);
        return FALSE;
    }
    ctrl->hz = data->hz;
    D(bug("[P4I2C] %s programmed, pads %u/%u, %lu Hz\n", ctrl->name,
          ctrl->sda, ctrl->scl, (ULONG)data->hz);)
    return TRUE;
}

/*
 * The write-then-read transaction; with either half empty, the plain
 * write or the plain read.  hidd.i2c carries the address in wire form;
 * the transport wants the 7-bit address and adds the R/W bit itself.
 */
BOOL P4I2C__Hidd_I2C__WriteRead(OOP_Class *cl, OOP_Object *o,
                                struct pHidd_I2C_WriteRead *msg)
{
    struct p4i2c_busdata *data = OOP_INST_DATA(cl, o);
    IPTR address = 0;
    int result;

    if (msg->device)
        OOP_GetAttr(msg->device, aHidd_I2CDevice_Address, &address);
    if (msg->writeLength > P4_I2C_MAX_WRITE
        || msg->readLength > P4_I2C_MAX_READ)
    {
        data->last = P4_I2C_TOOLONG;
        return FALSE;
    }

    ObtainSemaphore(&data->ctrl->lock);
    result = p4i2c_select(data)
           ? p4i2c_transfer(&data->ctrl->hw,
                            (unsigned int)((address >> 1) & 0x7f),
                            msg->writeBuffer, msg->writeLength,
                            msg->readBuffer, msg->readLength)
           : P4_I2C_NOTREADY;
    ReleaseSemaphore(&data->ctrl->lock);

    data->last = result;
    D(if (result != P4_I2C_OK)
          bug("[P4I2C] %s: %02lx w%lu r%lu: %s\n", data->ctrl->name,
              (ULONG)((address >> 1) & 0x7f), (ULONG)msg->writeLength,
              (ULONG)msg->readLength, p4i2c_result_name(result));)
    return result == P4_I2C_OK;
}

BOOL P4I2C__Hidd_I2C__ProbeAddress(OOP_Class *cl, OOP_Object *o,
                                   struct pHidd_I2C_ProbeAddress *msg)
{
    struct p4i2c_busdata *data = OOP_INST_DATA(cl, o);
    int result;

    ObtainSemaphore(&data->ctrl->lock);
    result = p4i2c_select(data)
           ? p4i2c_probe(&data->ctrl->hw,
                         (unsigned int)((msg->address >> 1) & 0x7f))
           : P4_I2C_NOTREADY;
    ReleaseSemaphore(&data->ctrl->lock);

    data->last = result;
    return result == P4_I2C_OK;
}

/*
 * What a controller cannot provide.  These refuse so that a caller gets
 * FALSE and not the base class's unimplemented PutBits/GetBits.
 */

BOOL P4I2C__Hidd_I2C__Start(OOP_Class *cl, OOP_Object *o,
                            struct pHidd_I2C_Start *msg)
{
    D(bug("[P4I2C] Start: not possible on a controller, use WriteRead\n");)
    return FALSE;
}

VOID P4I2C__Hidd_I2C__Stop(OOP_Class *cl, OOP_Object *o,
                           struct pHidd_I2C_Stop *msg)
{
    D(bug("[P4I2C] Stop: not possible on a controller, use WriteRead\n");)
}

BOOL P4I2C__Hidd_I2C__Address(OOP_Class *cl, OOP_Object *o,
                              struct pHidd_I2C_Address *msg)
{
    D(bug("[P4I2C] Address: not possible on a controller, use WriteRead\n");)
    return FALSE;
}

BOOL P4I2C__Hidd_I2C__PutByte(OOP_Class *cl, OOP_Object *o,
                              struct pHidd_I2C_PutByte *msg)
{
    D(bug("[P4I2C] PutByte: not possible on a controller, use WriteRead\n");)
    return FALSE;
}

BOOL P4I2C__Hidd_I2C__GetByte(OOP_Class *cl, OOP_Object *o,
                              struct pHidd_I2C_GetByte *msg)
{
    D(bug("[P4I2C] GetByte: not possible on a controller, use WriteRead\n");)
    return FALSE;
}
