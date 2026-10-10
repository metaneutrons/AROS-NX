/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef ESP32BOARD_BOARDTOOL_H
#define ESP32BOARD_BOARDTOOL_H

#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/types.h>
#include <stddef.h>
#include <string.h>

#include "layout.h"
#include "../../../arch/riscv-esp32p4/boardtool/discovery_client.h"

#define BOARDTOOL_PATH_MAX   512
#define BOARDTOOL_SHA256_LEN 64
#define BOARDTOOL_STATUS_MAX 384

enum BoardToolOperationId
{
    BOARDTOOL_OP_INFO = 1,
    BOARDTOOL_OP_CHECK_C6,
    BOARDTOOL_OP_LAYOUT,
    BOARDTOOL_OP_UPDATE_C6,
    BOARDTOOL_OP_DISCOVER_C6
};

struct BoardToolOperation
{
    ULONG id;
    CONST_STRPTR name;
    CONST_STRPTR label;
    BOOL available;
};

/* The command-line and MUI front ends resolve the same operation table. */
static const struct BoardToolOperation boardtool_operations[] =
{
    { BOARDTOOL_OP_INFO,      "INFO",      "Updater prototype information", TRUE  },
    { BOARDTOOL_OP_CHECK_C6,  "CHECK-C6",  "Check C6 image",                 TRUE  },
    { BOARDTOOL_OP_LAYOUT,    "LAYOUT",    "Preview imported flash table",   TRUE  },
    { BOARDTOOL_OP_UPDATE_C6, "UPDATE-C6", "Update C6 (unavailable)",       FALSE },
    { BOARDTOOL_OP_DISCOVER_C6, "DISCOVER-C6", "Query C6 (WLAN goes offline)", TRUE }
};

#define BOARDTOOL_OPERATION_COUNT \
    (sizeof(boardtool_operations) / sizeof(boardtool_operations[0]))

static inline const struct BoardToolOperation *
boardtool_find_operation_name(CONST_STRPTR name)
{
    size_t i;

    if (name == NULL)
        return NULL;
    for (i = 0; i < BOARDTOOL_OPERATION_COUNT; i++)
        if (strcmp((const char *)name,
                   (const char *)boardtool_operations[i].name) == 0)
            return &boardtool_operations[i];
    return NULL;
}

static inline const struct BoardToolOperation *
boardtool_find_operation_id(ULONG id)
{
    size_t i;

    for (i = 0; i < BOARDTOOL_OPERATION_COUNT; i++)
        if (boardtool_operations[i].id == id)
            return &boardtool_operations[i];
    return NULL;
}

enum BoardToolCommand
{
    BOARDTOOL_CMD_RUN = 1,
    BOARDTOOL_CMD_QUIT
};

/* One in-flight message is shared by the UI and worker. It is not re-used
 * until ReplyMsg has returned ownership to the main task. */
struct BoardToolMessage
{
    struct Message message;
    ULONG command;
    ULONG operation;
    LONG result;
    char image[BOARDTOOL_PATH_MAX];
    char sha256[BOARDTOOL_SHA256_LEN + 1];
    ULONG flash_mb;
    struct board_layout layout;
    struct board_client_observation observation;
    char status[BOARDTOOL_STATUS_MAX];
};

extern struct MsgPort *BoardToolWorkerPort;
extern struct Task *BoardToolMainTask;

VOID BoardToolWorker(VOID);
LONG boardtool_dispatch(ULONG operation, struct BoardToolMessage *message);

/* Bounded C6 application-file format and trusted-digest preflight only.
 * A pass does not establish compatibility, installed layout or recovery. */
int boardtool_check_c6(const char *path, const char *sha256,
                       char *status, size_t capacity);
int boardtool_discover_c6(struct board_client_observation *, char *, size_t);

#endif
