/* Copyright (C) 2026, The AROS Development Team. */
#include "bridge.h"
#include "service.h"
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdatomic.h>
#include <string.h>
struct rd_job {
    struct Message message;
    uint32_t ticket,generation;
    unsigned op;
    int status;
    size_t in_size,out_size;
    uint8_t in[RD_REQUEST_MAX],out[RD_DATA_MAX];
};
struct rd_bridge {
    struct rd_link link;
    struct rd_service service;
    struct rd_job job;
    struct MsgPort *reply,*work;
    ULONG child;
    bool busy,clear_needed;
    _Atomic uint32_t generation;
    char screen[128],root[RD_PATH_MAX];
};
static LONG worker(void) {
    struct rd_bridge *b=FindTask(NULL)->tc_UserData;
    struct MsgPort *port=&((struct Process *)FindTask(NULL))->pr_MsgPort;
    b->service.screen=rd_screen_open(b->screen,b->service.mouse);
    if(b->root[0]) b->service.files=rd_files_open(b->root);
    /* Initial reply publishes capability state to the owner. */
    ReplyMsg(&b->job.message);
    for(;;) {
        WaitPort(port);struct rd_job *j=(struct rd_job *)GetMsg(port);
        if(!j)continue;
        if(j->op==0)break;
        j->out_size=0;
        if(j->op==UINT32_MAX) {rd_service_clear(&b->service);j->status=RD_OK;}
        else if(j->generation!=atomic_load_explicit(&b->generation,memory_order_acquire))j->status=RD_SESSION;
        else j->status=rd_service_execute(&b->service,j->op,j->in,j->in_size,j->out,&j->out_size);
        ReplyMsg(&j->message);
    }
    rd_service_clear(&b->service);
    rd_screen_close(b->service.screen);rd_files_close(b->service.files);
    /* STOP message is not replied: parent waits for real child termination,
       not a reply emitted while this code could still be executing. */
    return 0;
}
static bool submit(void *ctx,uint32_t ticket,unsigned op,const uint8_t *p,size_t n) {
    struct rd_bridge *b=ctx;if(b->busy||b->clear_needed||n>sizeof(b->job.in))return false;
    b->job.op=op;b->job.ticket=ticket;b->job.generation=b->link.generation;b->job.in_size=n;
    if(n)memcpy(b->job.in,p,n);
    b->busy=true;PutMsg(b->work,&b->job.message);return true;
}
struct rd_bridge *rd_bridge_open(const char *screen,const char *root,bool mouse) {
    if(!screen||!screen[0]||strlen(screen)>=128||(root&&strlen(root)>=RD_PATH_MAX))return NULL;
    struct rd_bridge *b=AllocVec(sizeof(*b),MEMF_PUBLIC|MEMF_CLEAR);if(!b)return NULL;
    strcpy(b->screen,screen);if(root)strcpy(b->root,root);b->service.mouse=mouse;
    rd_link_init(&b->link,submit,b);atomic_init(&b->generation,b->link.generation);
    b->reply=CreateMsgPort();if(!b->reply){FreeVec(b);return NULL;}
    b->job.message.mn_ReplyPort=b->reply;b->job.message.mn_Length=sizeof(b->job);
    struct TagItem tags[]={ {NP_Entry,(IPTR)worker},{NP_UserData,(IPTR)b},
        {NP_Name,(IPTR)"RemoteDiag worker"},{NP_StackSize,32768},
        {NP_NotifyOnDeath,TRUE},{NP_CopyVars,FALSE},{NP_WindowPtr,(IPTR)-1},
        {TAG_DONE,0} };
    struct Process *p=CreateNewProc(tags);
    if(!p){DeleteMsgPort(b->reply);FreeVec(b);return NULL;}
    b->work=&p->pr_MsgPort;b->child=GetETaskID(&p->pr_Task);
    WaitPort(b->reply);GetMsg(b->reply);return b;
}
void rd_bridge_poll(struct rd_bridge *b) {
    if(b->busy && GetMsg(b->reply)) {
        b->busy=false;
        if(b->job.op!=UINT32_MAX)
            rd_link_complete(&b->link,b->job.ticket,b->job.status,b->job.out,b->job.out_size);
    }
    if(b->clear_needed&&!b->busy) {
        b->clear_needed=false;b->job.op=UINT32_MAX;b->busy=true;
        PutMsg(b->work,&b->job.message);
    }
}
void rd_bridge_reset(struct rd_bridge *b) {
    rd_link_reset(&b->link);atomic_store_explicit(&b->generation,b->link.generation,memory_order_release);
    b->clear_needed=true; /* Worker owns snapshots; reset callback never frees. */
}
void rd_bridge_close(struct rd_bridge *b) {
    if(!b)return;
    atomic_store_explicit(&b->generation,0,memory_order_release);
    if(b->busy){WaitPort(b->reply);GetMsg(b->reply);}
    b->job.op=0;PutMsg(b->work,&b->job.message);
    /* DOS calls are not forcibly cancellable. Keep code/storage alive until
       actual process death; a failed handler can delay shutdown indefinitely.
       This prototype must use a trusted local filesystem, not a hung handler. */
    ChildWait(b->child);ChildFree(b->child);
    DeleteMsgPort(b->reply);FreeVec(b);
}
size_t rd_bridge_room(struct rd_bridge *b) {
    if(b->busy||b->clear_needed||b->link.pending||b->link.tx_size||b->link.poisoned)return 0;
    return b->link.rx_need-b->link.rx_used;
}
size_t rd_bridge_feed(struct rd_bridge *b,const uint8_t *p,size_t n) {
    uint32_t generation=b->link.generation;
    size_t used=rd_link_feed(&b->link,p,n);
    if(generation!=b->link.generation) {
        atomic_store_explicit(&b->generation,b->link.generation,memory_order_release);
        b->clear_needed=true;
    }
    return used;
}
size_t rd_bridge_peek(struct rd_bridge *b,const uint8_t **p){return rd_link_peek(&b->link,p);}
bool rd_bridge_consume(struct rd_bridge *b,size_t n){return rd_link_consume(&b->link,n);}
