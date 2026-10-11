/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <devices/timer.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>
#include "boardtool.h"
#include "../../../arch/riscv-esp32p4/network/runtime_probe.h"

struct Device *TimerBase;
struct discovery_native {
    struct P4WifiBase *radio;
    struct MsgPort *port;
    struct timerequest *timer;
    ULONG frequency;
    UQUAD origin;
    struct p4_wifi_discovery_result reply;
    struct board_client_workspace *work;
};

typedef char discovery_abi_matches[(P4_WIFI_DEVICE_BRIDGE_ABI == 11u &&
    P4_WIFI_MAINT_IDLE == BOARD_CLIENT_LEASE_IDLE &&
    P4_WIFI_MAINT_REQUESTED == BOARD_CLIENT_LEASE_REQUESTED &&
    P4_WIFI_MAINT_ACTIVE == BOARD_CLIENT_LEASE_ACTIVE &&
    P4_WIFI_MAINT_RELEASING == BOARD_CLIENT_LEASE_RELEASING &&
    P4_WIFI_MAINT_FAILED == BOARD_CLIENT_LEASE_FAILED &&
    P4_WIFI_MAINT_RELEASED == BOARD_CLIENT_LEASE_RELEASED &&
    P4_WIFI_MAINT_BUSY == BOARD_CLIENT_LEASE_BUSY &&
    P4_WIFI_DISCOVERY_IDLE == BOARD_CLIENT_DISCOVERY_IDLE &&
    P4_WIFI_DISCOVERY_REQUESTED == BOARD_CLIENT_DISCOVERY_REQUESTED &&
    P4_WIFI_DISCOVERY_READY == BOARD_CLIENT_DISCOVERY_READY &&
    P4_WIFI_DISCOVERY_UNSUPPORTED == BOARD_CLIENT_DISCOVERY_UNSUPPORTED &&
    P4_WIFI_DISCOVERY_FAILED == BOARD_CLIENT_DISCOVERY_FAILED &&
    P4_WIFI_DISCOVERY_BUSY == BOARD_CLIENT_DISCOVERY_BUSY &&
    P4_WIFI_DISCOVERY_MAX == BOARD_DISCOVERY_WIRE_MAX &&
    P4_WIFI_MAINT_LEASE_US / 1000u == BOARD_CLIENT_TOTAL_MS) ? 1 : -1];

static int native_clock(void *opaque, uint32_t *milliseconds)
{
    struct discovery_native *context = opaque;
    struct EClockVal ev;
    ULONG hz = ReadEClock(&ev);
    UQUAD now = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
    if (!hz || hz != context->frequency || now < context->origin) return 0;
    /* Relative ticks avoid multiplying an arbitrarily old absolute clock. */
    now -= context->origin;
    *milliseconds = (uint32_t)((now / hz) * 1000u + (now % hz) * 1000u / hz);
    return 1;
}
static void native_wait(void *opaque) { (void)opaque; Delay(1); }
static int native_cancelled(void *opaque)
{ (void)opaque; return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0; }
static uint32_t native_request(void *opaque)
{
    struct discovery_native *c = opaque;
    return c->radio->maintenance_request(c->radio);
}
static unsigned native_lease_poll(void *opaque, uint32_t token)
{
    struct discovery_native *c = opaque;
    return c->radio->maintenance_poll(c->radio, token);
}
static int native_discover(void *opaque, uint32_t token)
{
    struct discovery_native *c = opaque;
    return c->radio->maintenance_discover(c->radio, token);
}
static unsigned native_discovery_poll(void *opaque, uint32_t token,
    struct board_client_reply *reply)
{
    struct discovery_native *c = opaque;
    ULONG state = c->radio->maintenance_discovery_poll(c->radio, token, &c->reply);
    if (state == P4_WIFI_DISCOVERY_READY) {
        if (c->reply.inventory_size > sizeof(reply->inventory))
            return BOARD_CLIENT_DISCOVERY_FAILED;
        reply->token = c->reply.token; reply->generation = c->reply.generation;
        reply->uid = c->reply.uid; reply->nonce = c->reply.nonce;
        reply->remote_status = c->reply.remote_status;
        reply->inventory_size = c->reply.inventory_size;
        CopyMem(c->reply.inventory, reply->inventory, c->reply.inventory_size);
    }
    return state;
}
static int native_release(void *opaque, uint32_t token)
{
    struct discovery_native *c = opaque;
    return c->radio->maintenance_release(c->radio, token);
}
static const struct board_client_ops native_ops = {
    native_clock, native_wait, native_cancelled, native_request,
    native_lease_poll, native_discover, native_discovery_poll, native_release
};

int boardtool_discover_c6(struct board_client_observation *output,
    char *status, size_t capacity)
{
    struct discovery_native *c;
    struct EClockVal ev;
    enum board_client_result result = BOARD_CLIENT_UNAVAILABLE;
    const char *setup_failure = NULL;
    int opened = 0;
    static const char *const errors[] = {
        "ok", "invalid storage/hooks", "owner unavailable or busy",
        "deadline expired", "cancelled", "owner/query failed",
        "installed C6 does not support discovery v2",
        "C6 metadata collection failed", "invalid correlated inventory",
        "release unconfirmed; session may retire; no automatic reset",
        "monotonic clock unavailable"
    };
    if (!output || !status || !capacity) return 0;
    c = AllocVec(sizeof(*c), MEMF_PUBLIC | MEMF_CLEAR);
    if (!c) { snprintf(status, capacity, "Discovery: no scratch memory; no query/write"); return 0; }
    c->work = AllocVec(sizeof(*c->work), MEMF_PUBLIC | MEMF_CLEAR);
    if (!c->work) { setup_failure = "no workspace memory"; goto done; }
    c->radio = (struct P4WifiBase *)OpenResource(P4_WIFI_DEVICE_BRIDGE_NAME);
    if (!c->radio) { setup_failure = "radio resource not installed"; goto done; }
    if (c->radio->bridge_abi != P4_WIFI_DEVICE_BRIDGE_ABI ||
        c->radio->bridge_size < sizeof(*c->radio) ||
        !c->radio->maintenance_request || !c->radio->maintenance_poll ||
        !c->radio->maintenance_release || !c->radio->maintenance_discover ||
        !c->radio->maintenance_discovery_poll || !c->radio->maintenance_fence) {
        setup_failure = "incompatible resource; matched ABI11 owner/device required";
        goto done;
    }
    c->port = CreateMsgPort();
    if (!c->port) { setup_failure = "cannot allocate timer port"; goto done; }
    c->timer = (struct timerequest *)CreateIORequest(c->port, sizeof(*c->timer));
    if (!c->timer) { setup_failure = "cannot allocate timer request"; goto done; }
    if (OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)c->timer, 0)) {
        setup_failure = "cannot open monotonic EClock timer"; goto done;
    }
    opened = 1;
    TimerBase = (struct Device *)c->timer->tr_node.io_Device;
    c->frequency = ReadEClock(&ev);
    c->origin = ((UQUAD)ev.ev_hi << 32) | ev.ev_lo;
    if (!c->frequency) { result = BOARD_CLIENT_CLOCK_FAILED; goto done; }
    result = board_client_discover(&native_ops, c, sizeof(*c), c->work, output);
done:
    if (opened) CloseDevice((struct IORequest *)c->timer);
    TimerBase = NULL;
    if (c->timer) DeleteIORequest((struct IORequest *)c->timer);
    if (c->port) DeleteMsgPort(c->port);
    if (c->work) FreeVec(c->work);
    FreeVec(c);
    if (result == BOARD_CLIENT_OK) {
        snprintf(status, capacity,
            "C6 registry observation: physical %lu MiB, usable %lu MiB; "
            "generation %lu UID %lu nonce %lu. Release confirmed; WLAN offline. "
            "Historical, not raw-table attestation or write/recovery permission.",
            (unsigned long)(output->snapshot.physical_bytes / 1048576u),
            (unsigned long)(output->snapshot.layout.flash_bytes / 1048576u),
            (unsigned long)output->generation, (unsigned long)output->uid,
            (unsigned long)output->nonce);
        return 1;
    }
    snprintf(status, capacity, "C6 discovery: %s; no new layout/write. "
        "Any existing preview remains historical.",
        setup_failure ? setup_failure : errors[result]);
    return 0;
}
