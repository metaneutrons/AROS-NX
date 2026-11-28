/* Copyright (C) 2026, The AROS Development Team. */
#include "cdc_log.h"
#include <string.h>
bool p4_cdc_log_init(struct p4_cdc_log *log,const struct p4_cdc_source *source,const struct p4_cdc_sink *sink)
{
    if(!log||!source||!sink||!source->pop||!source->dropped||
        !sink->connected||!sink->room||!sink->write||!sink->flush)return false;
    memset(log,0,sizeof(*log));log->source=*source;log->sink=*sink;return true;
}
void p4_cdc_log_reset(struct p4_cdc_log *log)
{
    /* A transport reset loses TinyUSB's own FIFO. Never replay an uncertain
       partial record into a new connection and pretend byte continuity. */
    memset(log->pending,0,sizeof(log->pending));log->length=log->position=0;
    log->reported_drops=0;log->warning_last=false;
    static const char notice[]="[p4cdc stream reset; continuity not guaranteed]\n";
    memcpy(log->pending,notice,sizeof(notice)-1);log->length=sizeof(notice)-1;
}
static void warning(struct p4_cdc_log *log,uint32_t drops)
{
    static const char prefix[]="[p4cdc overflow records=";
    static const char digits[]="0123456789abcdef";
    size_t pos=sizeof(prefix)-1;memcpy(log->pending,prefix,pos);
    for(unsigned i=0;i<8;++i)log->pending[pos++]=(uint8_t)digits[(drops>>(28-4*i))&15];
    log->pending[pos++]=']';log->pending[pos++]='\n';log->length=pos;
    log->reported_drops=drops;log->warning_last=true;
}
bool p4_cdc_log_step(struct p4_cdc_log *log,size_t budget)
{
    if(budget>256)budget=256;
    if(!log->sink.connected(log->sink.context))return true;
    size_t sent=0;
    while(sent<budget) {
        if(log->position==log->length) {
            log->position=log->length=0;
            uint32_t drops=log->source.dropped(log->source.context);
            if(drops!=log->reported_drops && !log->warning_last)warning(log,drops);
            else {
                struct p4_console_record record;
                int result=log->source.pop(log->source.context,&record);
                if(result<0)return false;
                if(!result){log->warning_last=false;break;}
                if(record.length==0||record.length>P4_CONSOLE_RECORD_BYTES)return false;
                memcpy(log->pending,record.data,record.length);log->length=record.length;
                log->warning_last=false;
            }
        }
        size_t room=log->sink.room(log->sink.context),count=log->length-log->position;
        if(count>room)count=room;
        if(count>budget-sent)count=budget-sent;
        if(!count)break;
        size_t written=log->sink.write(log->sink.context,log->pending+log->position,count);
        if(written>count)return false;
        if(!written)break;
        log->position+=written;sent+=written;
    }
    if(sent)log->sink.flush(log->sink.context);
    return true;
}
