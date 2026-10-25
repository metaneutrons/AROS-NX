/* Copyright (C) 2026, The AROS Development Team. */
#ifndef RD_SERVICE_H
#define RD_SERVICE_H
#include "protocol.h"
struct rd_service {
    struct rd_screen *screen;
    struct rd_files *files;
    struct rd_snapshot snapshot;
    uint32_t snapshot_id;
    bool mouse;
};
void rd_service_clear(struct rd_service *);
int rd_service_execute(struct rd_service *,unsigned,const uint8_t *,size_t,uint8_t *,size_t *);
#endif
