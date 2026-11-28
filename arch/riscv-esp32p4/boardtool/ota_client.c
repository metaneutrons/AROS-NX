/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <string.h>
#include "ota_client.h"

static int terminal_shape_valid(const struct p4_board_update_result *r)
{
    uint32_t chunks = r->image_size / P4_BOARD_UPDATE_CHUNK_SIZE +
        (r->image_size % P4_BOARD_UPDATE_CHUNK_SIZE != 0);
    uint32_t acknowledged_chunks = r->acknowledged_bytes /
        P4_BOARD_UPDATE_CHUNK_SIZE +
        (r->acknowledged_bytes % P4_BOARD_UPDATE_CHUNK_SIZE != 0);
    return (unsigned)r->status <= P4_BOARD_UPDATE_CLOCK_FAILED &&
        r->status != P4_BOARD_UPDATE_INSTALLED &&
        r->status != P4_BOARD_UPDATE_POSTBOOT_FAILED &&
        (unsigned)r->finish_mode <= P4_BOARD_UPDATE_FINISH_WAIT_REBOOT &&
        (unsigned)r->budget_error <= P4_BOARD_UPDATE_BUDGET_CLOCK &&
        r->lease_acquired <= 1 && r->lease_release_called <= 1 &&
        r->begin_attempted <= 1 && r->end_attempted <= 1 &&
        (!r->begin_attempted || r->lease_acquired) &&
        (!r->end_attempted || r->begin_attempted) &&
        (!r->acknowledged_bytes || r->begin_attempted) &&
        (!r->end_attempted || r->acknowledged_bytes == r->image_size) &&
        r->image_size <= P4_BOARD_UPDATE_MAX_IMAGE_SIZE &&
        r->acknowledged_bytes <= r->image_size &&
        r->total_chunks == chunks &&
        r->acknowledged_chunks == acknowledged_chunks &&
        r->acknowledged_chunks <= r->total_chunks &&
        r->total_chunks <= P4_BOARD_UPDATE_MAX_CHUNKS &&
        r->acknowledged_bytes == (r->acknowledged_chunks == r->total_chunks ?
            r->image_size : r->acknowledged_chunks * P4_BOARD_UPDATE_CHUNK_SIZE);
}

void board_ota_client_init(struct board_ota_client *client)
{
    if (client) memset(client, 0, sizeof(*client));
}

int board_ota_client_start(struct board_ota_client *client,
    const struct board_ota_client_ops *ops, void *context,
    const struct p4_wifi_ota_request *request)
{
    uint32_t token;
    if (!client || client->phase != BOARD_OTA_CLIENT_IDLE || !ops ||
        !ops->submit || !ops->poll || !ops->cancel || !ops->discard ||
        !request || !request->image || !request->image_size ||
        request->image_size > P4_BOARD_UPDATE_MAX_IMAGE_SIZE)
        return 0;
    client->ops = *ops;
    client->context = context;
    client->image_size = request->image_size;
    /* Latch before callback: there is no automatic submit retry. */
    client->phase = BOARD_OTA_CLIENT_REFUSED;
    token = client->ops.submit(context, request);
    if (!token) return 0;
    client->token = token;
    client->phase = BOARD_OTA_CLIENT_PENDING;
    return 1;
}

enum board_ota_client_phase board_ota_client_step(struct board_ota_client *c,
    int cancel_requested)
{
    struct p4_wifi_ota_observation reply;
    enum p4_wifi_ota_job_state state;
    if (!c) return BOARD_OTA_CLIENT_IDLE;
    if (c->phase != BOARD_OTA_CLIENT_PENDING &&
        c->phase != BOARD_OTA_CLIENT_DISCARDING) return c->phase;
    if (cancel_requested) c->cancel_requested = 1;
    if (c->phase == BOARD_OTA_CLIENT_DISCARDING) {
        if (c->ops.discard(c->context, c->token) == 1)
            c->phase = BOARD_OTA_CLIENT_FINISHED;
        return c->phase;
    }
    if (c->cancel_requested && !c->cancel_accepted &&
        c->ops.cancel(c->context, c->token) == 1)
        c->cancel_accepted = 1;
    memset(&reply, 0, sizeof(reply));
    state = c->ops.poll(c->context, c->token, &reply);
    if (state == P4_WIFI_OTA_JOB_BUSY) return c->phase;
    if ((state != P4_WIFI_OTA_JOB_QUEUED &&
         state != P4_WIFI_OTA_JOB_RUNNING && state != P4_WIFI_OTA_JOB_DONE) ||
        reply.state != state || reply.token != c->token || !reply.generation ||
        (c->generation && reply.generation != c->generation) ||
        reply.acknowledged_bytes > c->image_size ||
        reply.acknowledged_bytes < c->acknowledged_bytes ||
        reply.cancel_requested > 1 || reply.result_valid > 1 ||
        (c->saw_running && state == P4_WIFI_OTA_JOB_QUEUED) ||
        (state != P4_WIFI_OTA_JOB_DONE && reply.result_valid) ||
        (state == P4_WIFI_OTA_JOB_DONE && (!reply.result_valid ||
         !terminal_shape_valid(&reply.result) ||
         (reply.result.image_size != c->image_size &&
          (reply.result.image_size || reply.result.begin_attempted ||
           reply.result.end_attempted || reply.acknowledged_bytes ||
           (reply.result.status != P4_BOARD_UPDATE_LEASE_FAILED &&
            reply.result.status != P4_BOARD_UPDATE_CANCELLED_BEFORE_BEGIN &&
            reply.result.status != P4_BOARD_UPDATE_INVALID_INPUT))) ||
         reply.result.acknowledged_bytes != reply.acknowledged_bytes))) {
        /* Invalid metadata is never terminal evidence. Try cancellation on
         * subsequent steps and keep the same task/token alive for drainage. */
        c->observation_unconfirmed = 1;
        c->cancel_requested = 1;
        return c->phase;
    }
    c->generation = reply.generation;
    c->acknowledged_bytes = reply.acknowledged_bytes;
    if (reply.cancel_requested) c->cancel_accepted = 1;
    if (state == P4_WIFI_OTA_JOB_RUNNING) c->saw_running = 1;
    if (state == P4_WIFI_OTA_JOB_DONE) {
        c->terminal = reply;
        c->phase = BOARD_OTA_CLIENT_DISCARDING;
        /* A separate step retries discard if the owner is still detaching
         * the image. Never resubmit, recancel or repoll a terminal job. */
    }
    return c->phase;
}
