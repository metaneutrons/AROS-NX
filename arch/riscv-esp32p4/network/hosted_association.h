#ifndef P4_HOSTED_ASSOCIATION_H
#define P4_HOSTED_ASSOCIATION_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

#define P4_HOSTED_ASSOCIATION_MAX_REQUEST_SIZE 256u
#define P4_HOSTED_ASSOCIATION_MAX_BODY_SIZE 256u
#define P4_HOSTED_ASSOCIATION_SSID_SIZE 32u
#define P4_HOSTED_ASSOCIATION_PASSWORD_SIZE 64u
#define P4_HOSTED_ASSOCIATION_BSSID_SIZE 6u
#define P4_HOSTED_ASSOCIATION_SAE_ID_SIZE 32u

/* Wire-shaped STA settings from pinned wifi_sta_config. Byte fields are
 * borrowed only for the duration of the request encoder. No C string
 * terminator is added or assumed. The encoder accepts the full schema lengths
 * (including 32/64-byte SSID/password); see the slave strlen/privacy warning
 * below before using those lengths with an installed reference image. */
struct p4_hosted_sta_config {
    const uint8_t *ssid;
    size_t ssid_length;
    const uint8_t *password;
    size_t password_length;
    int32_t scan_method;
    unsigned bssid_set;
    const uint8_t *bssid;
    size_t bssid_length;
    uint32_t channel;
    uint32_t listen_interval;
    int32_t sort_method;
    int32_t threshold_rssi;
    int32_t threshold_authmode;
    unsigned pmf_capable;
    unsigned pmf_required;
    uint32_t bitmask;
    int32_t sae_pwe_h2e;
    uint32_t failure_retry_cnt;
    uint32_t he_bitmask;
    const uint8_t *sae_h2e_identifier;
    size_t sae_h2e_identifier_length;
};

struct p4_hosted_sta_connected_event {
    int32_t status;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint32_t ssid_length;
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint32_t channel;
    int32_t authmode;
    int32_t aid;
};

struct p4_hosted_sta_disconnected_event {
    int32_t status;
    uint8_t ssid[P4_HOSTED_ASSOCIATION_SSID_SIZE];
    uint32_t ssid_length;
    uint8_t bssid[P4_HOSTED_ASSOCIATION_BSSID_SIZE];
    uint32_t reason;
    int32_t rssi;
};

struct p4_hosted_wifi_event {
    int32_t status;
    int32_t event_id;
};

/* IDs pinned from the local ESP-IDF 5.4.3 wifi_event_t enum. Compatibility
 * with the installed C6 peer is unqualified; these values imply no lifecycle
 * barrier. */
enum p4_hosted_wifi_event_id {
    P4_HOSTED_WIFI_EVENT_STA_START = 2,
    P4_HOSTED_WIFI_EVENT_STA_STOP = 3,
    P4_HOSTED_WIFI_EVENT_STA_AUTHMODE_CHANGE = 6
};

/* Encode WifiSetConfig(STA), request ID 284. All schema fields are supported;
 * proto3 scalar defaults are omitted, while threshold field 9 and PMF field 10
 * are always emitted (the pinned slave dereferences both messages). Byte
 * fields are copied exactly; no truncation occurs. The complete envelope is
 * capped at 256 bytes. The output, written, config, and each nonempty input
 * span must be pairwise disjoint where either side is writable. Refusal leaves
 * output and *written unchanged. Temporary credential copies are volatile-
 * wiped on every post-validation path; successful output contains credentials
 * by design and is the caller's responsibility to protect and wipe.
 *
 * Source caveat: the pinned slave logs SSID/password and passes the fixed
 * arrays to strlen; installed log level and safety for full-width non-NUL
 * values are not established. This codec does not log credentials and does
 * not claim the slave is privacy-safe. */
int p4_hosted_association_set_config_request(
    uint32_t uid, const struct p4_hosted_sta_config *config,
    uint8_t *bytes, size_t capacity, size_t *written);

/* Encode empty-body WifiConnect (282) and WifiDisconnect (283) requests. */
int p4_hosted_association_connect_request(uint32_t uid, uint8_t *bytes,
                                         size_t capacity, size_t *written);
int p4_hosted_association_disconnect_request(uint32_t uid, uint8_t *bytes,
                                            size_t capacity, size_t *written);

/* Decode a correlated response for request ID 282, 283, or 284. The response
 * ID must be request ID + 256 and UID must match exactly and be nonzero. The
 * signed int32 status defaults to zero when omitted. A decoded negative status
 * is a remote operation failure, not a codec refusal. Refusal leaves status
 * unchanged. */
int p4_hosted_association_status_response(const struct p4_hosted_rpc *rpc,
                                          uint32_t expected_request_id,
                                          uint32_t expected_uid,
                                          int32_t *status);

/* Decode Event 775 with UID zero. Successful events require a 32-byte SSID
 * array, meaningful ssid_length <= 32, and six-byte BSSID. Output owns all
 * bytes; it borrows no RPC/reassembly storage. A nonzero wrapper status is
 * accepted without nested data. If nested event data is present, it is fully
 * validated but discarded; the status is retained and data fields are zero. */
int p4_hosted_association_connected_event(
    const struct p4_hosted_rpc *rpc,
    struct p4_hosted_sta_connected_event *result);

/* Decode Event 776 under the same envelope and ownership rules as Event 775. */
int p4_hosted_association_disconnected_event(
    const struct p4_hosted_rpc *rpc,
    struct p4_hosted_sta_disconnected_event *result);

/* Decode EVENT 773 (Rpc_Event_WifiEventNoArgs), UID zero. The pinned schema
 * has direct int32 resp=1 and event_id=2 fields, not a nested event message.
 * Proto3 defaults are accepted, including an empty body; signed and unknown
 * event IDs are preserved. A remote error retains status and clears event_id.
 * Decoding does not establish a station lifecycle barrier. */
int p4_hosted_association_wifi_event(
    const struct p4_hosted_rpc *rpc, struct p4_hosted_wifi_event *result);

#endif
