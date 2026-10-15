#ifndef P4_HOSTED_EVENTS_H
#define P4_HOSTED_EVENTS_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

#define P4_HOSTED_AUX_EVENT_MAX_PAYLOAD 256u
#define P4_HOSTED_AUX_EVENT_INIT_DATA_SIZE 256u

enum p4_hosted_aux_event_id {
    P4_HOSTED_AUX_EVENT_ESP_INIT = 769,
    P4_HOSTED_AUX_EVENT_HEARTBEAT = 770,
    P4_HOSTED_AUX_EVENT_AP_STA_CONNECTED = 771,
    P4_HOSTED_AUX_EVENT_AP_STA_DISCONNECTED = 772,
    P4_HOSTED_AUX_EVENT_STA_SCAN_DONE = 774
};

union p4_hosted_aux_event_data {
    struct {
        size_t size;
        uint8_t data[P4_HOSTED_AUX_EVENT_INIT_DATA_SIZE];
    } init;
    struct {
        int32_t value;
    } heartbeat;
    struct {
        uint8_t mac[6];
        uint32_t aid;
        unsigned is_mesh_child;
        uint32_t reason;
    } ap;
    struct {
        uint32_t status;
        uint32_t number;
        uint32_t scan_id;
    } scan;
};

struct p4_hosted_aux_event {
    uint32_t msg_id;
    int32_t status;
    union p4_hosted_aux_event_data data;
};

/* Decode one of ESPInit(769), Heartbeat(770), AP station connected (771),
 * AP station disconnected (772), or station scan done (774) from an already
 * decoded RPC envelope. The envelope must be EVENT with UID zero and the exact
 * supported message ID. The protobuf event payload is limited to 256 bytes.
 * Bytes returned by ESPInit are copied into the result; no input storage is
 * borrowed.
 *
 * Proto3 defaults are accepted. Duplicate compatible scalar and bytes fields
 * use the last value. Repeated scan_done submessages merge as protobuf does.
 * Unknown fields with wire types 0, 1, 2, and 5 are skipped; malformed fields,
 * known fields with the wrong wire type, groups, and integer overflow are
 * rejected. Successful AP station events require a six-byte MAC. Successful
 * scan-done events require the nested scan_done message. A nonzero wrapper
 * status is preserved after any present event data is validated, and clears
 * the result union. Heartbeats are decoded data only; they do not establish
 * peer liveness.
 *
 * The rpc object, its payload, and result must be valid non-overlapping spans.
 * Refusal leaves result unchanged. This decoder uses fixed storage and no
 * allocation. */
int p4_hosted_aux_event_decode(const struct p4_hosted_rpc *rpc,
                               struct p4_hosted_aux_event *result);

#endif
