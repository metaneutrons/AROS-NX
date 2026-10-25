#include "backend.h"
#include "screen_test_native.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static void test_explicit_public_screen(void)
{
    struct rd_screen *screen;

    rd_test_reset();
    CHECK(rd_screen_open(NULL, false) == NULL);
    CHECK(rd_screen_open("", false) == NULL);
    CHECK(rd_test.lock_calls == 0);

    rd_test.screen_available = false;
    screen = rd_screen_open("NoSuchPublicScreen", false);
    CHECK(screen == NULL);
    CHECK(rd_test.lock_calls == 1);
    CHECK(strcmp(rd_test.last_screen_name, "NoSuchPublicScreen") == 0);
    CHECK(rd_test.unlock_calls == 0);
    CHECK(rd_test.close_library_calls == 2);
}

static void test_open_failure_cleanup(void)
{
    struct rd_screen *screen;

    rd_test_reset();
    rd_test.fail_cyber_open = true;
    screen = rd_screen_open("Public", false);
    CHECK(screen == NULL);
    CHECK(rd_test.open_library_calls == 2);
    CHECK(rd_test.close_library_calls == 1);
    CHECK(rd_test.lock_calls == 0);
}

static void check_mouse_setup_falls_back(bool fail_port, bool fail_request,
    bool fail_device)
{
    struct rd_screen *screen;
    struct rd_snapshot snapshot;

    rd_test_reset();
    rd_test.fail_msg_port = fail_port;
    rd_test.fail_io_request = fail_request;
    rd_test.fail_input_open = fail_device;
    screen = rd_screen_open("Public", true);
    CHECK(screen != NULL);
    CHECK(!rd_screen_has_mouse(screen));
    CHECK(rd_screen_mouse(screen, 1, 1, 0) == RD_DENIED);
    CHECK(rd_test.doio_calls == 0);

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(rd_screen_capture(screen, 0, 0, 2, 2, &snapshot) == RD_OK);
    CHECK(snapshot.data != NULL && snapshot.size == 12);
    CHECK(snapshot.width == 2 && snapshot.height == 2);
    FreeVec(snapshot.data);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);

    CHECK(rd_test.msg_port_creates == 1);
    CHECK(rd_test.msg_port_deletes == (fail_port ? 0 : 1));
    CHECK(rd_test.io_request_creates == (fail_port ? 0 : 1));
    CHECK(rd_test.io_request_deletes == (fail_device ? 1 : 0));
    CHECK(rd_test.input_opens == (fail_device ? 1 : 0));
    CHECK(rd_test.input_closes == 0);
    rd_screen_close(screen);
    CHECK(rd_test.close_library_calls == 2);
}

static void test_mouse_setup_degrades_to_screenshot(void)
{
    check_mouse_setup_falls_back(true, false, false);
    check_mouse_setup_falls_back(false, true, false);
    check_mouse_setup_falls_back(false, false, true);
}

static void test_capture_validation_and_copy(void)
{
    struct rd_screen *screen;
    struct rd_snapshot snapshot;
    int frees_before;

    rd_test_reset();
    screen = rd_screen_open("Public", false);
    CHECK(screen != NULL);

    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK(rd_screen_capture(screen, 0, 0, 0, 0, &snapshot) == RD_OK);
    CHECK(snapshot.width == 640 && snapshot.height == 480);
    CHECK(snapshot.size == 640u * 480u * 3u);
    CHECK(snapshot.data != NULL);
    CHECK(rd_test.read_calls == 1);
    CHECK(rd_test.read_dest_modulo == 640u * 3u);
    CHECK(rd_test.read_src_x == 0 && rd_test.read_src_y == 0);
    CHECK(rd_test.read_width == 640 && rd_test.read_height == 480);
    CHECK(rd_test.read_format == RECTFMT_RGB);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);
    FreeVec(snapshot.data);

    CHECK(rd_screen_capture(screen, 12, 7, 20, 13, &snapshot) == RD_OK);
    CHECK(snapshot.width == 20 && snapshot.height == 13);
    CHECK(snapshot.size == 20u * 13u * 3u);
    CHECK(rd_test.read_src_x == 12 && rd_test.read_src_y == 7);
    CHECK(rd_test.read_dest_modulo == 60);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);
    FreeVec(snapshot.data);

    CHECK(rd_screen_capture(screen, 0, 0, 0, 4, &snapshot) == RD_BAD_REQUEST);
    CHECK(snapshot.data == NULL && snapshot.size == 0);
    CHECK(rd_screen_capture(screen, 1, 0, 0, 0, &snapshot) == RD_BAD_REQUEST);
    CHECK(rd_screen_capture(screen, UINT32_MAX, 0, 1, 1, &snapshot) == RD_BAD_REQUEST);
    CHECK(rd_screen_capture(screen, 639, 479, 2, 1, &snapshot) == RD_BAD_REQUEST);
    CHECK(rd_test.read_calls == 2);

    rd_test.screen.Width = 2000;
    rd_test.screen.Height = 2000;
    CHECK(rd_screen_capture(screen, 0, 0, 0, 0, &snapshot) == RD_LIMIT);
    CHECK(snapshot.data == NULL && snapshot.size == 0);
    CHECK(rd_test.read_calls == 2);

    rd_test.screen.Width = 30000;
    rd_test.screen.Height = 1;
    CHECK(rd_screen_capture(screen, 0, 0, 30000, 1, &snapshot) == RD_LIMIT);
    CHECK(rd_test.read_calls == 2);

    rd_test.screen.Width = 640;
    rd_test.screen.Height = 480;
    rd_test.override_read_count = true;
    rd_test.read_count = 3;
    frees_before = rd_test.free_calls;
    CHECK(rd_screen_capture(screen, 0, 0, 2, 2, &snapshot) == RD_IO);
    CHECK(snapshot.data == NULL && snapshot.size == 0 && snapshot.width == 0);
    CHECK(rd_test.free_calls == frees_before + 1);

    rd_test.fail_alloc_call = true;
    CHECK(rd_screen_capture(screen, 0, 0, 1, 1, &snapshot) == RD_IO);
    CHECK(snapshot.data == NULL && snapshot.size == 0);
    rd_screen_close(screen);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);
    CHECK(rd_test.close_library_calls == 2);
}

static void test_mouse_moves_and_clicks(void)
{
    static const UWORD codes[] = { 0, IECODE_LBUTTON, IECODE_RBUTTON, IECODE_MBUTTON };
    static const UWORD qualifiers[] = {
        0, IEQUALIFIER_LEFTBUTTON, IEQUALIFIER_RBUTTON, IEQUALIFIER_MIDBUTTON
    };
    struct rd_screen *screen;
    int button;

    rd_test_reset();
    screen = rd_screen_open("Public", true);
    CHECK(screen != NULL);
    CHECK(rd_screen_has_mouse(screen));
    CHECK(rd_test.input_opens == 1);

    rd_test.qualifier = IEQUALIFIER_CONTROL;
    CHECK(rd_screen_mouse(screen, 123, 234, 0) == RD_OK);
    CHECK(rd_test.doio_calls == 1);
    CHECK(rd_test.events[0].event_class == IECLASS_NEWPOINTERPOS);
    CHECK(rd_test.events[0].sub_class == IESUBCLASS_PIXEL);
    CHECK(rd_test.events[0].code == IECODE_NOBUTTON);
    CHECK(rd_test.events[0].has_pointer);
    CHECK(rd_test.events[0].pointer_screen == &rd_test.screen);
    CHECK(rd_test.events[0].pointer_x == 123 && rd_test.events[0].pointer_y == 234);

    for (button = 1; button <= 3; ++button) {
        int first_event = rd_test.doio_calls;
        CHECK(rd_screen_mouse(screen, (uint32_t)(button * 10),
            (uint32_t)(button * 20), (unsigned)button) == RD_OK);
        CHECK(rd_test.doio_calls == first_event + 3);
        CHECK(rd_test.events[first_event].event_class == IECLASS_NEWPOINTERPOS);
        CHECK(rd_test.events[first_event + 1].event_class == IECLASS_RAWMOUSE);
        CHECK(rd_test.events[first_event + 1].code == codes[button]);
        CHECK((rd_test.events[first_event + 1].qualifier & qualifiers[button]) != 0);
        CHECK((rd_test.events[first_event + 1].qualifier & IEQUALIFIER_CONTROL) != 0);
        CHECK(rd_test.events[first_event + 2].event_class == IECLASS_RAWMOUSE);
        CHECK(rd_test.events[first_event + 2].code ==
            (UWORD)(codes[button] | IECODE_UP_PREFIX));
        CHECK((rd_test.events[first_event + 2].qualifier & qualifiers[button]) == 0);
        CHECK((rd_test.events[first_event + 2].qualifier & IEQUALIFIER_CONTROL) != 0);
        CHECK((rd_test.qualifier & (IEQUALIFIER_LEFTBUTTON |
            IEQUALIFIER_RBUTTON | IEQUALIFIER_MIDBUTTON)) == 0);
    }

    {
        int calls = rd_test.doio_calls;
        rd_test.qualifier |= IEQUALIFIER_LEFTBUTTON;
        CHECK(rd_screen_mouse(screen, 300, 300, 0) == RD_DENIED);
        CHECK(rd_screen_mouse(screen, 300, 300, 1) == RD_DENIED);
        CHECK(rd_test.doio_calls == calls);
        rd_test.qualifier &= (ULONG)~IEQUALIFIER_LEFTBUTTON;
    }

    CHECK(rd_screen_mouse(screen, 640, 1, 0) == RD_BAD_REQUEST);
    CHECK(rd_screen_mouse(screen, 1, 1, 4) == RD_BAD_REQUEST);
    rd_screen_close(screen);
    CHECK(rd_test.input_closes == 1);
    CHECK(rd_test.io_request_deletes == 1);
    CHECK(rd_test.msg_port_deletes == 1);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);
}

static void test_mouse_failure_releases(void)
{
    struct rd_screen *screen;

    rd_test_reset();
    screen = rd_screen_open("Public", true);
    CHECK(screen != NULL);
    rd_test.fail_doio_call = 2;
    CHECK(rd_screen_mouse(screen, 5, 6, 1) == RD_IO);
    CHECK(rd_test.doio_calls == 3);
    CHECK(rd_test.events[1].code == IECODE_LBUTTON);
    CHECK(rd_test.events[2].code == (IECODE_LBUTTON | IECODE_UP_PREFIX));
    CHECK((rd_test.qualifier & IEQUALIFIER_LEFTBUTTON) == 0);
    rd_screen_close(screen);

    rd_test_reset();
    screen = rd_screen_open("Public", true);
    CHECK(screen != NULL);
    rd_test.fail_doio_call = 3;
    CHECK(rd_screen_mouse(screen, 5, 6, 2) == RD_IO);
    CHECK(rd_test.doio_calls == 3);
    CHECK(rd_test.events[2].code == (IECODE_RBUTTON | IECODE_UP_PREFIX));
    rd_screen_close(screen);
    CHECK(rd_test.doio_calls == 4);
    CHECK(rd_test.events[3].code == (IECODE_RBUTTON | IECODE_UP_PREFIX));
    CHECK(rd_test.input_closes == 1);
}

static void test_physical_press_after_lock_is_denied(void)
{
    struct rd_screen *screen;
    int calls;

    rd_test_reset();
    screen = rd_screen_open("Public", true);
    CHECK(screen != NULL);
    calls = rd_test.doio_calls;
    rd_test.press_button_on_lock = true;
    rd_test.qualifier_on_lock = IEQUALIFIER_RBUTTON;
    CHECK(rd_screen_mouse(screen, 15, 20, 1) == RD_DENIED);
    CHECK(rd_test.doio_calls == calls);
    CHECK(rd_test.events[0].event_class == 0);
    CHECK(rd_test.unlock_calls == rd_test.lock_calls);
    rd_screen_close(screen);
}

static void test_mouse_is_opt_in(void)
{
    struct rd_screen *screen;

    rd_test_reset();
    screen = rd_screen_open("Public", false);
    CHECK(screen != NULL);
    CHECK(rd_test.input_opens == 0);
    CHECK(!rd_screen_has_mouse(screen));
    CHECK(rd_screen_mouse(screen, 1, 1, 0) == RD_DENIED);
    rd_screen_close(screen);
    CHECK(rd_test.input_opens == 0);
}

int main(void)
{
    test_explicit_public_screen();
    test_open_failure_cleanup();
    test_mouse_setup_degrades_to_screenshot();
    test_capture_validation_and_copy();
    test_mouse_moves_and_clicks();
    test_mouse_failure_releases();
    test_physical_press_after_lock_is_denied();
    test_mouse_is_opt_in();

    if (failures != 0) {
        fprintf(stderr, "%d screen backend check(s) failed\n", failures);
        return 1;
    }

    puts("screen backend host checks passed");
    return 0;
}
