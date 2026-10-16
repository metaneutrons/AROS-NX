/* Development-only bounded USB detach/attach stimulus; no MMIO in this helper. */
#ifndef HS_LINK_RECONNECT_H
#define HS_LINK_RECONNECT_H
#include <stdbool.h>
#include <stddef.h>

struct hs_reconnect_ops {
    void *ctx;
    bool (*disconnect)(void *);
    bool (*connect)(void *);
    void (*wait_ms)(void *,unsigned);
    void (*note)(void *,bool,bool);
};

/* Single observer-task owner. Attempt reconnect even if detach reports false,
 * and record both outcomes. Never repeat a failed/uncertain stimulus. */
static inline bool hs_reconnect_once(bool *attempted,const struct hs_reconnect_ops *ops)
{
    if(!attempted || !ops || !ops->disconnect || !ops->connect ||
       !ops->wait_ms || !ops->note || *attempted)return false;
    *attempted=true;
    bool ok=ops->disconnect(ops->ctx);
    ops->note(ops->ctx,false,ok);
    ops->wait_ms(ops->ctx,1000);
    ok=ops->connect(ops->ctx);
    ops->note(ops->ctx,true,ok);
    return true;
}
#endif
