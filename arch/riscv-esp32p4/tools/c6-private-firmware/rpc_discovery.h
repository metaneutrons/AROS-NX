/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef C6_RPC_DISCOVERY_H
#define C6_RPC_DISCOVERY_H
#include "esp_hosted_rpc.pb-c.h"
#include "esp_err.h"
/* Called only by the serialized Hosted command worker. Exclusion contract
 * in discovery.h applies. The response is owned until pack/cleanup. */
esp_err_t c6_rpc_board_discovery(Rpc *request, Rpc *response, void *private_data);
void c6_rpc_board_discovery_cleanup(Rpc *response);
#endif
