/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hosted_request_ids.h"

static unsigned long checks;

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "line %u: %s\n", (unsigned)__LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void check_same(const struct p4_hosted_request_ids *actual,
                       const struct p4_hosted_request_ids *expected)
{
    CHECK(memcmp(actual, expected, sizeof *actual) == 0);
}

static void test_init_and_reserve(void)
{
    struct p4_hosted_request_ids ids, before;
    uint32_t uid;
    uintptr_t bad_address = UINTPTR_MAX - 1u;

    memset(&ids, 0, sizeof ids);
    before = ids;
    CHECK(!p4_hosted_request_ids_init(&ids, 0u));
    check_same(&ids, &before);
    CHECK(p4_hosted_request_ids_init(&ids, 17u));
    CHECK(ids.initialized == 1u && ids.retired == 0u);
    CHECK(ids.generation == 17u && ids.next_uid == 1u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    CHECK(ids.exhausted == 0u);

    before = ids;
    CHECK(!p4_hosted_request_ids_init(&ids, 17u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_init(&ids, 18u));
    check_same(&ids, &before);

    uid = UINT32_C(0xa5a55a5a);
    CHECK(!p4_hosted_request_ids_reserve(&ids, 16u, 284u, &uid));
    CHECK(uid == UINT32_C(0xa5a55a5a));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_reserve(&ids, 17u, 0u, &uid));
    CHECK(uid == UINT32_C(0xa5a55a5a));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_reserve(&ids, 17u,
                                        UINT32_MAX - 255u, &uid));
    CHECK(uid == UINT32_C(0xa5a55a5a));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_reserve(&ids, 17u, 284u, NULL));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_reserve(
        &ids, 17u, 284u, (uint32_t *)bad_address));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_reserve(
        (struct p4_hosted_request_ids *)bad_address, 17u, 284u, &uid));
    CHECK(uid == UINT32_C(0xa5a55a5a));
    CHECK(!p4_hosted_request_ids_init(
        (struct p4_hosted_request_ids *)bad_address, 17u));

    CHECK(!p4_hosted_request_ids_reserve(
        &ids, 17u, 284u, (uint32_t *)(void *)&ids.next_uid));
    check_same(&ids, &before);
    CHECK(p4_hosted_request_ids_reserve(&ids, 17u, 284u, &uid));
    CHECK(uid == 1u);
    CHECK(ids.next_uid == 2u && ids.active_uid == 1u);
    CHECK(ids.active_request_id == 284u && ids.exhausted == 0u);

    before = ids;
    uid = 99u;
    CHECK(!p4_hosted_request_ids_reserve(&ids, 17u, 282u, &uid));
    CHECK(uid == 99u);
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 16u, 1u, 284u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 17u, 2u, 284u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 17u, 1u, 283u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 17u, 1u, 0u));
    check_same(&ids, &before);
    CHECK(p4_hosted_request_ids_finish(&ids, 17u, 1u, 284u));
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    CHECK(ids.next_uid == 2u);

    CHECK(p4_hosted_request_ids_reserve(&ids, 17u,
                                        UINT32_MAX - 256u, &uid));
    CHECK(uid == 2u);
    CHECK(p4_hosted_request_ids_finish(&ids, 17u, uid,
                                       UINT32_MAX - 256u));
    CHECK(ids.next_uid == 3u && ids.active_uid == 0u);
}

static void test_retire_and_rebind(void)
{
    struct p4_hosted_request_ids ids, before;
    uint32_t uid, stale_uid;

    memset(&ids, 0, sizeof ids);
    CHECK(p4_hosted_request_ids_init(&ids, 41u));
    CHECK(p4_hosted_request_ids_reserve(&ids, 41u, 282u, &uid));
    CHECK(uid == 1u);
    before = ids;
    CHECK(!p4_hosted_request_ids_init(&ids, 42u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_invalidate(&ids, 40u));
    check_same(&ids, &before);

    CHECK(p4_hosted_request_ids_invalidate(&ids, 41u));
    CHECK(ids.retired == 1u);
    CHECK(ids.active_uid == 0u && ids.active_request_id == 0u);
    CHECK(ids.next_uid == 2u && ids.generation == 41u);
    before = ids;
    stale_uid = 77u;
    CHECK(!p4_hosted_request_ids_reserve(&ids, 41u, 282u, &stale_uid));
    CHECK(stale_uid == 77u);
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 41u, 1u, 282u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_invalidate(&ids, 41u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_init(&ids, 41u));
    check_same(&ids, &before);

    CHECK(p4_hosted_request_ids_init(&ids, 42u));
    CHECK(ids.generation == 42u && ids.retired == 0u);
    CHECK(ids.next_uid == 2u && ids.exhausted == 0u);
    before = ids;
    CHECK(!p4_hosted_request_ids_reserve(&ids, 41u, 282u, &stale_uid));
    CHECK(stale_uid == 77u);
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_finish(&ids, 41u, 1u, 282u));
    check_same(&ids, &before);
    CHECK(!p4_hosted_request_ids_invalidate(&ids, 41u));
    check_same(&ids, &before);

    CHECK(p4_hosted_request_ids_reserve(&ids, 42u, 283u, &uid));
    CHECK(uid == 2u);
    before = ids;
    CHECK(!p4_hosted_request_ids_init(&ids, 43u));
    check_same(&ids, &before);
    CHECK(p4_hosted_request_ids_invalidate(&ids, 42u));
    CHECK(p4_hosted_request_ids_init(&ids, 43u));
    CHECK(ids.generation == 43u && ids.next_uid == 3u);
    CHECK(p4_hosted_request_ids_reserve(&ids, 43u, 284u, &uid));
    CHECK(uid == 3u);
}

static void test_max_uid_exhaustion(void)
{
    struct p4_hosted_request_ids ids, before;
    uint32_t uid = 0u;

    memset(&ids, 0, sizeof ids);
    CHECK(p4_hosted_request_ids_init(&ids, 90u));
    /* Reach the boundary without billions of loop iterations. */
    ids.next_uid = UINT32_MAX;
    before = ids;
    CHECK(p4_hosted_request_ids_reserve(&ids, 90u, 284u, &uid));
    CHECK(uid == UINT32_MAX);
    CHECK(ids.next_uid == UINT32_MAX && ids.exhausted == 1u);
    CHECK(ids.active_uid == UINT32_MAX && ids.active_request_id == 284u);
    CHECK(p4_hosted_request_ids_finish(&ids, 90u, UINT32_MAX, 284u));
    before = ids;
    uid = 0x13572468u;
    CHECK(!p4_hosted_request_ids_reserve(&ids, 90u, 284u, &uid));
    CHECK(uid == 0x13572468u);
    check_same(&ids, &before);
    CHECK(p4_hosted_request_ids_invalidate(&ids, 90u));
    CHECK(p4_hosted_request_ids_init(&ids, 91u));
    CHECK(ids.next_uid == UINT32_MAX && ids.exhausted == 1u);
    before = ids;
    CHECK(!p4_hosted_request_ids_reserve(&ids, 91u, 284u, &uid));
    CHECK(uid == 0x13572468u);
    check_same(&ids, &before);
}

int main(void)
{
    test_init_and_reserve();
    test_retire_and_rebind();
    test_max_uid_exhaustion();
    printf("hosted request IDs: %lu checks\n", checks);
    return 0;
}
