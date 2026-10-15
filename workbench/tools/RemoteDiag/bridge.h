/* Transport-neutral asynchronous DOS worker bridge. */
#ifndef RD_BRIDGE_H
#define RD_BRIDGE_H
#include "protocol.h"
struct rd_bridge;
struct rd_bridge *rd_bridge_open(const char *screen,const char *root,bool mouse);
void rd_bridge_close(struct rd_bridge *);
void rd_bridge_reset(struct rd_bridge *);
void rd_bridge_poll(struct rd_bridge *);
size_t rd_bridge_room(struct rd_bridge *);
size_t rd_bridge_feed(struct rd_bridge *,const uint8_t *,size_t);
size_t rd_bridge_peek(struct rd_bridge *,const uint8_t **);
bool rd_bridge_consume(struct rd_bridge *,size_t);
#endif
