/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#define MUIMASTER_YES_INLINE_STDARG

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <limits.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

#include <stdio.h>
#include <string.h>

#include "boardtool.h"
#include "layout_gui.h"

#define VERSION_TAG "$VER: ESP32Board 0.2 (11.10.2026) AROS Development Team"
#define ARG_TEMPLATE "ACTION/K,IMAGE/K,SHA256/K,TABLE/K,FLASHMB/K/N,GUI/S"
#define GUI_CLOSE_ID 100
#define STATUS_WRAP_COLUMN 68

struct MsgPort *BoardToolWorkerPort;
struct Task *BoardToolMainTask;

static struct MsgPort *reply_port;
static struct BoardToolMessage *work_message;
static struct Process *worker_process;
static BOOL busy;
static BOOL close_pending;
static ULONG reply_signal;

static Object *application;
static Object *window;
static Object *image_string;
static Object *table_string;
static Object *sha_string;
static Object *flash_string;
static Object *info_button;
static Object *check_button;
static Object *layout_button;
static Object *discover_button;
static Object *compare_button;
static Object *update_button;
static Object *status_text;
static Object *register_group;
static Object *layout_group;
static Object *partition_list;
static Object *details_text;
static Object *table_sha_string;
static Object *package_string;
static Object *target_cycle;
static Object *source_cycle;
static Object *package_check_button;
static Object *release_refresh_button;
static Object *backup_button;
static Object *write_button;
static Object *raw_compare_button;
static Object *expert_check;
static Object *goto_update_button;
static Object *goto_flash_button;
static Object *goto_partitions_button;
static Object *goto_diagnostics_button;

static const char info_text[] =
    "C6 file format and hash preflight only.\n"
    "Compatibility and independent recovery are NOT verified.\n"
    "A registry observation is not raw-table attestation or write permission.\n"
    "Compare checks image metadata only; it grants no admission.\n"
    "Queries stop WLAN and leave it offline. Updates remain unavailable.";

static char status_buffer[BOARDTOOL_STATUS_MAX * 2];

static void set_status(CONST_STRPTR text)
{
    size_t source = 0;
    size_t target = 0;
    size_t line_length = 0;

    if (status_text == NULL)
        return;
    if (text == NULL)
        text = "";

    /* Status can come from a worker buffer. Wrap it before passing it to a
     * TextObject so an error or safety message cannot set the window width. */
    while (text[source] != '\0' && target + 1 < sizeof(status_buffer))
    {
        size_t word_start;
        size_t word_length;

        if (text[source] == '\n')
        {
            status_buffer[target++] = '\n';
            ++source;
            line_length = 0;
            continue;
        }
        while (text[source] == ' ' || text[source] == '\t' ||
               text[source] == '\r')
            ++source;
        if (text[source] == '\0')
            break;
        if (text[source] == '\n')
            continue;

        word_start = source;
        while (text[source] != '\0' && text[source] != '\n' &&
               text[source] != ' ' && text[source] != '\t' &&
               text[source] != '\r')
            ++source;
        word_length = source - word_start;
        if (word_length == 0)
            continue;
        if (line_length != 0 && line_length + 1 + word_length >
                                STATUS_WRAP_COLUMN)
        {
            status_buffer[target++] = '\n';
            line_length = 0;
        }
        else if (line_length != 0 && target + 1 < sizeof(status_buffer))
        {
            status_buffer[target++] = ' ';
            ++line_length;
        }
        while (word_length != 0 && target + 1 < sizeof(status_buffer))
        {
            if (line_length == STATUS_WRAP_COLUMN)
            {
                if (target + 1 >= sizeof(status_buffer))
                    break;
                status_buffer[target++] = '\n';
                line_length = 0;
            }
            if (target + 1 >= sizeof(status_buffer))
                break;
            status_buffer[target++] = text[word_start++];
            --word_length;
            ++line_length;
        }
    }
    status_buffer[target] = '\0';
    SET(status_text, MUIA_Text_Contents, (IPTR)status_buffer);
}

static BOOL copy_exact(char *out, size_t capacity, CONST_STRPTR in)
{
    size_t length;

    if (capacity == 0 || in == NULL)
        return FALSE;
    length = strlen((const char *)in);
    if (length == 0 || length >= capacity)
        return FALSE;
    memcpy(out, in, length + 1);
    return TRUE;
}

static BOOL flash_capacity(ULONG flash_mb)
{
    switch (flash_mb)
    {
        case 1: case 2: case 4: case 8: case 16: case 32: case 64: case 128:
            return TRUE;
        default:
            return FALSE;
    }
}

static BOOL parse_flash_capacity(CONST_STRPTR text, ULONG *flash_mb)
{
    ULONG value = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (p == NULL || *p == '\0')
        return FALSE;
    while (*p != '\0')
    {
        ULONG digit;
        if (*p < '0' || *p > '9')
            return FALSE;
        digit = (ULONG)(*p++ - '0');
        if (value > (ULONG_MAX - digit) / 10)
            return FALSE;
        value = value * 10 + digit;
    }
    if (!flash_capacity(value))
        return FALSE;
    *flash_mb = value;
    return TRUE;
}

static void print_layout(const struct board_layout *layout)
{
    ULONG i;

    printf("Imported preview, not live/active, no write. Flash capacity: %lu MiB\n",
           (unsigned long)(layout->flash_bytes / (1024UL * 1024UL)));
    printf("Offset      Size        Type/Subtype Flags      Label\n");
    for (i = 0; i < layout->count; i++)
    {
        const struct board_partition *entry = &layout->partitions[i];
        printf("%08lx  %08lx  %02x/%02x       %08lx  %s\n",
               (unsigned long)entry->offset, (unsigned long)entry->size,
               entry->type, entry->subtype, (unsigned long)entry->flags,
               entry->label);
    }
}

static void print_discovery(const struct board_client_observation *observation)
{
    const struct board_discovery_snapshot *s = &observation->snapshot;
    size_t i;
    printf("Historical C6 registry observation, not raw-table attestation; no write.\n"
        "Running/boot/update indices: %u/%u/%u (255=unknown); state=%u\n"
        "Security known mask=%02x enabled mask=%02x (unknown is not disabled).\n",
        s->running, s->boot, s->update, s->running_state,
        s->security_known, s->security_enabled);
    printf("Target known mask=%02x chip=%u revision(MXX)=%u physical secure count=%lu\n",
        s->target_known, s->chip_id, s->chip_revision,
        (unsigned long)s->secure_version);
    printf("Index Offset      Size        Type/Subtype Flags      Label\n");
    for (i = 0; i < s->layout.count; ++i) {
        const struct board_partition *p = &s->layout.partitions[i];
        printf("%2lu    %08lx  %08lx  %02x/%02x       %08lx  %s\n",
            (unsigned long)i, (unsigned long)p->offset, (unsigned long)p->size,
            p->type, p->subtype, (unsigned long)p->flags, p->label);
    }
}

LONG boardtool_dispatch(ULONG operation, struct BoardToolMessage *message)
{
    const struct BoardToolOperation *descriptor =
        boardtool_find_operation_id(operation);
    char *status;
    size_t capacity;

    if (message == NULL)
        return 0;
    status = message->status;
    capacity = sizeof(message->status);
    status[0] = '\0';

    if (descriptor == NULL)
    {
        snprintf(status, capacity, "Unknown operation; no write");
        return 0;
    }
    if (!descriptor->available)
    {
        snprintf(status, capacity,
            "C6 update unavailable: OTA backend and independent recovery "
            "are unverified; discovery does not authorize writes");
        return 0;
    }

    switch (operation)
    {
        case BOARDTOOL_OP_INFO:
            snprintf(status, capacity, "%s", info_text);
            return 1;

        case BOARDTOOL_OP_CHECK_C6:
            return boardtool_check_c6(message->image, message->sha256,
                                      status, capacity);

        case BOARDTOOL_OP_DISCOVER_C6:
            return boardtool_discover_c6(&message->observation, status, capacity);

        case BOARDTOOL_OP_COMPARE_C6:
            return boardtool_compare_c6(message->image, message->sha256,
                &message->observation, &message->comparison, status, capacity);

        case BOARDTOOL_OP_LAYOUT:
            if (!flash_capacity(message->flash_mb))
            {
                snprintf(status, capacity,
                    "Choose an explicit flash capacity: 1, 2, 4, 8, 16, "
                    "32, 64 or 128 MiB; no live query or write");
                return 0;
            }
            return boardtool_load_layout(message->image, message->sha256,
                message->flash_mb * 1024UL * 1024UL, &message->layout,
                status, capacity);

        default:
            snprintf(status, capacity, "Operation not implemented; no write");
            return 0;
    }
}

static BOOL start_worker(void)
{
    if (reply_port != NULL && work_message != NULL)
        return TRUE;

    reply_port = CreateMsgPort();
    work_message = AllocVec(sizeof(*work_message), MEMF_PUBLIC | MEMF_CLEAR);
    if (reply_port == NULL || work_message == NULL)
        return FALSE;

    reply_signal = 1UL << reply_port->mp_SigBit;
    BoardToolMainTask = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    worker_process = CreateNewProcTags(NP_Entry, (IPTR)BoardToolWorker,
        NP_Name, (IPTR)"ESP32Board worker", NP_StackSize, 65536, TAG_END);
    if (worker_process == NULL)
        return FALSE;

    Wait(SIGF_SINGLE);
    return BoardToolWorkerPort != NULL;
}

static BOOL send_operation(ULONG operation, CONST_STRPTR path,
                           CONST_STRPTR sha256, ULONG flash_mb)
{
    const struct BoardToolOperation *descriptor =
        boardtool_find_operation_id(operation);

    if (busy || BoardToolWorkerPort == NULL || descriptor == NULL ||
        !descriptor->available)
        return FALSE;

    work_message->image[0] = '\0';
    work_message->sha256[0] = '\0';
    work_message->flash_mb = flash_mb;
    memset(&work_message->layout, 0, sizeof(work_message->layout));
    memset(&work_message->observation, 0, sizeof(work_message->observation));
    memset(&work_message->comparison, 0, sizeof(work_message->comparison));
    if ((operation == BOARDTOOL_OP_CHECK_C6 ||
         operation == BOARDTOOL_OP_COMPARE_C6 ||
         operation == BOARDTOOL_OP_LAYOUT) &&
        (!copy_exact(work_message->image, sizeof(work_message->image), path) ||
         !copy_exact(work_message->sha256, sizeof(work_message->sha256), sha256)))
        return FALSE;

    work_message->command = BOARDTOOL_CMD_RUN;
    work_message->operation = operation;
    work_message->message.mn_Node.ln_Type = NT_MESSAGE;
    work_message->message.mn_Length = sizeof(*work_message);
    work_message->message.mn_ReplyPort = reply_port;
    busy = TRUE;
    SET(check_button, MUIA_Disabled, TRUE);
    SET(info_button, MUIA_Disabled, TRUE);
    SET(layout_button, MUIA_Disabled, TRUE);
    SET(discover_button, MUIA_Disabled, TRUE);
    SET(compare_button, MUIA_Disabled, TRUE);
    SET(image_string, MUIA_Disabled, TRUE);
    SET(table_string, MUIA_Disabled, TRUE);
    SET(sha_string, MUIA_Disabled, TRUE);
    SET(table_sha_string, MUIA_Disabled, TRUE);
    SET(flash_string, MUIA_Disabled, TRUE);
    PutMsg(BoardToolWorkerPort, &work_message->message);
    return TRUE;
}

static void handle_reply(void)
{
    struct BoardToolMessage *reply;

    while ((reply = (struct BoardToolMessage *)GetMsg(reply_port)) != NULL)
    {
        if (reply != work_message)
            continue;
        busy = FALSE;
        SET(check_button, MUIA_Disabled, FALSE);
        SET(info_button, MUIA_Disabled, FALSE);
        SET(layout_button, MUIA_Disabled, FALSE);
        SET(discover_button, MUIA_Disabled, FALSE);
        SET(compare_button, MUIA_Disabled, FALSE);
        SET(image_string, MUIA_Disabled, FALSE);
        SET(table_string, MUIA_Disabled, FALSE);
        SET(sha_string, MUIA_Disabled, FALSE);
        SET(table_sha_string, MUIA_Disabled, FALSE);
        SET(flash_string, MUIA_Disabled, FALSE);
        set_status(reply->status);
        if (reply->result &&
            reply->operation == BOARDTOOL_OP_LAYOUT && layout_group != NULL)
        {
            if (!boardtool_layout_render(application, layout_group,
                    partition_list, details_text, &reply->layout, NULL))
                set_status("Imported table validated, but its graphical "
                           "preview could not be built; no write.");
        }
        if (reply->result && (reply->operation == BOARDTOOL_OP_DISCOVER_C6 ||
            reply->operation == BOARDTOOL_OP_COMPARE_C6) &&
            layout_group != NULL && !boardtool_layout_render(application,
                layout_group, partition_list, details_text,
                &reply->observation.snapshot.layout,
                &reply->observation.snapshot))
            set_status("C6 observation graph could not be built. Comparison, "
                "policy and recovery remain incomplete; no write.");
    }
}

static void stop_worker(void)
{
    if (BoardToolWorkerPort == NULL || work_message == NULL)
        return;

    SetSignal(0, SIGF_SINGLE);
    work_message->command = BOARDTOOL_CMD_QUIT;
    work_message->message.mn_Node.ln_Type = NT_MESSAGE;
    work_message->message.mn_Length = sizeof(*work_message);
    work_message->message.mn_ReplyPort = reply_port;
    PutMsg(BoardToolWorkerPort, &work_message->message);
    WaitPort(reply_port);
    GetMsg(reply_port);                 /* worker has replied to the quit */
    Wait(SIGF_SINGLE);                  /* its port is now deleted */
}

static void request_close(BOOL *done)
{
    if (busy)
    {
        close_pending = TRUE;
        if ((work_message->operation == BOARDTOOL_OP_DISCOVER_C6 ||
             work_message->operation == BOARDTOOL_OP_COMPARE_C6) && worker_process)
            Signal(&worker_process->pr_Task, SIGBREAKF_CTRL_C);
        set_status("Close requested. Discovery is cancelled with bounded lease "
                   "release; file checks finish before closing. No write is running.");
        SET(window, MUIA_Window_Open, TRUE);
    }
    else
        *done = TRUE;
}

static LONG run_gui(void)
{
    const struct BoardToolOperation *info =
        boardtool_find_operation_name("INFO");
    const struct BoardToolOperation *check =
        boardtool_find_operation_name("CHECK-C6");
    const struct BoardToolOperation *layout =
        boardtool_find_operation_name("LAYOUT");
    const struct BoardToolOperation *update =
        boardtool_find_operation_name("UPDATE-C6");
    const struct BoardToolOperation *discover =
        boardtool_find_operation_name("DISCOVER-C6");
    const struct BoardToolOperation *compare =
        boardtool_find_operation_name("COMPARE-C6");
    ULONG signals = 0;
    BOOL done = FALSE;
    LONG result = RETURN_FAIL;
    static STRPTR page_titles[] =
        { "Overview", "Update", "Flash & Backup", "Diagnostics", NULL };
    static STRPTR update_targets[] =
        { "AROS Kickstart + BSP", "C6 radio firmware", NULL };
    static STRPTR update_sources[] = { "Local file", "GitHub Releases", NULL };

    if (info == NULL || check == NULL || layout == NULL || update == NULL || discover == NULL || compare == NULL ||
        !start_worker())
    {
        PutStr("ESP32Board: cannot start worker or operation registry\n");
        goto cleanup;
    }

    boardtool_layout_gui_init();
    application = (Object *)ApplicationObject,
        MUIA_Application_Title, (IPTR)"ESP32Board",
        MUIA_Application_Version, (IPTR)VERSION_TAG,
        MUIA_Application_Description, (IPTR)"ESP32 board maintenance tools",
        MUIA_Application_Base, (IPTR)"ESP32BOARD",
        SubWindow, (IPTR)(window = (Object *)WindowObject,
            MUIA_Window_Title, (IPTR)"ESP32Board",
            MUIA_Window_ID, MAKE_ID('E','3','2','B'),
            /* Keep the maintenance window compact even on a wider panel.
             * MUI dimensions are inner sizes; leave room for screen/window
             * chrome on an800x600 desktop. Native min/max can still clamp. */
            MUIA_Window_Width, 740,
            MUIA_Window_Height, 500,
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(register_group = (Object *)RegisterObject,
                    MUIA_Register_Titles, (IPTR)page_titles,
                    /* Overview: show no board facts until a read-only query. */
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)"Board maintenance overview",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Hardware facts have not been queried. No write is enabled.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Board profile"),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"Not queried",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                            Child, (IPTR)Label((STRPTR)"P4 / C6 identity"),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"Not queried",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"AROS release"),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"Not queried",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                            Child, (IPTR)Label((STRPTR)"Active slot"),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"Not queried",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(goto_update_button =
                                (Object *)SimpleButton("Update firmware")),
                            Child, (IPTR)(goto_flash_button =
                                (Object *)SimpleButton("Flash & Backup")),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(goto_diagnostics_button =
                                (Object *)SimpleButton("Inspect diagnostics")),
                            Child, (IPTR)(goto_partitions_button =
                                (Object *)SimpleButton("View partitions")),
                        End),
                    End),
                    /* Update controls describe unsupported operations only. */
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Update, release download and signature checks are unavailable.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Target"),
                            Child, (IPTR)(target_cycle = (Object *)CycleObject,
                                MUIA_Cycle_Entries, (IPTR)update_targets,
                                MUIA_Cycle_Active, 0,
                                MUIA_CycleChain, 1,
                            End),
                            Child, (IPTR)Label((STRPTR)"Source"),
                            Child, (IPTR)(source_cycle = (Object *)CycleObject,
                                MUIA_Cycle_Entries, (IPTR)update_sources,
                                MUIA_Cycle_Active, 0,
                                MUIA_CycleChain, 1,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Local package"),
                            Child, (IPTR)(PopaslObject,
                                MUIA_Popasl_Type, ASL_FileRequest,
                                ASLFR_RejectIcons, TRUE,
                                MUIA_Popstring_String, (IPTR)(package_string =
                                    (Object *)StringObject,
                                    StringFrame,
                                    MUIA_String_MaxLen, BOARDTOOL_PATH_MAX - 1,
                                    MUIA_CycleChain, 1,
                                End),
                                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopFile),
                            End),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "GitHub Releases: no listing or download available.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "No P4 package, signature, compatibility or install backend.\n"
                                "Target/source selection does not start an operation.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(release_refresh_button = (Object *)
                                SimpleButton("Refresh releases (unavailable)")),
                            Child, (IPTR)(package_check_button = (Object *)
                                SimpleButton("Check package (unavailable)")),
                            Child, (IPTR)(update_button = (Object *)
                                SimpleButton("Install (unavailable)")),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)"Existing C6 file diagnostics",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"C6 image"),
                            Child, (IPTR)(PopaslObject,
                                MUIA_Popasl_Type, ASL_FileRequest,
                                ASLFR_RejectIcons, TRUE,
                                MUIA_Popstring_String, (IPTR)(image_string =
                                    (Object *)StringObject,
                                    StringFrame,
                                    MUIA_String_MaxLen, BOARDTOOL_PATH_MAX - 1,
                                    MUIA_CycleChain, 1,
                                End),
                                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopFile),
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Trusted SHA256"),
                            Child, (IPTR)(sha_string = (Object *)StringObject,
                                StringFrame,
                                MUIA_String_MaxLen, BOARDTOOL_SHA256_LEN,
                                MUIA_CycleChain, 1,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(check_button =
                                (Object *)SimpleButton("Check C6 file")),
                            Child, (IPTR)(compare_button = (Object *)
                                SimpleButton("Compare C6 (WLAN offline)")),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Check is format/hash preflight; compare grants no admission.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                    End),
                    /* Imported or queried layout: proportional map and rows. */
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Imported table"),
                            Child, (IPTR)(PopaslObject,
                                MUIA_Popasl_Type, ASL_FileRequest,
                                ASLFR_RejectIcons, TRUE,
                                MUIA_Popstring_String, (IPTR)(table_string =
                                    (Object *)StringObject,
                                    StringFrame,
                                    MUIA_String_MaxLen, BOARDTOOL_PATH_MAX - 1,
                                    MUIA_CycleChain, 1,
                                End),
                                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopFile),
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Trusted table SHA256"),
                            Child, (IPTR)(table_sha_string =
                                (Object *)StringObject,
                                StringFrame,
                                MUIA_String_MaxLen, BOARDTOOL_SHA256_LEN,
                                MUIA_CycleChain, 1,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)Label((STRPTR)"Flash MiB (explicit)"),
                            Child, (IPTR)(flash_string = (Object *)StringObject,
                                StringFrame,
                                MUIA_String_MaxLen, 3,
                                MUIA_String_Contents, (IPTR)"",
                                MUIA_CycleChain, 1,
                            End),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"1/2/4/8/16/32/64/128",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(layout_button = (Object *)
                                SimpleButton("Preview imported table")),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)
                                    "No live query or write.",
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Text_SetMax, FALSE,
                            End),
                        End),
                        Child, (IPTR)(HGroup,
                            MUIA_FixHeight, 14,
                            MUIA_Group_Spacing, 1,
                            Child, (IPTR)(layout_group = (Object *)HGroup,
                                MUIA_Group_Spacing, 1,
                            End),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Filled: listed partitions. Empty: unlisted/reserved, NOT FREE.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(ListviewObject,
                            MUIA_Listview_List, (IPTR)(partition_list = ListObject,
                                InputListFrame,
                                MUIA_List_Format, (IPTR)
                                    "BAR,BAR,BAR,BAR,BAR,COL=5",
                                MUIA_List_ConstructHook,
                                    MUIV_List_ConstructHook_String,
                                MUIA_List_DestructHook,
                                    MUIV_List_DestructHook_String,
                                MUIA_List_DisplayHook,
                                    (IPTR)&boardtool_partition_display_hook,
                                MUIA_List_Title, TRUE,
                            End),
                            MUIA_Listview_Input, FALSE,
                            MUIA_CycleChain, 1,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(backup_button = (Object *)
                                SimpleButton("Save backup (unavailable)")),
                            Child, (IPTR)(write_button = (Object *)
                                SimpleButton("Write file (unavailable)")),
                            Child, (IPTR)(raw_compare_button = (Object *)
                                SimpleButton("Compare raw (unavailable)")),
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(expert_check = (Object *)
                                MUI_MakeObject(MUIO_Checkmark, NULL)),
                            Child, (IPTR)Label((STRPTR)
                                "Expert mode (raw ranges unavailable)"),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Imported roles are unknown. Gaps are not verified free.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                    End),
                    /* Read-only diagnostics, including scrollable detail text. */
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Identity and capabilities are unknown until queried.\n"
                                "Query stops WLAN and leaves it offline; no write occurs.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(HGroup,
                            Child, (IPTR)(info_button =
                                (Object *)SimpleButton("Safety information")),
                            Child, (IPTR)(discover_button = (Object *)
                                SimpleButton("Refresh information (WLAN offline)")),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)
                                "Reset and destructive tests are not routine diagnostics.",
                            MUIA_Text_SetMin, FALSE,
                            MUIA_Text_SetMax, FALSE,
                        End),
                        Child, (IPTR)(ScrollgroupObject,
                            MUIA_Scrollgroup_FreeHoriz, FALSE,
                            MUIA_Scrollgroup_FreeVert, TRUE,
                            MUIA_Scrollgroup_Contents, (IPTR)(VirtgroupObject,
                                Child, (IPTR)(details_text = (Object *)TextObject,
                                    MUIA_Text_Contents, (IPTR)
                                        "No query or layout loaded.\n"
                                        "No live identity or capability facts asserted.",
                                    MUIA_Text_SetMin, FALSE,
                                    MUIA_Text_SetMax, FALSE,
                                End),
                            End),
                        End),
                    End),
                End),
                Child, (IPTR)(status_text = (Object *)TextObject,
                    MUIA_Text_Contents, (IPTR)
                        "Ready. Hardware not queried; writes are unavailable.",
                    MUIA_Text_SetMin, FALSE,
                    MUIA_Text_SetMax, FALSE,
                End),
            End),
        End),
    End;

    if (application == NULL)
    {
        PutStr("ESP32Board: cannot create MUI application\n");
        goto cleanup;
    }

    DoMethod(window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
        (IPTR)application, 2, MUIM_Application_ReturnID, GUI_CLOSE_ID);
    DoMethod(info_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)application, 2, MUIM_Application_ReturnID, info->id);
    DoMethod(check_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)application, 2, MUIM_Application_ReturnID, check->id);
    DoMethod(layout_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)application, 2, MUIM_Application_ReturnID, layout->id);
    DoMethod(discover_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)application, 2, MUIM_Application_ReturnID, discover->id);
    DoMethod(compare_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)application, 2, MUIM_Application_ReturnID, compare->id);
    DoMethod(goto_update_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)register_group, 3, MUIM_Set, MUIA_Group_ActivePage, 1);
    DoMethod(goto_flash_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)register_group, 3, MUIM_Set, MUIA_Group_ActivePage, 2);
    DoMethod(goto_partitions_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)register_group, 3, MUIM_Set, MUIA_Group_ActivePage, 2);
    DoMethod(goto_diagnostics_button, MUIM_Notify, MUIA_Pressed, FALSE,
        (IPTR)register_group, 3, MUIM_Set, MUIA_Group_ActivePage, 3);
    SET(update_button, MUIA_Disabled, !update->available);
    SET(release_refresh_button, MUIA_Disabled, TRUE);
    SET(package_check_button, MUIA_Disabled, TRUE);
    SET(backup_button, MUIA_Disabled, TRUE);
    SET(write_button, MUIA_Disabled, TRUE);
    SET(raw_compare_button, MUIA_Disabled, TRUE);
    SET(expert_check, MUIA_Disabled, TRUE);
    SET(window, MUIA_Window_Open, TRUE);
    if (!XGET(window, MUIA_Window_Open))
    {
        PutStr("ESP32Board: cannot open window\n");
        goto cleanup;
    }

    result = RETURN_OK;
    while (!done)
    {
        LONG id = (LONG)DoMethod(application, MUIM_Application_NewInput,
                                  (IPTR)&signals);

        if (id == (LONG)info->id)
        {
            if (!busy)
            {
                boardtool_dispatch(info->id, work_message);
                set_status(work_message->status);
            }
        }
        else if (id == (LONG)discover->id && !busy && !close_pending)
        {
            if (send_operation((ULONG)id, NULL, NULL, 0))
                set_status("Querying C6 registry; WLAN stops and remains offline. "
                    "Prior preview is historical; no flash write or reset.");
        }
        else if ((id == (LONG)check->id || id == (LONG)layout->id ||
                  id == (LONG)compare->id) &&
                 !busy && !close_pending)
        {
            CONST_STRPTR path;
            CONST_STRPTR digest;
            ULONG flash_mb = 0;
            BOOL valid;

            if (id == (LONG)check->id || id == (LONG)compare->id)
            {
                path = (CONST_STRPTR)XGET(image_string, MUIA_String_Contents);
                digest = (CONST_STRPTR)XGET(sha_string, MUIA_String_Contents);
                valid = copy_exact(work_message->image,
                                   sizeof(work_message->image), path) &&
                        copy_exact(work_message->sha256,
                                   sizeof(work_message->sha256), digest);
            }
            else
            {
                path = (CONST_STRPTR)XGET(table_string, MUIA_String_Contents);
                digest = (CONST_STRPTR)XGET(table_sha_string,
                                             MUIA_String_Contents);
                valid = copy_exact(work_message->image,
                                   sizeof(work_message->image), path) &&
                        copy_exact(work_message->sha256,
                                   sizeof(work_message->sha256), digest) &&
                        parse_flash_capacity((CONST_STRPTR)XGET(flash_string,
                            MUIA_String_Contents), &flash_mb);
            }

            if (!valid)
            {
                set_status("Enter a bounded path, trusted 64-hex SHA256, and "
                           "an allowed explicit flash capacity for preview.");
                result = RETURN_FAIL;
            }
            else if (send_operation((ULONG)id, path, digest, flash_mb))
            {
                if (id == (LONG)check->id)
                    set_status("Checking C6 image format and trusted hash; no write.");
                else if (id == (LONG)compare->id)
                    set_status("Comparing inspected C6 image with registry query; "
                        "WLAN goes offline; no admission/write.");
                else
                    set_status("Reading imported table for a non-live preview; no write.");
            }
            else
                set_status("Request unavailable or already in progress; no write.");
        }
        else if (id == GUI_CLOSE_ID ||
                 id == (LONG)MUIV_Application_ReturnID_Quit)
            request_close(&done);

        /* Drain replies independently of input-event traffic. GetMsg is safe
         * when the reply queue is empty and does not relinquish message data. */
        handle_reply();

        /* A close event or the final deferred reply must not require another
         * input signal merely to reach cleanup. */
        if (done || (close_pending && !busy))
            break;

        if (signals != 0)
        {
            signals = Wait(signals | reply_signal | SIGBREAKF_CTRL_C);
            if (signals & reply_signal)
                handle_reply();
            if (signals & SIGBREAKF_CTRL_C)
                request_close(&done);
        }

        if (close_pending && !busy)
            done = TRUE;
    }

cleanup:
    if (application != NULL)
    {
        MUI_DisposeObject(application);
        application = NULL;
        window = NULL;
    }
    stop_worker();
    if (work_message != NULL)
    {
        FreeVec(work_message);
        work_message = NULL;
    }
    if (reply_port != NULL)
    {
        DeleteMsgPort(reply_port);
        reply_port = NULL;
    }
    BoardToolWorkerPort = NULL;
    BoardToolMainTask = NULL;
    return result;
}

static LONG run_cli(CONST_STRPTR action, CONST_STRPTR image,
                    CONST_STRPTR sha256, CONST_STRPTR table,
                    ULONG flash_mb)
{
    const struct BoardToolOperation *operation =
        boardtool_find_operation_name(action);
    struct BoardToolMessage message;
    LONG result;

    if (operation == NULL)
    {
        fprintf(stderr, "ESP32Board: unknown ACTION; use INFO, CHECK-C6, "
                        "LAYOUT, DISCOVER-C6, COMPARE-C6 or UPDATE-C6\n");
        return RETURN_FAIL;
    }

    memset(&message, 0, sizeof(message));
    if (operation->id == BOARDTOOL_OP_CHECK_C6 ||
        operation->id == BOARDTOOL_OP_COMPARE_C6)
    {
        if (!copy_exact(message.image, sizeof(message.image), image) ||
            !copy_exact(message.sha256, sizeof(message.sha256), sha256) ||
            table != NULL || flash_mb != 0)
        {
            fprintf(stderr, "ESP32Board: CHECK-C6/COMPARE-C6 requires IMAGE and "
                            "trusted SHA256; no TABLE/FLASHMB\n");
            return RETURN_FAIL;
        }
    }
    else if (operation->id == BOARDTOOL_OP_LAYOUT)
    {
        if (!copy_exact(message.image, sizeof(message.image), table) ||
            !copy_exact(message.sha256, sizeof(message.sha256), sha256) ||
            !flash_capacity(flash_mb) || image != NULL)
        {
            fprintf(stderr, "ESP32Board: LAYOUT requires TABLE, trusted "
                "SHA256 and FLASHMB=1|2|4|8|16|32|64|128; no live query/write\n");
            return RETURN_FAIL;
        }
        message.flash_mb = flash_mb;
    }
    else if (image != NULL || sha256 != NULL || table != NULL || flash_mb != 0)
    {
        fprintf(stderr, "ESP32Board: arguments are not used by this action\n");
        return RETURN_FAIL;
    }

    if (operation->id == BOARDTOOL_OP_DISCOVER_C6 ||
        operation->id == BOARDTOOL_OP_COMPARE_C6)
        puts("C6 query stops WLAN and leaves it offline after release; "
             "no flash write, credentials or automatic reset.");
    result = boardtool_dispatch(operation->id, &message);
    puts(message.status);
    if (result && operation->id == BOARDTOOL_OP_LAYOUT)
        print_layout(&message.layout);
    if (result && (operation->id == BOARDTOOL_OP_DISCOVER_C6 ||
        operation->id == BOARDTOOL_OP_COMPARE_C6))
        print_discovery(&message.observation);
    if (result && operation->id == BOARDTOOL_OP_COMPARE_C6)
        return message.comparison.rejected ? RETURN_FAIL : RETURN_WARN;
    return result ? RETURN_OK : RETURN_FAIL;
}

int main(int argc, char **argv)
{
    IPTR args[6] = { 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rdargs;
    CONST_STRPTR action, image, sha256, table;
    ULONG flash_mb = 0;
    BOOL gui;
    LONG result;

    (void)argv;
    if (argc == 0)
        return run_gui();

    rdargs = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rdargs == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"ESP32Board");
        return RETURN_FAIL;
    }

    action = (CONST_STRPTR)args[0];
    image = (CONST_STRPTR)args[1];
    sha256 = (CONST_STRPTR)args[2];
    table = (CONST_STRPTR)args[3];
    if (args[4] != 0)
        flash_mb = *(ULONG *)args[4];
    gui = (BOOL)args[5];

    if (gui || action == NULL)
        result = run_gui();
    else
        result = run_cli(action, image, sha256, table, flash_mb);

    FreeArgs(rdargs);
    return result;
}
