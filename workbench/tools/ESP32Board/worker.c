/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#include <exec/ports.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>

#include "boardtool.h"

VOID BoardToolWorker(VOID)
{
    struct MsgPort *port = CreateMsgPort();
    struct BoardToolMessage *message;
    BOOL done = FALSE;

    BoardToolWorkerPort = port;
    Signal(BoardToolMainTask, SIGF_SINGLE);   /* startup result is now visible */
    if (port == NULL)
        return;

    while (!done)
    {
        WaitPort(port);
        while ((message = (struct BoardToolMessage *)GetMsg(port)) != NULL)
        {
            if (message->command == BOARDTOOL_CMD_QUIT)
            {
                done = TRUE;
                message->result = 1;
                snprintf(message->status, sizeof(message->status),
                         "Worker stopped");
            }
            else if (message->command == BOARDTOOL_CMD_RUN)
            {
                const struct BoardToolOperation *operation =
                    boardtool_find_operation_id(message->operation);
                if (operation == NULL)
                {
                    message->result = 0;
                    snprintf(message->status, sizeof(message->status),
                             "Unknown operation ID; no write");
                }
                else
                    message->result = boardtool_dispatch(operation->id,
                                                          message);
            }
            else
            {
                message->result = 0;
                snprintf(message->status, sizeof(message->status),
                         "Unknown worker command; no write");
            }

            ReplyMsg(&message->message);
        }
    }

    /* Main waits for this after the quit reply before disposing the port. */
    DeleteMsgPort(port);
    BoardToolWorkerPort = NULL;
    Signal(BoardToolMainTask, SIGF_SINGLE);
}
