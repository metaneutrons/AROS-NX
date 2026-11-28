/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include "rpc_discovery.h"
#include "discovery.h"
#include "../../boardtool/discovery_wire.h"
#include <stdlib.h>

esp_err_t c6_rpc_board_discovery(Rpc *request, Rpc *response, void *private_data)
{
    RpcRespBoardDiscovery *reply;
    struct board_discovery_snapshot *snapshot;
    uint8_t *bytes;
    size_t written = 0;
    (void)private_data;
    if (!request || !response || request->msg_type != RPC_TYPE__Req ||
        request->msg_id != RPC_ID__Req_BoardDiscovery || !request->uid ||
        request->payload_case != RPC__PAYLOAD_REQ_BOARD_DISCOVERY ||
        !request->req_board_discovery ||
        response->msg_type != RPC_TYPE__Resp ||
        response->msg_id != RPC_ID__Resp_BoardDiscovery ||
        response->payload_case != RPC__PAYLOAD_RESP_BOARD_DISCOVERY ||
        response->uid != request->uid || response->resp_board_discovery)
        return ESP_ERR_INVALID_ARG;
    reply = calloc(1, sizeof(*reply));
    if (!reply) return ESP_ERR_NO_MEM;
    rpc__resp__board_discovery__init(reply);
    response->resp_board_discovery = reply;
    reply->resp = ESP_ERR_INVALID_ARG;
    if (request->req_board_discovery->version != BOARD_DISCOVERY_WIRE_VERSION ||
        !request->req_board_discovery->nonce) return ESP_OK;
    snapshot = calloc(1, sizeof(*snapshot));
    bytes = malloc(BOARD_DISCOVERY_WIRE_MAX);
    if (!snapshot || !bytes) {
        free(snapshot); free(bytes);
        reply->resp = ESP_ERR_NO_MEM;
        return ESP_OK;
    }
    reply->resp = ESP_FAIL;
    if (!c6_board_discovery_collect(snapshot) ||
        !board_discovery_encode(snapshot, request->req_board_discovery->nonce,
            bytes, BOARD_DISCOVERY_WIRE_MAX, &written)) {
        free(snapshot); free(bytes);
        return ESP_OK;
    }
    free(snapshot);
    reply->snapshot.data = bytes;
    reply->snapshot.len = written;
    reply->resp = ESP_OK;
    return ESP_OK;
}

void c6_rpc_board_discovery_cleanup(Rpc *response)
{
    if (!response || response->msg_id != RPC_ID__Resp_BoardDiscovery ||
        response->payload_case != RPC__PAYLOAD_RESP_BOARD_DISCOVERY ||
        !response->resp_board_discovery) return;
    free(response->resp_board_discovery->snapshot.data);
    free(response->resp_board_discovery);
    response->resp_board_discovery = NULL;
}
