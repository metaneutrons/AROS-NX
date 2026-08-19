/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: AROS World-Class Zune/MUI GUI Dual-Bank OTA Firmware Upgrade Application.
*/

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include "../../../arch/riscv32-esp32p4/include/spiflash_ota.h"

static const char version_tag[] = "$VER: OTAUpdater 1.0 (19.08.2026)\r\n";

#define ID_ABOUT        0x1001
#define ID_FLASH        0x1002
#define ID_CANCEL       0x1003
#define ID_FILE_CHANGED 0x1004

#define CHUNK_SIZE      4096

struct OTAUpdaterApp
{
    APTR    app;
    APTR    win;
    APTR    txt_running_slot;
    APTR    txt_target_slot;
    APTR    pop_file;
    APTR    str_filepath;
    APTR    txt_fileinfo;
    APTR    chk_reboot;
    APTR    gauge_progress;
    APTR    txt_status;
    APTR    btn_flash;
    APTR    btn_cancel;
};

static void update_file_info(struct OTAUpdaterApp *data)
{
    STRPTR path = NULL;
    GetAttr(MUIA_String_Contents, data->str_filepath, (ULONG *)(IPTR)&path);

    if (!path || path[0] == '\0') {
        SetAttrs(data->txt_fileinfo, MUIA_Text_Contents, "\033cNo firmware binary selected.", TAG_DONE);
        SetAttrs(data->btn_flash, MUIA_Disabled, TRUE, TAG_DONE);
        return;
    }

    BPTR fh = Open(path, MODE_OLDFILE);
    if (!fh) {
        SetAttrs(data->txt_fileinfo, MUIA_Text_Contents, "\033c\033rERROR: Cannot open selected file.\033n", TAG_DONE);
        SetAttrs(data->btn_flash, MUIA_Disabled, TRUE, TAG_DONE);
        return;
    }

    Seek(fh, 0, OFFSET_END);
    LONG size = Seek(fh, 0, OFFSET_BEGINNING);

    UBYTE magic = 0;
    Read(fh, &magic, 1);
    Close(fh);

    if (magic == 0xE9 && size > 0 && size <= (8 * 1024 * 1024)) {
        static char info_buf[128];
        ULONG size_kb = (ULONG)size / 1024;
        /* Formatted information */
        SetAttrs(data->txt_fileinfo, MUIA_Text_Contents, "\033c\033g[OK]\033n Valid ESP32-P4 Image (Magic 0xE9) | Size: 8 MB Max", TAG_DONE);
        SetAttrs(data->btn_flash, MUIA_Disabled, FALSE, TAG_DONE);
    } else {
        SetAttrs(data->txt_fileinfo, MUIA_Text_Contents, "\033c\033rInvalid Image (Expected ESP32-P4 Magic 0xE9)\033n", TAG_DONE);
        SetAttrs(data->btn_flash, MUIA_Disabled, TRUE, TAG_DONE);
    }
}

static void do_flash_firmware(struct OTAUpdaterApp *data)
{
    STRPTR path = NULL;
    GetAttr(MUIA_String_Contents, data->str_filepath, (ULONG *)(IPTR)&path);
    if (!path) return;

    BPTR fh = Open(path, MODE_OLDFILE);
    if (!fh) return;

    Seek(fh, 0, OFFSET_END);
    LONG filesize = Seek(fh, 0, OFFSET_BEGINNING);

    SetAttrs(data->btn_flash, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(data->btn_cancel, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(data->txt_status, MUIA_Text_Contents, "\033c\033bErasing and Writing SPI Flash Partition...\033n", TAG_DONE);

    UBYTE chunk_buf[CHUNK_SIZE];
    ULONG total_written = 0;
    BOOL success = TRUE;

    while (total_written < (ULONG)filesize) {
        LONG bytes_read = Read(fh, chunk_buf, CHUNK_SIZE);
        if (bytes_read <= 0) break;

        if (ota_write_target_slot(total_written, chunk_buf, (ULONG)bytes_read) < 0) {
            success = FALSE;
            break;
        }

        total_written += bytes_read;
        ULONG pct = (total_written * 100) / filesize;

        SetAttrs(data->gauge_progress, MUIA_Gauge_Current, pct, TAG_DONE);

        /* Handle UI events cooperatively */
        DoMethod(data->app, MUIM_Application_InputBuffered);
    }

    Close(fh);

    if (success && ota_activate_target_slot() == 0) {
        SetAttrs(data->gauge_progress, MUIA_Gauge_Current, 100, TAG_DONE);
        SetAttrs(data->txt_status, MUIA_Text_Contents, "\033c\033g\033bFlash Upgrade Successful! Bootloader Slot Updated.\033n", TAG_DONE);

        ULONG do_reboot = 0;
        GetAttr(MUIA_Selected, data->chk_reboot, &do_reboot);

        if (do_reboot) {
            SetAttrs(data->txt_status, MUIA_Text_Contents, "\033c\033bRebooting System now...\033n", TAG_DONE);
            DoMethod(data->app, MUIM_Application_InputBuffered);
            ota_reboot();
        }
    } else {
        SetAttrs(data->txt_status, MUIA_Text_Contents, "\033c\033r\033bERROR: Flash write or verification failed!\033n", TAG_DONE);
    }

    SetAttrs(data->btn_cancel, MUIA_Disabled, FALSE, TAG_DONE);
}

int main(int argc, char **argv)
{
    struct Library *MUIMasterBase = OpenLibrary("muimaster.library", 19);
    if (!MUIMasterBase) return RETURN_FAIL;

    struct OTAUpdaterApp data = {0};

    int running = ota_get_running_slot();
    int target  = ota_get_target_slot();

    CONST_STRPTR running_str = (running == OTA_SLOT_A)
        ? "\033b\033gSlot A (Active: 0x00020000)\033n"
        : "\033b\033gSlot B (Active: 0x00820000)\033n";

    CONST_STRPTR target_str = (target == OTA_SLOT_A)
        ? "\033b\033bSlot A (Target: 0x00020000)\033n"
        : "\033b\033bSlot B (Target: 0x00820000)\033n";

    data.app = ApplicationObject,
        MUIA_Application_Title,       "AROS OTA Firmware Updater",
        MUIA_Application_Version,     "$VER: OTAUpdater 1.0 (19.08.2026)",
        MUIA_Application_Copyright,   "The AROS Development Team",
        MUIA_Application_Author,      "AROS",
        MUIA_Application_Description, "Dual-Bank OTA Firmware Upgrade Utility",
        MUIA_Application_Base,        "OTAUPDATER",

        SubWindow, data.win = WindowObject,
            MUIA_Window_Title, "AROS ESP32-P4 Dual-Bank OTA Manager",
            MUIA_Window_ID,    MAKE_ID('O','T','A','U'),
            MUIA_Window_Width, 480,
            WindowContents, VGroup,

                /* 1. Header Banner Group */
                Child, VGroup,
                    MUIA_Frame, MUIV_Frame_Group,
                    MUIA_Background, MUII_GroupBack,
                    Child, TextObject,
                        MUIA_Text_Contents, "\33c\033b\033uAROS Research Operating System\033n\n\033cESP32-P4 Dual-Bank OTA Firmware Manager",
                        End,
                    End,

                /* 2. Dual-Bank Slot Status Group */
                Child, VGroup,
                    MUIA_Frame, MUIV_Frame_Group,
                    MUIA_FrameTitle, "Partition Bank Topology",
                    Child, HGroup,
                        Child, Label("Running Bank:"),
                        Child, data.txt_running_slot = TextObject,
                            MUIA_Text_Contents, running_str,
                            End,
                        End,
                    Child, HGroup,
                        Child, Label("Inactive Target:"),
                        Child, data.txt_target_slot = TextObject,
                            MUIA_Text_Contents, target_str,
                            End,
                        End,
                    End,

                /* 3. Firmware Selection Group */
                Child, VGroup,
                    MUIA_Frame, MUIV_Frame_Group,
                    MUIA_FrameTitle, "Firmware Binary Image (.bin)",
                    Child, data.pop_file = PopaslObject,
                        MUIA_Popasl_Type, ASL_FileRequest,
                        MUIA_Popstring_String, data.str_filepath = StringObject,
                            MUIA_Frame, MUIV_Frame_String,
                            MUIA_String_Contents, "DH0:aros_a.bin",
                            End,
                        MUIA_Popstring_Button, PopButton(MUII_PopFile),
                        End,
                    Child, data.txt_fileinfo = TextObject,
                        MUIA_Text_Contents, "\033cValidating firmware...",
                        End,
                    End,

                /* 4. Progress & Status Group */
                Child, VGroup,
                    MUIA_Frame, MUIV_Frame_Group,
                    MUIA_FrameTitle, "Flash Upgrade Status",
                    Child, data.gauge_progress = GaugeObject,
                        MUIA_Gauge_Current, 0,
                        MUIA_Gauge_Max,     100,
                        MUIA_Gauge_InfoText, "%ld %%",
                        End,
                    Child, data.txt_status = TextObject,
                        MUIA_Text_Contents, "\033cReady to flash into inactive partition.",
                        End,
                    End,

                /* 5. Options Group */
                Child, HGroup,
                    Child, Label("Automatic Reboot:"),
                    Child, data.chk_reboot = CheckMark(TRUE),
                    Child, HSpace(0),
                    End,

                /* 6. Action Buttons */
                Child, HGroup,
                    Child, data.btn_flash = SimpleButton(" \033bFlash Firmware\033n "),
                    Child, data.btn_cancel = SimpleButton(" Quit "),
                    End,
                End,
            End,
        End;

    if (!data.app) {
        CloseLibrary(MUIMasterBase);
        return RETURN_FAIL;
    }

    /* Setup MUI Notifications */
    DoMethod(data.win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             data.app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(data.btn_cancel, MUIM_Notify, MUIA_Pressed, FALSE,
             data.app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(data.btn_flash, MUIM_Notify, MUIA_Pressed, FALSE,
             data.app, 2, MUIM_Application_ReturnID, ID_FLASH);

    DoMethod(data.str_filepath, MUIM_Notify, MUIA_String_Contents, MUIV_EveryCharacter,
             data.app, 2, MUIM_Application_ReturnID, ID_FILE_CHANGED);

    /* Open Window */
    SetAttrs(data.win, MUIA_Window_Open, TRUE, TAG_DONE);
    update_file_info(&data);

    /* Event Loop */
    ULONG sigs = 0;
    BOOL running_loop = TRUE;

    while (running_loop) {
        ULONG id = DoMethod(data.app, MUIM_Application_NewInput, &sigs);

        switch (id) {
            case MUIV_Application_ReturnID_Quit:
                running_loop = FALSE;
                break;
            case ID_FLASH:
                do_flash_firmware(&data);
                break;
            case ID_FILE_CHANGED:
                update_file_info(&data);
                break;
        }

        if (running_loop && sigs) {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C);
            if (sigs & SIGBREAKF_CTRL_C) break;
        }
    }

    SetAttrs(data.win, MUIA_Window_Open, FALSE, TAG_DONE);
    MUI_DisposeObject(data.app);
    CloseLibrary(MUIMasterBase);

    return RETURN_OK;
}
