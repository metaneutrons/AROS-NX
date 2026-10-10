#ifndef P4_HOSTED_SCAN_H
#define P4_HOSTED_SCAN_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

#define P4_HOSTED_SCAN_MAX_RECORDS 8u
#define P4_HOSTED_SCAN_MAX_BODY_SIZE 4096u
#define P4_HOSTED_SCAN_BSSID_SIZE 6u
#define P4_HOSTED_SCAN_SSID_MAX_SIZE 32u
#define P4_HOSTED_SCAN_COUNTRY_CC_MAX_SIZE 3u

/* Owned, typed representation of the pinned wifi_ap_record schema. Byte
 * strings carry explicit lengths and are not C strings. Optional country and
 * HE submessages retain their proto3 zero defaults. */
struct p4_hosted_scan_record {
    uint8_t bssid[P4_HOSTED_SCAN_BSSID_SIZE];
    size_t ssid_size;
    uint8_t ssid[P4_HOSTED_SCAN_SSID_MAX_SIZE];
    uint32_t primary;
    int32_t second;
    int32_t rssi;
    int32_t authmode;
    int32_t pairwise_cipher;
    int32_t group_cipher;
    int32_t ant;
    uint32_t bitmask;
    size_t country_cc_size;
    uint8_t country_cc[P4_HOSTED_SCAN_COUNTRY_CC_MAX_SIZE];
    uint32_t country_schan;
    uint32_t country_nchan;
    int32_t country_max_tx_power;
    int32_t country_policy;
    uint32_t he_bitmask;
    uint32_t he_bssid_index;
};

struct p4_hosted_scan_count {
    int32_t status;
    uint16_t number;
};

struct p4_hosted_scan_records {
    int32_t status;
    uint16_t number;
    size_t record_count;
    struct p4_hosted_scan_record records[P4_HOSTED_SCAN_MAX_RECORDS];
};

/* Encode ScanStart for the NULL/default config and block in {0,1}. The false
 * default is omitted; config_set=0 is explicitly present. uid must be
 * nonzero. Refusal leaves bytes and *written unchanged. */
int p4_hosted_scan_start_request(uint32_t uid, unsigned block,
                                 uint8_t *bytes, size_t capacity,
                                 size_t *written);

/* Encode the empty ScanStop (287) and ScanGetApNum (288) request bodies. */
int p4_hosted_scan_stop_request(uint32_t uid, uint8_t *bytes,
                                size_t capacity, size_t *written);
int p4_hosted_scan_num_request(uint32_t uid, uint8_t *bytes,
                               size_t capacity, size_t *written);

/* Encode ScanGetApRecords with an explicit bounded count in [1, 8]. */
int p4_hosted_scan_records_request(uint32_t uid, uint16_t number,
                                   uint8_t *bytes, size_t capacity,
                                   size_t *written);

/* Decode ScanStart or ScanStop status only. The response type, ID, and
 * nonzero expected UID must match. An omitted proto3 status means zero. */
int p4_hosted_scan_status_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_request_id,
                                   uint32_t expected_uid, int32_t *status);

/* Decode ScanGetApNum response status and nonnegative uint16 count. */
int p4_hosted_scan_num_response(const struct p4_hosted_rpc *rpc,
                                uint32_t expected_uid,
                                struct p4_hosted_scan_count *result);

/* Decode ScanGetApRecords. requested_capacity must be at most eight; the
 * decoded number must equal the count of repeated AP records and fit that
 * capacity. A zero-record result is syntactically valid; callers must compare
 * against any earlier positive count and apply their scan-completion/nonempty
 * policy. */
int p4_hosted_scan_records_response(const struct p4_hosted_rpc *rpc,
                                   uint32_t expected_uid,
                                   uint16_t requested_capacity,
                                   struct p4_hosted_scan_records *result);

#endif
