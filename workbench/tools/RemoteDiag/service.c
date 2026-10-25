/* Copyright (C) 2026, The AROS Development Team. */
#include "service.h"
#include <string.h>
#ifdef RD_SERVICE_TEST
#include <stdlib.h>
#define FreeVec free
#else
#include <proto/exec.h>
#endif
void rd_service_clear(struct rd_service *s) {
    if(s->snapshot.data) FreeVec(s->snapshot.data);
    memset(&s->snapshot,0,sizeof(s->snapshot));
}
static const char *path_at(const uint8_t *p,size_t n,size_t offset) {
    if(n<=offset || n-offset>RD_PATH_MAX || p[n-1]!=0 ||
        memchr(p+offset,0,n-offset-1))return NULL;
    const char *path=(const char *)p+offset;return rd_valid_relative(path)?path:NULL;
}
static void metadata(uint8_t *out,const struct rd_metadata *m) {
    rd_put32(out,m->protection);rd_put32(out+4,m->size);rd_put32(out+8,(uint32_t)m->type);
    rd_put32(out+12,(uint32_t)m->days);rd_put32(out+16,(uint32_t)m->minutes);
    rd_put32(out+20,(uint32_t)m->ticks);memcpy(out+24,m->name,108);memcpy(out+132,m->comment,80);
}
int rd_service_execute(struct rd_service *s,unsigned op,const uint8_t *p,size_t n,uint8_t *out,size_t *length) {
    *length=0;if((n&&!p)||!out)return RD_BAD_REQUEST;
    if(op==RD_STATUS) {
        if(n)return RD_BAD_REQUEST;
        rd_put32(out,(s->screen?1u:0u)|(s->screen&&s->mouse&&rd_screen_has_mouse(s->screen)?2u:0u)|(s->files?4u:0u));
        rd_put32(out+4,s->snapshot.data?s->snapshot_id:0);*length=8;return RD_OK;
    }
    if(op==RD_CHUNK) {
        if(n!=12)return RD_BAD_REQUEST;
        uint32_t id=rd_u32(p),offset=rd_u32(p+4),count=rd_u32(p+8);
        if(!s->snapshot.data || id!=s->snapshot_id)return RD_CHANGED;
        if(!count||count>RD_DATA_MAX||offset>s->snapshot.size||count>s->snapshot.size-offset)return RD_LIMIT;
        memcpy(out,s->snapshot.data+offset,count);*length=count;return RD_OK;
    }
    if(op==RD_RELEASE) {
        if(n!=4)return RD_BAD_REQUEST;
        if(!s->snapshot.data||rd_u32(p)!=s->snapshot_id)return RD_CHANGED;
        rd_service_clear(s);return RD_OK;
    }
    if(op==RD_MOVE || op==RD_CLICK) {
        if(n!=(op==RD_MOVE?8u:12u))return RD_BAD_REQUEST;
        if(!s->screen||!s->mouse)return RD_DENIED;
        unsigned button=op==RD_MOVE?0:rd_u32(p+8);
        if(op==RD_CLICK && (button<1 || button>3))return RD_BAD_REQUEST;
        return rd_screen_mouse(s->screen,rd_u32(p),rd_u32(p+4),button);
    }
    if(op==RD_STAT || op==RD_LIST) {
        const char *path=path_at(p,n,op==RD_STAT?0:4);
        if(!path)return RD_BAD_REQUEST;
        if(!s->files)return RD_DENIED;
        struct rd_metadata m={0};
        int status=op==RD_STAT?rd_files_stat(s->files,path,&m):rd_files_list(s->files,path,rd_u32(p),&m);
        if(status!=RD_OK)return status;
        /* Always bounded C strings on the wire, no uninitialized padding. */
        m.name[107]=0;m.comment[79]=0;metadata(out,&m);*length=212;return RD_OK;
    }
    if(op==RD_CAPTURE || op==RD_READ) {
        struct rd_snapshot fresh={0};int status;
        if(op==RD_CAPTURE) {
            if(n!=16)return RD_BAD_REQUEST;
            if(!s->screen)return RD_DENIED;
            status=rd_screen_capture(s->screen,rd_u32(p),rd_u32(p+4),rd_u32(p+8),rd_u32(p+12),&fresh);
        } else {
            const char *path=path_at(p,n,8);
            if(!path)return RD_BAD_REQUEST;
            if(!s->files)return RD_DENIED;
            status=rd_files_snapshot(s->files,path,rd_u32(p),rd_u32(p+4),&fresh);
        }
        if(status!=RD_OK) {if(fresh.data)FreeVec(fresh.data);return status;}
        if(!fresh.data || fresh.size>RD_SNAPSHOT_MAX) {if(fresh.data)FreeVec(fresh.data);return RD_IO;}
        rd_service_clear(s);s->snapshot=fresh;if(!++s->snapshot_id)++s->snapshot_id;
        rd_put32(out,s->snapshot_id);rd_put32(out+4,fresh.size);rd_put32(out+8,fresh.width);
        rd_put32(out+12,fresh.height);rd_put32(out+16,rd_crc(fresh.data,fresh.size));*length=20;return RD_OK;
    }
    return RD_BAD_REQUEST;
}
