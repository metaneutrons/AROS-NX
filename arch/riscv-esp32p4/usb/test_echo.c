/* Host tests for the production echo-ring implementation. */
#include "echo.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool condition, const char *expression, int line)
{
    if (!condition)
    {
        fprintf(stderr, "test_echo:%d: check failed: %s\n", line, expression);
        exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static void check_same(const struct p4_echo *left, const struct p4_echo *right)
{
    CHECK(left->head == right->head);
    CHECK(left->used == right->used);
    CHECK(left->generation == right->generation);
    CHECK(left->received == right->received);
    CHECK(left->sent == right->sent);
    CHECK(memcmp(left->data, right->data, sizeof(left->data)) == 0);
}

static void test_wrap_and_partial_consumption(void)
{
    struct p4_echo echo;
    uint8_t initial[P4_ECHO_CAPACITY - 1];
    const uint8_t wrapped[] = { 0x91, 0x02, 0xa3, 0x14, 0xc5 };
    const uint8_t *view;
    size_t length;
    size_t i;

    memset(&echo, 0, sizeof(echo));
    p4_echo_reset(&echo);
    for (i = 0; i < sizeof(initial); ++i)
        initial[i] = (uint8_t)(i * 37u + 11u);

    CHECK(p4_echo_push(&echo, initial, sizeof(initial)));
    CHECK(echo.used == sizeof(initial));
    CHECK(echo.received == sizeof(initial));
    CHECK(p4_echo_consume(&echo, sizeof(initial)));
    CHECK(echo.used == 0);
    CHECK(echo.head == P4_ECHO_CAPACITY - 1);

    CHECK(p4_echo_push(&echo, wrapped, sizeof(wrapped)));
    CHECK(echo.used == sizeof(wrapped));
    CHECK(echo.received == sizeof(initial) + sizeof(wrapped));

    length = p4_echo_peek(&echo, &view);
    CHECK(length == 1);
    CHECK(view[0] == wrapped[0]);
    CHECK(p4_echo_consume(&echo, 1));

    length = p4_echo_peek(&echo, &view);
    CHECK(length == sizeof(wrapped) - 1);
    CHECK(view == echo.data);
    CHECK(memcmp(view, wrapped + 1, sizeof(wrapped) - 1) == 0);
    CHECK(p4_echo_consume(&echo, 2));

    length = p4_echo_peek(&echo, &view);
    CHECK(length == 2);
    CHECK(view[0] == wrapped[3] && view[1] == wrapped[4]);
    CHECK(p4_echo_consume(&echo, 2));
    CHECK(echo.used == 0);
    CHECK(echo.sent == sizeof(initial) + sizeof(wrapped));
}

static void test_zero_length_and_null_input(void)
{
    struct p4_echo echo;
    struct p4_echo before;
    const uint8_t payload[] = { 0x00, 0x7f, 0xff };

    memset(&echo, 0, sizeof(echo));
    p4_echo_reset(&echo);
    CHECK(p4_echo_push(&echo, NULL, 0));
    CHECK(p4_echo_consume(&echo, 0));
    before = echo;
    CHECK(!p4_echo_push(&echo, NULL, sizeof(payload)));
    check_same(&echo, &before);

    CHECK(p4_echo_push(&echo, payload, sizeof(payload)));
    before = echo;
    CHECK(p4_echo_push(&echo, NULL, 0));
    check_same(&echo, &before);
    CHECK(!p4_echo_push(&echo, NULL, 1));
    check_same(&echo, &before);
}

static void test_overflow_and_invalid_consume_are_atomic(void)
{
    struct p4_echo echo;
    struct p4_echo before;
    uint8_t full[P4_ECHO_CAPACITY];
    const uint8_t extra = 0x5a;
    size_t i;

    memset(&echo, 0, sizeof(echo));
    p4_echo_reset(&echo);
    for (i = 0; i < sizeof(full); ++i)
        full[i] = (uint8_t)(255u - (i * 13u));
    CHECK(p4_echo_push(&echo, full, sizeof(full)));
    before = echo;

    CHECK(!p4_echo_push(&echo, &extra, 1));
    check_same(&echo, &before);
    CHECK(!p4_echo_consume(&echo, P4_ECHO_CAPACITY + 1u));
    check_same(&echo, &before);
}

static void test_generation_reset_wrap(void)
{
    struct p4_echo echo;
    const uint8_t payload[] = { 1, 2, 3, 4 };
    size_t i;

    memset(&echo, 0, sizeof(echo));
    p4_echo_reset(&echo);
    CHECK(echo.generation == 1);
    CHECK(p4_echo_push(&echo, payload, sizeof(payload)));
    echo.generation = UINT32_MAX;

    p4_echo_reset(&echo);
    CHECK(echo.generation == 1);
    CHECK(echo.head == 0 && echo.used == 0);
    CHECK(echo.received == 0 && echo.sent == 0);
    for (i = 0; i < sizeof(echo.data); ++i)
        CHECK(echo.data[i] == 0);
}

int main(void)
{
    test_wrap_and_partial_consumption();
    test_zero_length_and_null_input();
    test_overflow_and_invalid_consume_are_atomic();
    test_generation_reset_wrap();
    puts("test_echo: pass");
    return EXIT_SUCCESS;
}
