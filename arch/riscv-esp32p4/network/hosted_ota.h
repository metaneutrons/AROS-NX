#ifndef P4_HOSTED_OTA_H
#define P4_HOSTED_OTA_H

/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */

#include "hosted_rpc.h"

/* Local policy bounds the complete Rpc envelope, including OTAWrite data. */
#define P4_HOSTED_OTA_MAX_REQUEST_SIZE 256u

/* With request UID UINT32_MAX, this is the largest OTAWrite field-1 byte
 * string whose nested body and full Rpc envelope fit in 256 bytes. */
#define P4_HOSTED_OTA_MAX_WRITE_SIZE 238u
#define P4_HOSTED_OTA_MAX_RESPONSE_BODY_SIZE 256u

/* Encode schema-defined empty Rpc_Req_OTABegin (ID 272). */
int p4_hosted_ota_begin_request(uint32_t uid, uint8_t *bytes,
                                size_t capacity, size_t *written);

/* Encode Rpc_Req_OTAWrite (ID 273), with 1..238 input bytes in field 1.
 * The complete Rpc envelope is at most 256 bytes even when uid is UINT32_MAX.
 * Refusal leaves bytes and *written unchanged and never modifies data. */
int p4_hosted_ota_write_request(uint32_t uid, const uint8_t *data,
                                size_t data_size, uint8_t *bytes,
                                size_t capacity, size_t *written);

/* Encode schema-defined empty Rpc_Req_OTAEnd (ID 274). */
int p4_hosted_ota_end_request(uint32_t uid, uint8_t *bytes,
                              size_t capacity, size_t *written);

/* Decode Rpc_Resp_OTABegin / OTAWrite / OTAEnd status. The expected request
 * ID must be 272, 273, or 274; response type, ID, and nonzero UID must match.
 * Proto3's omitted int32 resp field defaults to zero. Duplicate or malformed
 * status fields and oversized bodies are rejected. Refusal leaves *status
 * unchanged; status must not overlap rpc or its nested response bytes. */
int p4_hosted_ota_status_response(const struct p4_hosted_rpc *rpc,
                                  uint32_t expected_request_id,
                                  uint32_t expected_uid, int32_t *status);

#endif
