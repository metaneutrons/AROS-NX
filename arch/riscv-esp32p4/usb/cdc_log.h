/* Copyright (C) 2026, The AROS Development Team. */
#ifndef P4_CDC_LOG_H
#define P4_CDC_LOG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../kernel/console_ring.h"
struct p4_cdc_source {
    void *context;
    int (*pop)(void *,struct p4_console_record *);
    uint32_t (*dropped)(void *);
};
struct p4_cdc_sink {
    void *context;
    bool (*connected)(void *);
    size_t (*room)(void *);
    size_t (*write)(void *,const uint8_t *,size_t);
    void (*flush)(void *);
};
struct p4_cdc_log {
    struct p4_cdc_source source;
    struct p4_cdc_sink sink;
    uint8_t pending[160];
    size_t length,position;
    uint32_t reported_drops;
    bool warning_last;
};
bool p4_cdc_log_init(struct p4_cdc_log *,const struct p4_cdc_source *,const struct p4_cdc_sink *);
/* Only USB owner calls this; never from kernel producers or a hardware IRQ.
   Return false for invalid native API results, rather than lose a suffix. */
bool p4_cdc_log_step(struct p4_cdc_log *,size_t budget);
void p4_cdc_log_reset(struct p4_cdc_log *);
#endif
