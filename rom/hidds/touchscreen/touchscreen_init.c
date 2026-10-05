/* touchscreen.hidd: library initialisation. */

#include <aros/debug.h>
#include <aros/symbolsets.h>
#include <exec/lists.h>
#include <proto/exec.h>
#include <proto/oop.h>

#include "touchscreen_intern.h"
#include LC_LIBDEFS_FILE

#define SysBase ((struct ExecBase *)LIBBASE->tsd.cs_SysBase)
#define OOPBase LIBBASE->tsd.cs_OOPBase

/*
 * Only the class is set up here. Driver objects are made by whoever knows
 * the machine (board code, a USB driver), with HW_AddDriver() on the
 * pointing device subsystem; see <hidd/touchscreen.h>.
 */
static int Touch_Init(LIBBASETYPEPTR LIBBASE)
{
    struct TouchStaticData *tsd = &LIBBASE->tsd;
    struct OOP_ABDescr attrbases[] =
    {
        {IID_Hidd_Input,           &tsd->hiddInputAB},
        {IID_Hidd_Mouse,           &tsd->hiddMouseAB},
        {IID_Hidd_TouchScreen,     &tsd->hiddTouchScreenAB},
        {IID_Hidd_TouchController, &tsd->hiddTouchControllerAB},
        {NULL, NULL}
    };

    InitSemaphore(&LIBBASE->pub.tsb_Lock);
    NEWLIST((struct List *)&LIBBASE->pub.tsb_Drivers);

    tsd->cs_UtilityBase = OpenLibrary("utility.library", 0);
    if (!tsd->cs_UtilityBase)
        return FALSE;
    if (!OOP_ObtainAttrBases(attrbases))
    {
        bug("[Touch] cannot obtain attribute bases\n");
        CloseLibrary(tsd->cs_UtilityBase);
        return FALSE;
    }
    /* The controller method IDs exist only once a controller class has
       registered IID_Hidd_TouchController, which may be after this; New
       resolves them. */
    return TRUE;
}

ADD2INITLIB(Touch_Init, 0)
