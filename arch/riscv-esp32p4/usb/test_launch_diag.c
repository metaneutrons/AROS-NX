/* Host fixture for the standalone consent-gated USBDiag launcher.
 * Build with: cc -std=c11 -Wall -Wextra -Werror test_launch_diag.c -o test_launch_diag
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
    CONST_STRPTR es_Text;
    CONST_STRPTR es_GadgetFormat;
};

#define BNULL ((BPTR)0)
#define FALSE 0
#define TRUE 1
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
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
LONG SystemTags(STRPTR command, ...);
struct Library *OpenLibrary(CONST_STRPTR name, ULONG version);
void CloseLibrary(struct Library *library);
LONG EasyRequestArgs(void *window, struct EasyStruct *request,
                     void *idcmp, void *args);

#define LAUNCH_DIAG_HOST_TEST 1
#define main launch_diag_main
#include "launch_diag.c"
#undef main

#define MAX_CALLS 8
#define MAX_TEXT 1200

static struct IntuitionBase fake_intuition;
static int fake_library_open_fails;
static int fake_library_open_count;
static int fake_library_close_count;
static int fake_consent_result;
static int fake_failure_result;
static int fake_request_count;
static int fake_failure_request_count;
static int fake_open_count;
static int fake_fail_open_at;
static int fake_close_count;
static int fake_system_count;
static LONG fake_system_result;
static BPTR fake_closed_handles[MAX_CALLS];
static char fake_open_paths[MAX_CALLS][64];
static LONG fake_open_modes[MAX_CALLS];
static char fake_request_title[128];
static char fake_request_text[MAX_TEXT];
static char fake_request_gadgets[128];
static IPTR fake_tags[16];
static IPTR fake_values[16];
static int fake_tag_count;
static char fake_command[256];

static void reset_fake(void)
{
    fake_library_open_fails = 0;
    fake_library_open_count = 0;
    fake_library_close_count = 0;
    fake_consent_result = 1;
    fake_failure_result = 1;
    fake_request_count = 0;
    fake_failure_request_count = 0;
    fake_open_count = 0;
    fake_fail_open_at = 0;
    fake_close_count = 0;
    fake_system_count = 0;
    fake_system_result = RETURN_OK;
    fake_tag_count = 0;
    fake_request_title[0] = '\0';
    fake_request_text[0] = '\0';
    fake_request_gadgets[0] = '\0';
    fake_command[0] = '\0';
    memset(fake_closed_handles, 0, sizeof(fake_closed_handles));
    memset(fake_open_paths, 0, sizeof(fake_open_paths));
    memset(fake_open_modes, 0, sizeof(fake_open_modes));
    memset(fake_tags, 0, sizeof(fake_tags));
    memset(fake_values, 0, sizeof(fake_values));
}

BPTR Open(CONST_STRPTR name, LONG mode)
{
    int call = ++fake_open_count;

    assert(call <= MAX_CALLS);
    snprintf(fake_open_paths[call - 1], sizeof(fake_open_paths[call - 1]),
             "%s", name);
    fake_open_modes[call - 1] = mode;
    if (call == fake_fail_open_at)
        return BNULL;
    return (BPTR)(100 + call);
}

LONG Close(BPTR file)
{
    assert(fake_close_count < MAX_CALLS);
    fake_closed_handles[fake_close_count++] = file;
    return RETURN_OK;
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
    snprintf(fake_request_title, sizeof(fake_request_title), "%s",
             request->es_Title);
    snprintf(fake_request_text, sizeof(fake_request_text), "%s",
             request->es_Text);
    snprintf(fake_request_gadgets, sizeof(fake_request_gadgets), "%s",
             request->es_GadgetFormat);
    if (strcmp(request->es_Title, "Private USB diagnostics") == 0)
        return fake_consent_result;
    assert(strcmp(request->es_Title, "USBDiag did not complete") == 0);
    fake_failure_request_count++;
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

static void assert_request_lines_fit(void)
{
    const char *line = fake_request_text;
    const char *cursor = line;

    while (*cursor != '\0') {
        if (*cursor == '\n') {
            assert((size_t)(cursor - line) <= 64);
            line = cursor + 1;
        }
        cursor++;
    }
    assert((size_t)(cursor - line) <= 64);
}

static void test_start_success(void)
{
    reset_fake();
    assert(launch_diag_main() == RETURN_OK);
    assert(fake_library_open_count == 1);
    assert(fake_library_close_count == 1);
    assert(fake_request_count == 1);
    assert(fake_failure_request_count == 0);
    assert(fake_open_count == 3);
    assert(strcmp(fake_open_paths[0], "RAM:USBDiag.log") == 0);
    assert(fake_open_modes[0] == MODE_NEWFILE);
    assert(strcmp(fake_open_paths[1], "NIL:") == 0);
    assert(fake_open_modes[1] == MODE_OLDFILE);
    assert(strcmp(fake_open_paths[2], "NIL:") == 0);
    assert(fake_open_modes[2] == MODE_NEWFILE);
    assert(fake_system_count == 1);
    assert(strcmp(fake_command,
                  "FLASHDISK0P0:USBDiag ENABLE VID 4617 PID 1 SECONDS 600 "
                  "STEADYCLOCKS NONEAUTH SCREEN Workbench ROOT SYS:Prefs "
                  "ALLOWMOUSE UNATTENDED") == 0);
    assert(fake_tag_count == 5);
    assert(fake_tags[0] == SYS_Input && fake_values[0] == 102);
    assert(fake_tags[1] == SYS_Output && fake_values[1] == 101);
    assert(fake_tags[2] == SYS_Error && fake_values[2] == 103);
    assert(fake_tags[3] == SYS_Asynch && fake_values[3] == FALSE);
    assert(fake_tags[4] == SYS_Background && fake_values[4] == TRUE);
    assert_closed_exactly(3);
    assert(strstr(fake_request_text, "1209:0001") != NULL);
    assert(strstr(fake_request_text, "no authentication or encryption") != NULL);
    assert(strstr(fake_request_text, "RAM:USBDiag.log") != NULL);
    assert(strstr(fake_request_text, "not exposed by this USB service") != NULL);
    assert(strstr(fake_request_text, "No local touch or\nmouse") != NULL);
    assert(strcmp(fake_request_gadgets, "Start|Cancel") == 0);
    assert_request_lines_fit();
}

static void test_cancel_does_not_start_usb(void)
{
    reset_fake();
    fake_consent_result = 0;
    assert(launch_diag_main() == RETURN_OK);
    assert(fake_request_count == 1);
    assert(fake_failure_request_count == 0);
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
        assert(fake_open_count == failure_at);
        assert(fake_system_count == 0);
        assert(fake_library_close_count == 1);
        assert_closed_exactly(failure_at - 1);
        assert(strstr(fake_request_text, "RAM:USBDiag.log") != NULL);
        assert(strstr(fake_request_text, "not exposed over USB") != NULL);
        assert_request_lines_fit();
    }
}

static void test_system_result_is_preserved(void)
{
    reset_fake();
    fake_system_result = 37;
    assert(launch_diag_main() == 37);
    assert(fake_system_count == 1);
    assert(fake_failure_request_count == 1);
    assert_closed_exactly(3);
    assert(fake_library_close_count == 1);
}

int main(void)
{
    test_start_success();
    test_cancel_does_not_start_usb();
    test_library_open_failure();
    test_file_open_failures_cleanup();
    test_system_result_is_preserved();
    puts("launcher host fixture: 5 tests passed");
    return 0;
}
