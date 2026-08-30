/* ESP32-P4 D1001 absolute touch mouse HIDD registration. */

#include <aros/debug.h>
#include <aros/kernel.h>
#include <aros/symbolsets.h>
#include <proto/exec.h>
#include <proto/kernel.h>
#include <proto/oop.h>

#include "p4touch_intern.h"
#include LC_LIBDEFS_FILE

#define SysBase ((struct ExecBase *)LIBBASE->ptd.cs_SysBase)
#define OOPBase LIBBASE->ptd.cs_OOPBase
#undef HiddInputBase
#define HiddInputBase LIBBASE->ptd.hiddInputBase

static int P4Touch_Init(LIBBASETYPEPTR LIBBASE)
{
    struct P4TouchStaticData *ptd = &LIBBASE->ptd;
    struct KernelBase *KernelBase = OpenResource("kernel.resource");
    struct OOP_ABDescr attrbases[] =
    {
        {IID_Hidd_Input, &ptd->hiddInputAB},
        {IID_Hidd_Mouse, &ptd->hiddMouseAB},
        {NULL, NULL}
    };
    OOP_Object *subsystem;
    OOP_Object *driver = NULL;

    InitSemaphore(&ptd->lock);
    ptd->cs_UtilityBase = OpenLibrary("utility.library", 0);
    if (!ptd->cs_UtilityBase)
    {
        bug("[P4Touch/C4] cannot open utility.library\n");
        return FALSE;
    }
    ptd->ops = KernelBase
        ? (struct KrnTouchScreenOps *)KrnGetSystemAttr(KATTR_TouchScreenOps)
        : NULL;
    if (!ptd->ops || ptd->ops == (APTR)-1
        || ptd->ops->version != KRN_TOUCHSCREEN_OPS_VERSION
        || !ptd->ops->load_firmware
        || !ptd->ops->acquire
        || !ptd->ops->release
        || !ptd->ops->read_contacts
        || ptd->ops->logical_width != 1280
        || ptd->ops->logical_height != 800
        || ptd->ops->raw_width < ptd->ops->logical_width
        || ptd->ops->raw_height < ptd->ops->logical_height)
    {
        bug("[P4Touch/C4] touch operation table missing or invalid\n");
        CloseLibrary(ptd->cs_UtilityBase);
        return FALSE;
    }

    if (!OOP_ObtainAttrBases(attrbases))
    {
        bug("[P4Touch/C4] cannot obtain input attribute bases\n");
        CloseLibrary(ptd->cs_UtilityBase);
        return FALSE;
    }
    ptd->hiddInputBase = OOP_GetMethodID(IID_Hidd_Input, 0);

    subsystem = OOP_NewObject(NULL, CLID_Hidd_Mouse, NULL);
    if (subsystem)
    {
        driver = HIDD_Input_AddHardwareDriver(subsystem, ptd->mouseclass, NULL);
        OOP_DisposeObject(subsystem);
    }
    if (!driver)
    {
        OOP_ReleaseAttrBases(attrbases);
        CloseLibrary(ptd->cs_UtilityBase);
        bug("[P4Touch/C4] hardware mouse registration failed\n");
        return FALSE;
    }

    LIBBASE->library.lib_OpenCnt = 1;
    bug("[P4Touch/C4] absolute 1280x800 contact-frame driver registered; "
        "up to %u contacts, raw %lux%lu, direct X and mirrored Y\n",
        KRN_TOUCHSCREEN_MAX_CONTACTS,
        (unsigned long)ptd->ops->raw_width,
        (unsigned long)ptd->ops->raw_height);
    return TRUE;
}

ADD2INITLIB(P4Touch_Init, 0)
