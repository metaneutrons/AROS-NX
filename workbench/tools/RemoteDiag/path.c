/* Copyright (C) 2026, The AROS Development Team. */
#include "backend.h"
#include <string.h>
bool rd_valid_relative(const char *path)
{
    if (!path) return false;
    size_t n=0, component=0;
    for (;path[n];++n) {
        unsigned char c=(unsigned char)path[n];
        if(n>=RD_PATH_MAX-1 || c<32 || c==127 || c==':' || c=='\\') return false;
        if(c=='/') {
            if(!component || (component==1 && path[n-1]=='.') ||
                (component==2 && path[n-2]=='.' && path[n-1]=='.')) return false;
            component=0;
        } else ++component;
    }
    if(n && (!component || (component==1 && path[n-1]=='.') ||
        (component==2 && path[n-2]=='.' && path[n-1]=='.'))) return false;
    return true; /* Empty string explicitly denotes the configured root. */
}
