/* Copyright (C) 2026, The AROS Development Team. */
#ifndef RD_PROTOCOL_H
#define RD_PROTOCOL_H
#include "backend.h"
#define RD_HEADER 32u
#define RD_REQUEST_MAX 1024u
#define RD_RESPONSE_MAX (RD_DATA_MAX+RD_HEADER)
enum rd_op { RD_HELLO=1,RD_STATUS=2,RD_CAPTURE=3,RD_CHUNK=4,RD_RELEASE=5,
    RD_MOVE=6,RD_CLICK=7,RD_STAT=8,RD_LIST=9,RD_READ=10 };
uint32_t rd_u32(const uint8_t *);
void rd_put32(uint8_t *,uint32_t);
uint64_t rd_u64(const uint8_t *);
void rd_put64(uint8_t *,uint64_t);
uint32_t rd_crc(const uint8_t *,size_t);
/* One outstanding operation. Request body must be copied by submit. Completion
   may arrive after reset: unique ticket binds it to the original operation. */
typedef bool (*rd_submit_fn)(void *,uint32_t ticket,unsigned op,const uint8_t *,size_t);
struct rd_link {
    uint8_t rx[RD_HEADER+RD_REQUEST_MAX], tx[RD_RESPONSE_MAX];
    size_t rx_used,rx_need,tx_size,tx_sent;
    uint64_t session;
    uint32_t generation,sequence,ticket,serial,request_id;
    unsigned operation;
    bool pending,poisoned;
    rd_submit_fn submit;
    void *context;
};
void rd_link_init(struct rd_link *,rd_submit_fn,void *);
void rd_link_reset(struct rd_link *);
size_t rd_link_feed(struct rd_link *,const uint8_t *,size_t);
size_t rd_link_peek(struct rd_link *,const uint8_t **);
bool rd_link_consume(struct rd_link *,size_t);
void rd_link_complete(struct rd_link *,uint32_t ticket,int status,const uint8_t *,size_t);
#endif
