#ifndef P4_HOSTED_WIFI_H
#define P4_HOSTED_WIFI_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

/* Defensive bound for one nested request/response protobuf body. */
#define P4_HOSTED_WIFI_MAX_BODY_SIZE 256u

/* Field names and types follow wifi_init_config in esp_hosted_rpc.proto.
 * Runtime owners supply and validate values; this codec supplies no defaults. */
struct p4_hosted_wifi_config {
    int32_t static_rx_buf_num;
    int32_t dynamic_rx_buf_num;
    int32_t tx_buf_type;
    int32_t static_tx_buf_num;
    int32_t dynamic_tx_buf_num;
    int32_t cache_tx_buf_num;
    int32_t csi_enable;
    int32_t ampdu_rx_enable;
    int32_t ampdu_tx_enable;
    int32_t amsdu_tx_enable;
    int32_t nvs_enable;
    int32_t nano_enable;
    int32_t rx_ba_win;
    int32_t wifi_task_core_id;
    int32_t beacon_max_len;
    int32_t mgmt_sbuf_num;
    uint64_t feature_caps;
    unsigned sta_disconnected_pm;
    int32_t espnow_max_encrypt_num;
    int32_t magic;
};

/* Encode a WifiInit request (ID 278), explicitly serializing all 20 config
 * fields, including zero values. Signed int32 values use protobuf's canonical
 * sign-extended varint representation. uid must be nonzero. Refusal leaves
 * bytes and *written unchanged; caller-owned config is never modified. */
int p4_hosted_wifi_init_request(uint32_t uid,
                                const struct p4_hosted_wifi_config *config,
                                uint8_t *bytes, size_t capacity,
                                size_t *written);

/* Encode only the empty WifiDeinit (279), WifiStart (280), or WifiStop (281)
 * request body. uid must be nonzero. */
int p4_hosted_wifi_simple_request(uint32_t request_id, uint32_t uid,
                                  uint8_t *bytes, size_t capacity,
                                  size_t *written);

/* Encode SetMode (260) for STA mode (mode value 1) only. uid must be nonzero. */
int p4_hosted_wifi_set_mode_request(uint32_t uid, int32_t mode,
                                    uint8_t *bytes, size_t capacity,
                                    size_t *written);

/* Decode the status field in the response to one supported request ID.
 * expected_uid must be nonzero; response type, corresponding response ID and
 * UID must match exactly. An omitted proto3 status is zero. Unknown bounded
 * fields are skipped; duplicate status, malformed values, groups, and bodies
 * over P4_HOSTED_WIFI_MAX_BODY_SIZE are rejected. Refusal leaves *status
 * unchanged; status must not overlap rpc or its nested payload. */
int p4_hosted_wifi_status_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_request_id,
                                   uint32_t expected_uid, int32_t *status);

#endif
