/* Host fixture for the consent-gated USB launchers.
 * Diagnostic: cc -std=c11 -Wall -Wextra -Werror test_launch_diag.c
 * Echo mode: add -DP4_USB_LAUNCH_ECHO=1 to the compile command.
 */
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef intptr_t BPTR;
typedef intptr_t IPTR;
typedef intptr_t LONG;
typedef uintptr_t ULONG;
typedef char *STRPTR;
typedef const char *CONST_STRPTR;
typedef void *APTR;
typedef unsigned char UBYTE;
typedef int BOOL;

struct Library {
    int unused;
};

struct IntuitionBase {
    struct Library LibNode;
};

struct EasyStruct {
    ULONG es_StructSize;
    ULONG es_Flags;
    CONST_STRPTR es_Title;
    CONST_STRPTR es_TextFormat;
    CONST_STRPTR es_GadgetFormat;
};

struct FileInfoBlock {
    LONG fib_Size;
};

#define BNULL ((BPTR)0)
#define FALSE 0
#define TRUE 1
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define OFFSET_BEGINNING -1
#define OFFSET_CURRENT 0
#define OFFSET_END 1
#define DOS_FIB 2
#define RETURN_OK ((LONG)0)
#define RETURN_FAIL ((LONG)-1)
#define TAG_END ((IPTR)0)
#define SYS_Input ((IPTR)1)
#define SYS_Output ((IPTR)2)
#define SYS_Error ((IPTR)3)
#define SYS_Asynch ((IPTR)4)
#define SYS_Background ((IPTR)5)

BPTR Open(CONST_STRPTR name, LONG mode);
LONG Close(BPTR file);
LONG Read(BPTR file, APTR buffer, LONG length);
LONG Seek(BPTR file, LONG position, LONG offset);
LONG SystemTags(STRPTR command, ...);
LONG IoErr(void);
APTR AllocDosObject(ULONG type, APTR tags);
void FreeDosObject(ULONG type, APTR object);
BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *library);
LONG EasyRequestArgs(void *window, struct EasyStruct *request,
                     void *idcmp, void *args);

#define LAUNCH_DIAG_HOST_TEST 1
#define main launch_diag_main
#include "launch_diag.c"
#undef main

#if defined(P4_USB_LAUNCH_ECHO)
#define EXPECTED_CONSENT_TITLE "START: USBEcho"
#define EXPECTED_FAILURE_TITLE "FAILED: USBEcho"
#define EXPECTED_FINISHED_TITLE "FINISHED: USBEcho"
#define EXPECTED_LOG_PATH "RAM:USBEcho.log"
#define EXPECTED_COMMAND \
    "FLASHDISK0P0:USBEcho ENABLE VID 4617 PID 1 SECONDS 600 STEADYCLOCKS"
#else
#define EXPECTED_CONSENT_TITLE "START: USBDiag"
#define EXPECTED_FAILURE_TITLE "FAILED: USBDiag"
#define EXPECTED_FINISHED_TITLE "FINISHED: USBDiag"
#define EXPECTED_LOG_PATH "RAM:USBDiag.log"
#define EXPECTED_COMMAND \
    "FLASHDISK0P0:USBDiag ENABLE VID 4617 PID 1 SECONDS 600 " \
    "STEADYCLOCKS NONEAUTH SCREEN Workbench ROOT SYS:Prefs " \
    "ALLOWMOUSE UNATTENDED"
#endif

#define MAX_CALLS 8
#define MAX_TEXT 1200
#define MAX_LOG 4096

static struct IntuitionBase fake_intuition;
static struct FileInfoBlock fake_fib;
static int fake_library_open_fails;
static int fake_library_open_count;
static int fake_library_close_count;
static int fake_consent_result;
static int fake_failure_result;
static int fake_request_count;
static int fake_failure_request_count;
static int fake_finished_request_count;
static int fake_open_count;
static int fake_fail_open_at;
static int fake_fail_log_read_open;
static int fake_fail_fib_alloc;
static int fake_fail_examine;
static int fake_fail_seek;
static int fake_fail_read;
static int fake_close_count;
static int fake_system_count;
static LONG fake_system_result;
static LONG fake_dos_error;
static BPTR fake_closed_handles[MAX_CALLS];
static BPTR fake_next_handle;
static BPTR fake_log_read_handle;
static char fake_open_paths[MAX_CALLS][64];
static LONG fake_open_modes[MAX_CALLS];
static char fake_consent_title[128];
static char fake_consent_text[MAX_TEXT];
static char fake_consent_gadgets[128];
static char fake_final_title[128];
static char fake_final_text[MAX_TEXT];
static char fake_request_text[MAX_TEXT];
static char fake_request_gadgets[128];
static IPTR fake_tags[16];
static IPTR fake_values[16];
static int fake_tag_count;
static char fake_command[256];
static char fake_system_log[MAX_LOG];
static size_t fake_system_log_length;
static UBYTE fake_log_data[MAX_LOG];
static size_t fake_log_size;
static size_t fake_log_position;
static size_t fake_log_read_bytes;
static int fake_log_created;
static int fake_fib_alloc_count;
static int fake_fib_free_count;

static void set_system_log(const char *text)
{
    fake_system_log_length = strlen(text);
    assert(fake_system_log_length <= sizeof(fake_system_log));
    memcpy(fake_system_log, text, fake_system_log_length);
}

static void reset_fake(void)
{
    fake_library_open_fails = 0;
    fake_library_open_count = 0;
    fake_library_close_count = 0;
    fake_consent_result = 1;
    fake_failure_result = 1;
    fake_request_count = 0;
    fake_failure_request_count = 0;
    fake_finished_request_count = 0;
    fake_open_count = 0;
    fake_fail_open_at = 0;
    fake_fail_log_read_open = 0;
    fake_fail_fib_alloc = 0;
    fake_fail_examine = 0;
    fake_fail_seek = 0;
    fake_fail_read = 0;
    fake_close_count = 0;
    fake_system_count = 0;
    fake_system_result = RETURN_OK;
    fake_dos_error = 0;
    fake_next_handle = 100;
    fake_log_read_handle = BNULL;
    fake_system_log_length = 0;
    fake_log_size = 0;
    fake_log_position = 0;
    fake_log_read_bytes = 0;
    fake_log_created = 0;
    fake_fib_alloc_count = 0;
    fake_fib_free_count = 0;
    fake_tag_count = 0;
    fake_consent_title[0] = '\0';
    fake_consent_text[0] = '\0';
    fake_consent_gadgets[0] = '\0';
    fake_final_title[0] = '\0';
    fake_final_text[0] = '\0';
    fake_request_text[0] = '\0';
    fake_request_gadgets[0] = '\0';
    fake_command[0] = '\0';
    memset(&fake_fib, 0, sizeof(fake_fib));
    memset(fake_closed_handles, 0, sizeof(fake_closed_handles));
    memset(fake_open_paths, 0, sizeof(fake_open_paths));
    memset(fake_open_modes, 0, sizeof(fake_open_modes));
    memset(fake_tags, 0, sizeof(fake_tags));
    memset(fake_values, 0, sizeof(fake_values));
    memset(fake_system_log, 0, sizeof(fake_system_log));
    memset(fake_log_data, 0, sizeof(fake_log_data));
}

BPTR Open(CONST_STRPTR name, LONG mode)
{
    int call = ++fake_open_count;

    assert(call <= MAX_CALLS);
    snprintf(fake_open_paths[call - 1], sizeof(fake_open_paths[call - 1]),
             "%s", name);
    fake_open_modes[call - 1] = mode;
    if (call == fake_fail_open_at) {
        fake_dos_error = 1000 + call;
        return BNULL;
    }
    if (strcmp(name, EXPECTED_LOG_PATH) == 0 && mode == MODE_NEWFILE) {
        fake_log_created = 1;
        fake_log_size = 0;
        fake_log_position = 0;
    } else if (strcmp(name, EXPECTED_LOG_PATH) == 0 &&
               mode == MODE_OLDFILE) {
        if (fake_fail_log_read_open || !fake_log_created) {
            fake_dos_error = 1200 + call;
            return BNULL;
        }
        fake_log_read_handle = ++fake_next_handle;
        fake_log_position = 0;
        return fake_log_read_handle;
    }
    return ++fake_next_handle;
}

LONG Close(BPTR file)
{
    assert(fake_close_count < MAX_CALLS);
    fake_closed_handles[fake_close_count++] = file;
    return RETURN_OK;
}

LONG Read(BPTR file, APTR buffer, LONG length)
{
    size_t available;
    size_t count;

    assert(file == fake_log_read_handle);
    assert(length >= 0);
    if (fake_fail_read) {
        fake_dos_error = 1301;
        return -1;
    }
    available = fake_log_size - fake_log_position;
    count = (size_t)length < available ? (size_t)length : available;
    memcpy(buffer, fake_log_data + fake_log_position, count);
    fake_log_position += count;
    fake_log_read_bytes += count;
    return (LONG)count;
}

LONG Seek(BPTR file, LONG position, LONG offset)
{
    LONG old_position;
    LONG base;
    LONG next;

    assert(file == fake_log_read_handle);
    if (fake_fail_seek) {
        fake_dos_error = 1302;
        return -1;
    }
    old_position = (LONG)fake_log_position;
    if (offset == OFFSET_BEGINNING)
        base = 0;
    else if (offset == OFFSET_CURRENT)
        base = old_position;
    else if (offset == OFFSET_END)
        base = (LONG)fake_log_size;
    else
        return -1;
    next = base + position;
    if (next < 0 || (size_t)next > fake_log_size) {
        fake_dos_error = 1303;
        return -1;
    }
    fake_log_position = (size_t)next;
    return old_position;
}

LONG IoErr(void)
{
    return fake_dos_error;
}

APTR AllocDosObject(ULONG type, APTR tags)
{
    assert(type == DOS_FIB);
    assert(tags == NULL);
    fake_fib_alloc_count++;
    if (fake_fail_fib_alloc) {
        fake_dos_error = 1300;
        return NULL;
    }
    return &fake_fib;
}

void FreeDosObject(ULONG type, APTR object)
{
    assert(type == DOS_FIB);
    assert(object == &fake_fib);
    fake_fib_free_count++;
}

BOOL ExamineFH(BPTR file, struct FileInfoBlock *fib)
{
    assert(file == fake_log_read_handle);
    if (fake_fail_examine) {
        fake_dos_error = 1304;
        return FALSE;
    }
    fib->fib_Size = (LONG)fake_log_size;
    return TRUE;
}

LONG SystemTags(STRPTR command, ...)
{
    va_list args;
    IPTR tag;

    fake_system_count++;
    snprintf(fake_command, sizeof(fake_command), "%s", command);
    va_start(args, command);
    while ((tag = va_arg(args, IPTR)) != TAG_END) {
        assert(fake_tag_count < 15);
        fake_tags[fake_tag_count] = tag;
        fake_values[fake_tag_count] = va_arg(args, IPTR);
        fake_tag_count++;
    }
    va_end(args);
    assert(fake_log_created);
    assert(fake_system_log_length <= sizeof(fake_log_data));
    memcpy(fake_log_data, fake_system_log, fake_system_log_length);
    fake_log_size = fake_system_log_length;
    return fake_system_result;
}

struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    assert(strcmp(name, "intuition.library") == 0);
    assert(version == 39);
    fake_library_open_count++;
    if (fake_library_open_fails)
        return NULL;
    return &fake_intuition.LibNode;
}

void CloseLibrary(struct Library *library)
{
    assert(library == &fake_intuition.LibNode);
    fake_library_close_count++;
}

LONG EasyRequestArgs(void *window, struct EasyStruct *request,
                     void *idcmp, void *args)
{
    assert(window == NULL);
    assert(idcmp == NULL);
    assert(args == NULL);
    fake_request_count++;
    snprintf(fake_request_gadgets, sizeof(fake_request_gadgets), "%s",
             request->es_GadgetFormat);
    if (strcmp(request->es_Title, EXPECTED_CONSENT_TITLE) == 0) {
        snprintf(fake_consent_title, sizeof(fake_consent_title), "%s",
                 request->es_Title);
        snprintf(fake_consent_text, sizeof(fake_consent_text), "%s",
                 request->es_TextFormat);
        snprintf(fake_consent_gadgets, sizeof(fake_consent_gadgets), "%s",
                 request->es_GadgetFormat);
        return fake_consent_result;
    }
    snprintf(fake_final_title, sizeof(fake_final_title), "%s",
             request->es_Title);
    snprintf(fake_final_text, sizeof(fake_final_text), "%s",
             request->es_TextFormat);
    snprintf(fake_request_text, sizeof(fake_request_text), "%s",
             request->es_TextFormat);
    if (strcmp(request->es_Title, EXPECTED_FAILURE_TITLE) == 0)
        fake_failure_request_count++;
    else {
        assert(strcmp(request->es_Title, EXPECTED_FINISHED_TITLE) == 0);
        fake_finished_request_count++;
    }
    return fake_failure_result;
}

static void assert_closed_exactly(int successful_open_count)
{
    int i;
    int seen[MAX_CALLS] = {0};

    assert(fake_close_count == successful_open_count);
    for (i = 0; i < fake_close_count; i++) {
        BPTR handle = fake_closed_handles[i];
        int slot = (int)handle - 100;

        assert(slot > 0 && slot <= successful_open_count);
        assert(!seen[slot]);
        seen[slot] = 1;
    }
    for (i = 1; i <= successful_open_count; i++)
        assert(seen[i]);
}

static void assert_text_layout(const char *text, size_t max_lines)
{
    const char *line = text;
    const char *cursor = text;
    size_t lines = 1;

    while (*cursor != '\0') {
        if (*cursor == '\n') {
            assert((size_t)(cursor - line) <= 64);
            line = cursor + 1;
            lines++;
        }
        cursor++;
    }
    assert((size_t)(cursor - line) <= 64);
    assert(lines <= max_lines);
}

static void assert_no_control_bytes(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    while (*cursor != '\0') {
        assert(*cursor == '\n' || (*cursor >= 32 && *cursor != 127));
        cursor++;
    }
}

static void test_start_success(void)
{
    reset_fake();
#if defined(P4_USB_LAUNCH_ECHO)
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
#else
    set_system_log("USBDiag: unauthenticated USB diagnosis explicitly enabled; "
                   "no file writes, drag or keyboard\n"
                   "[USBDiag] console mirror claimed\n"
                   "USBEcho: polled HS PIO ready; duration=600\n");
#endif
    assert(launch_diag_main() == RETURN_OK);
    assert(fake_library_open_count == 1);
    assert(fake_library_close_count == 1);
    assert(fake_request_count == 2);
    assert(fake_failure_request_count == 0);
    assert(fake_finished_request_count == 1);
    assert(fake_open_count == 4);
    assert(strcmp(fake_open_paths[0], EXPECTED_LOG_PATH) == 0);
    assert(fake_open_modes[0] == MODE_NEWFILE);
    assert(strcmp(fake_open_paths[1], "NIL:") == 0);
    assert(fake_open_modes[1] == MODE_OLDFILE);
    assert(strcmp(fake_open_paths[2], "NIL:") == 0);
    assert(fake_open_modes[2] == MODE_NEWFILE);
    assert(strcmp(fake_open_paths[3], EXPECTED_LOG_PATH) == 0);
    assert(fake_open_modes[3] == MODE_OLDFILE);
    assert(fake_system_count == 1);
    assert(strcmp(fake_command, EXPECTED_COMMAND) == 0);
    assert(fake_tag_count == 5);
    assert(fake_tags[0] == SYS_Input && fake_values[0] == 102);
    assert(fake_tags[1] == SYS_Output && fake_values[1] == 101);
    assert(fake_tags[2] == SYS_Error && fake_values[2] == 103);
    assert(fake_tags[3] == SYS_Asynch && fake_values[3] == FALSE);
    assert(fake_tags[4] == SYS_Background && fake_values[4] == TRUE);
    assert_closed_exactly(4);
    assert(fake_fib_alloc_count == 1);
    assert(fake_fib_free_count == 1);
    assert(fake_log_read_bytes <= USB_LOG_TAIL_MAX + 1);
    assert(strcmp(fake_consent_title, EXPECTED_CONSENT_TITLE) == 0);
    assert(strcmp(fake_consent_gadgets, "Start|Cancel") == 0);
    assert(strstr(fake_consent_text, "1209:0001") != NULL);
    assert(strstr(fake_consent_text, EXPECTED_LOG_PATH) != NULL);
    assert(strstr(fake_consent_text, "wait for FINISHED or FAILED") != NULL);
    assert(strcmp(fake_final_title, EXPECTED_FINISHED_TITLE) == 0);
    assert(strstr(fake_final_text, "DOS return: 0") != NULL);
    assert(strstr(fake_final_text, "Last known status: HS_POLLING_READY") != NULL);
    assert(strstr(fake_final_text, "elapsed time is not verified") != NULL);
    assert(strstr(fake_final_text, EXPECTED_LOG_PATH) != NULL);
#if defined(P4_USB_LAUNCH_ECHO)
    assert(strstr(fake_consent_text,
                  "not\nexposed by this USB service") != NULL);
    assert(strstr(fake_consent_text, "USB bulk accepts test bytes") != NULL);
    assert(strstr(fake_consent_text,
                  "No screen capture, filesystem access, pointer control or\n"
                  "input injection is enabled") != NULL);
    assert(strstr(fake_consent_text, "no authentication") == NULL);
    assert(strstr(fake_command, " SCREEN ") == NULL);
    assert(strstr(fake_command, " ROOT ") == NULL);
    assert(strstr(fake_command, " ALLOWMOUSE") == NULL);
    assert(strstr(fake_command, " UNATTENDED") == NULL);
    assert(strstr(fake_command, " NONEAUTH") == NULL);
#else
    assert(strstr(fake_consent_text, "not exposed by this USB service") != NULL);
    assert(strstr(fake_consent_text, "no authentication or encryption") != NULL);
    assert(strstr(fake_consent_text, "No local touch or\nmouse") != NULL);
#endif
    assert(strcmp(fake_request_gadgets, "OK") == 0);
    assert_text_layout(fake_consent_text, 24);
    assert_text_layout(fake_final_text, 6);
    assert_no_control_bytes(fake_final_text);
}

static void test_cancel_does_not_start_usb(void)
{
    reset_fake();
    fake_consent_result = 0;
    assert(launch_diag_main() == RETURN_OK);
    assert(fake_request_count == 1);
    assert(fake_failure_request_count == 0);
    assert(fake_finished_request_count == 0);
    assert(fake_open_count == 0);
    assert(fake_system_count == 0);
    assert(fake_library_close_count == 1);
}

static void test_library_open_failure(void)
{
    reset_fake();
    fake_library_open_fails = 1;
    assert(launch_diag_main() == RETURN_FAIL);
    assert(fake_library_open_count == 1);
    assert(fake_library_close_count == 0);
    assert(fake_request_count == 0);
    assert(fake_open_count == 0);
    assert(fake_system_count == 0);
}

static void test_file_open_failures_cleanup(void)
{
    int failure_at;

    for (failure_at = 1; failure_at <= 3; failure_at++) {
        reset_fake();
        fake_fail_open_at = failure_at;
        assert(launch_diag_main() == RETURN_FAIL);
        assert(fake_request_count == 2);
        assert(fake_failure_request_count == 1);
        assert(fake_finished_request_count == 0);
        assert(fake_open_count == (failure_at == 1 ? 1 : failure_at + 1));
        assert(fake_system_count == 0);
        assert(fake_library_close_count == 1);
        assert_closed_exactly(failure_at == 1 ? 0 : failure_at);
        assert(strstr(fake_final_text, "Setup failed:") != NULL);
        if (failure_at == 1) {
            assert(strstr(fake_final_text, "Setup failed: STDOUT_LOG_OPEN") != NULL);
            assert(strstr(fake_final_text, "DOS IoErr: 1001") != NULL);
        } else if (failure_at == 2) {
            assert(strstr(fake_final_text, "Setup failed: STDIN_OPEN") != NULL);
            assert(strstr(fake_final_text, "DOS IoErr: 1002") != NULL);
        } else {
            assert(strstr(fake_final_text, "Setup failed: STDERR_OPEN") != NULL);
            assert(strstr(fake_final_text, "DOS IoErr: 1003") != NULL);
        }
        assert(strstr(fake_final_text, "Last known status:") != NULL);
        assert(strstr(fake_final_text, EXPECTED_LOG_PATH) != NULL);
        assert_text_layout(fake_final_text, 6);
        assert_no_control_bytes(fake_final_text);
    }
}

static void test_log_open_failure_ignores_stale_log(void)
{
    static const char stale_log[] =
        "USBEcho: polled HS PIO ready; duration=600\n";

    reset_fake();
    fake_fail_open_at = 1;
    fake_log_created = 1;
    fake_log_size = sizeof(stale_log) - 1;
    memcpy(fake_log_data, stale_log, fake_log_size);
    assert(launch_diag_main() == RETURN_FAIL);
    assert(fake_open_count == 1);
    assert(strcmp(fake_final_title, EXPECTED_FAILURE_TITLE) == 0);
    assert(strstr(fake_final_text, "Setup failed: STDOUT_LOG_OPEN") != NULL);
    assert(strstr(fake_final_text, "Last known status: LOG_UNAVAILABLE") != NULL);
    assert(strstr(fake_final_text, "HS_POLLING_READY") == NULL);
}

static void test_system_result_is_preserved(void)
{
    reset_fake();
    set_system_log("USBEcho: PHY setup refused/failed\n");
    fake_system_result = 37;
    assert(launch_diag_main() == 37);
    assert(fake_system_count == 1);
    assert(fake_failure_request_count == 1);
    assert(fake_finished_request_count == 0);
    assert(strcmp(fake_final_title, EXPECTED_FAILURE_TITLE) == 0);
    assert(strstr(fake_final_text, "DOS return: 37") != NULL);
    assert(strstr(fake_final_text, "Last known status: PHY_SETUP_FAILED") != NULL);
    assert_closed_exactly(4);
    assert(fake_library_close_count == 1);
    assert_text_layout(fake_final_text, 6);
}

static void test_log_failure_marker_overrides_zero_exit(void)
{
    reset_fake();
    set_system_log("USBEcho: PHY setup refused/failed\n");
    fake_system_result = RETURN_OK;
    assert(launch_diag_main() == RETURN_OK);
    assert(strcmp(fake_final_title, EXPECTED_FAILURE_TITLE) == 0);
    assert(strstr(fake_final_text, "DOS return: 0") != NULL);
    assert(strstr(fake_final_text, "Last known status: PHY_SETUP_FAILED") != NULL);
    assert(fake_failure_request_count == 1);
    assert(fake_finished_request_count == 0);
    assert_text_layout(fake_final_text, 6);
}

static void test_missing_log_and_read_error(void)
{
    reset_fake();
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
    fake_fail_log_read_open = 1;
    assert(launch_diag_main() == RETURN_OK);
    assert(strcmp(fake_final_title, EXPECTED_FINISHED_TITLE) == 0);
    assert(strstr(fake_final_text, "Last known status: LOG_UNAVAILABLE") != NULL);
    assert(strstr(fake_final_text, "Log IoErr: 1204") != NULL);
    assert(fake_log_read_bytes == 0);

    reset_fake();
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
    fake_fail_read = 1;
    assert(launch_diag_main() == RETURN_OK);
    assert(strstr(fake_final_text, "Last known status: LOG_READ_ERROR") != NULL);
    assert(strstr(fake_final_text, "Log IoErr: 1301") != NULL);
    assert(fake_fib_alloc_count == fake_fib_free_count);
    assert_text_layout(fake_final_text, 6);
}

static void test_metadata_and_seek_errors_are_reported(void)
{
    reset_fake();
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
    fake_fail_fib_alloc = 1;
    assert(launch_diag_main() == RETURN_OK);
    assert(strstr(fake_final_text, "Last known status: LOG_READ_ERROR") != NULL);
    assert(strstr(fake_final_text, "Log IoErr: 1300") != NULL);
    assert(fake_fib_alloc_count == 1);
    assert(fake_fib_free_count == 0);

    reset_fake();
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
    fake_fail_examine = 1;
    assert(launch_diag_main() == RETURN_OK);
    assert(strstr(fake_final_text, "Last known status: LOG_READ_ERROR") != NULL);
    assert(strstr(fake_final_text, "Log IoErr: 1304") != NULL);
    assert(fake_fib_alloc_count == fake_fib_free_count);

    reset_fake();
    set_system_log("USBEcho: polled HS PIO ready; duration=600\n");
    fake_fail_seek = 1;
    assert(launch_diag_main() == RETURN_OK);
    assert(strstr(fake_final_text, "Last known status: LOG_READ_ERROR") != NULL);
    assert(strstr(fake_final_text, "Log IoErr: 1302") != NULL);
    assert(fake_fib_alloc_count == fake_fib_free_count);
    assert_text_layout(fake_final_text, 6);
}

static void test_restore_detail_code_is_sanitized(void)
{
    reset_fake();
    set_system_log("USBEcho: restore failed (-23); owner/snapshot quarantined "
                   "until reboot\n");
    fake_system_result = RETURN_FAIL;
    assert(launch_diag_main() == RETURN_FAIL);
    assert(strstr(fake_final_text,
                  "Last known status: PHY_RESTORE_QUARANTINED") != NULL);
    assert(strstr(fake_final_text, "Restore code: -23") != NULL);
    assert(strstr(fake_final_text, "owner/snapshot") == NULL);
    assert_no_control_bytes(fake_final_text);
}

#if !defined(P4_USB_LAUNCH_ECHO)
static void test_diagnostic_stage_marker(void)
{
    reset_fake();
    set_system_log("USBDiag: worker startup failed\n");
    fake_system_result = RETURN_FAIL;
    assert(launch_diag_main() == RETURN_FAIL);
    assert(strstr(fake_final_text,
                  "Last known status: DIAG_WORKER_FAILED") != NULL);
}
#endif

static void test_unknown_and_control_log_text_is_not_shown(void)
{
    reset_fake();
    set_system_log("secret=top-secret\nUSBEcho: \033[31mprivate-tail\n");
    fake_system_result = 37;
    assert(launch_diag_main() == 37);
    assert(strstr(fake_final_text, "Last known status: USB_RUNTIME_FAULT") != NULL);
    assert(strstr(fake_final_text, "top-secret") == NULL);
    assert(strstr(fake_final_text, "private-tail") == NULL);
    assert(strchr(fake_final_text, '\033') == NULL);
    assert_no_control_bytes(fake_final_text);
}

static void test_log_tail_is_bounded(void)
{
    static const char filler[] = "SECRET-DATA-DO-NOT-SHOW\n";
    static const char ready[] =
        "USBEcho: polled HS PIO ready; duration=600\n";
    size_t used = 0;
    size_t filler_length = sizeof(filler) - 1;

    reset_fake();
    while (used + filler_length + 64 < sizeof(fake_system_log)) {
        memcpy(fake_system_log + used, filler, filler_length);
        used += filler_length;
    }
    memcpy(fake_system_log + used, ready, sizeof(ready) - 1);
    used += sizeof(ready) - 1;
    fake_system_log_length = used;

    assert(launch_diag_main() == RETURN_OK);
    assert(fake_system_log_length > USB_LOG_TAIL_MAX + 1);
    assert(fake_log_read_bytes <= USB_LOG_TAIL_MAX + 1);
    assert(strstr(fake_final_text, "Last known status: HS_POLLING_READY") != NULL);
    assert(strstr(fake_final_text, "SECRET-DATA") == NULL);
    assert_text_layout(fake_final_text, 6);
}

int main(void)
{
    test_start_success();
    test_cancel_does_not_start_usb();
    test_library_open_failure();
    test_file_open_failures_cleanup();
    test_log_open_failure_ignores_stale_log();
    test_system_result_is_preserved();
    test_log_failure_marker_overrides_zero_exit();
    test_missing_log_and_read_error();
    test_metadata_and_seek_errors_are_reported();
    test_restore_detail_code_is_sanitized();
#if !defined(P4_USB_LAUNCH_ECHO)
    test_diagnostic_stage_marker();
#endif
    test_unknown_and_control_log_text_is_not_shown();
    test_log_tail_is_bounded();
#if defined(P4_USB_LAUNCH_ECHO)
    puts("echo launcher host fixture: 13 tests passed");
#else
    puts("diagnostic launcher host fixture: 14 tests passed");
#endif
    return 0;
}
