/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
/* Source-extracted ABI10 hook-registration tests. The runtime bridge header
 * and actual hook initializer are compiled with minimal host Exec types; the
 * owner entry points are typed identity mocks, not transport tests. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXEC_TYPES_H 1
#define EXEC_LIBRARIES_H 1
#define EXEC_SEMAPHORES_H 1
typedef void *APTR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint8_t UBYTE;
typedef int16_t BOOL;
#define TRUE ((BOOL)1)
#define FALSE ((BOOL)0)
static BOOL wifi_ota_update_enabled = FALSE;
struct ExecBase { ULONG unused; };
struct Task { ULONG unused; };
struct Library { ULONG unused; };
struct SignalSemaphore { struct Task *ss_Owner; ULONG depth; };

#include "runtime_probe.h"

#if P4_WIFI_DEVICE_BRIDGE_ABI != 11u
#error "wifi OTA hooks must use the matched discovery-v2 bridge ABI 11"
#endif

static unsigned test_checks;
#define CHECK(expression) do { \
    ++test_checks; \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #expression); \
        exit(1); \
    } \
} while (0)

uint32_t p4_wifi_ota_submit(struct P4WifiBase *base,
    const struct p4_wifi_ota_request *request)
{
    (void)base;
    (void)request;
    return 0u;
}

enum p4_wifi_ota_job_state p4_wifi_ota_poll(struct P4WifiBase *base,
    uint32_t token, struct p4_wifi_ota_observation *output)
{
    (void)base;
    (void)token;
    (void)output;
    return P4_WIFI_OTA_JOB_INVALID;
}

int p4_wifi_ota_cancel(struct P4WifiBase *base, uint32_t token)
{
    (void)base;
    (void)token;
    return 0;
}

int p4_wifi_ota_discard(struct P4WifiBase *base, uint32_t token)
{
    (void)base;
    (void)token;
    return 0;
}

#include "wifi_ota_hooks_extract.inc"

static void test_hook_initializer(BOOL association_enabled)
{
    struct P4WifiBase base;
    memset(&base, 0xa5, sizeof(base));
    base.association_enabled = association_enabled;
    base.associated = FALSE;
    wifi_ota_hooks_init(&base);
#if P4_WIFI_HOSTED_OWNER && P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED && \
    P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED
    CHECK(wifi_ota_update_enabled == TRUE);
    CHECK(base.ota_submit == p4_wifi_ota_submit);
    CHECK(base.ota_poll == p4_wifi_ota_poll);
    CHECK(base.ota_cancel == p4_wifi_ota_cancel);
    CHECK(base.ota_discard == p4_wifi_ota_discard);
#else
    CHECK(wifi_ota_update_enabled == FALSE);
    CHECK(base.ota_submit == NULL);
    CHECK(base.ota_poll == NULL);
    CHECK(base.ota_cancel == NULL);
    CHECK(base.ota_discard == NULL);
#endif
    CHECK(base.association_enabled == association_enabled);
    CHECK(base.associated == FALSE);
}

int main(void)
{
    CHECK(P4_WIFI_DEVICE_BRIDGE_ABI == 11u);
    test_hook_initializer(FALSE);
    test_hook_initializer(TRUE);
    printf("wifi OTA bridge hooks (owner=%u c6privacy=%u host=%u c6update=%u enabled=%u, both association settings): %u checks passed\n",
        (unsigned)P4_WIFI_HOSTED_OWNER,
        (unsigned)P4_WIFI_C6_PRIVACY_RECOVERY_VERIFIED,
        (unsigned)P4_WIFI_HOST_PRIVACY_RECOVERY_VERIFIED,
        (unsigned)P4_WIFI_C6_UPDATE_RECOVERY_VERIFIED,
        (unsigned)(wifi_ota_update_enabled == TRUE), test_checks);
    return 0;
}
