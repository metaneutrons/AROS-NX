/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: AROS Zune/MUI GUI Dual-Bank OTA Firmware Upgrade Application (System:OTAUpdater).
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include "../../../arch/riscv32-esp32p4/include/spiflash_ota.h"

static const char version_tag[] = "$VER: OTAUpdater 1.0 (19.08.2026)\r\n";

struct OTAUpdaterApp
{
    APTR    app;
    APTR    win;
    APTR    gauge_progress;
    APTR    text_status;
    APTR    str_filepath;
    APTR    btn_flash;
    APTR    btn_cancel;
};

int main(int argc, char **argv)
{
    struct Library *MUIMasterBase = OpenLibrary("muimaster.library", 19);
    if (!MUIMasterBase) {
        return RETURN_FAIL;
    }

    int running_slot = ota_get_running_slot();
    int target_slot  = ota_get_target_slot();

    CONST_STRPTR slot_info = (running_slot == OTA_SLOT_A)
        ? "Active: Slot A (0x00020000)  |  Target: Slot B (0x00820000)"
        : "Active: Slot B (0x00820000)  |  Target: Slot A (0x00020000)";

    APTR app = ApplicationObject,
        MUIA_Application_Title,       "AROS OTA Firmware Updater",
        MUIA_Application_Version,     "$VER: OTAUpdater 1.0 (19.08.2026)",
        MUIA_Application_Copyright,   "The AROS Development Team",
        MUIA_Application_Author,      "AROS",
        MUIA_Application_Description, "Dual-Bank OTA Firmware Upgrade Utility",
        MUIA_Application_Base,        "OTAUPDATER",

        SubWindow, WindowObject,
            MUIA_Window_Title, "AROS ESP32-P4 Firmware Upgrade",
            MUIA_Window_ID,    MAKE_ID('O','T','A','U'),
            WindowContents, VGroup,
                Child, TextObject,
                    MUIA_Text_Contents, "\33c\033bAROS Dual-Bank OTA Manager\033n",
                    End,
                Child, TextObject,
                    MUIA_Text_Contents, slot_info,
                    End,
                Child, RectangleObject, MUIA_Rectangle_HBar, TRUE, End,
                Child, HGroup,
                    Child, Label("Firmware Image:"),
                    Child, StringObject,
                        MUIA_String_Contents, "DH0:aros_update.bin",
                        End,
                    End,
                Child, GaugeObject,
                    MUIA_Gauge_Current, 0,
                    MUIA_Gauge_Max,     100,
                    MUIA_Gauge_InfoText, "Ready to Flash",
                    End,
                Child, HGroup,
                    Child, SimpleButton("Flash & Reboot"),
                    Child, SimpleButton("Cancel"),
                    End,
                End,
            End,
        End;

    if (app) {
        SetAttrs(app, MUIA_Application_Active, TRUE, TAG_DONE);
        DoMethod(app, MUIM_Application_Run);
        MUI_DisposeObject(app);
    }

    CloseLibrary(MUIMasterBase);
    return RETURN_OK;
}
