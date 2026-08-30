#ifndef AROS_TOUCHSCREEN_H
#define AROS_TOUCHSCREEN_H

/*
 * Optional platform contract behind KATTR_TouchScreenOps.
 *
 * The platform owns controller transport, reset and firmware state.  A
 * hardware input HIDD may poll this table from task context and translate a
 * coherent contact frame into the generic input event stream without linking
 * against private kernel symbols.  Controller-specific report bytes and
 * transport details stay behind this contract.
 */

#include <exec/types.h>

#define KRN_TOUCHSCREEN_OPS_VERSION 3
#define KRN_TOUCHSCREEN_MAX_CONTACTS 10

struct KrnTouchScreenContact
{
    ULONG id;
    ULONG x;
    ULONG y;
};

struct KrnTouchScreenFrame
{
    /* reported_count preserves the raw controller count for diagnosis;
       count contains only bounded parsed contacts present in contact[]. */
    ULONG reported_count;
    ULONG count;
    struct KrnTouchScreenContact contact[KRN_TOUCHSCREEN_MAX_CONTACTS];
};

struct KrnTouchScreenOps
{
    ULONG version;
    ULONG raw_width;
    ULONG raw_height;
    ULONG logical_width;
    ULONG logical_height;

    /* The platform transport is singleton-backed.  A worker acquires one
       bounded polling session and releases it on every exit. */
    BOOL (*acquire)(void);
    VOID (*release)(void);

    /* TRUE means one coherent report was read successfully.  count == 0 is a
       successful release/idle frame, not an error. */
    BOOL (*read_contacts)(struct KrnTouchScreenFrame *frame);
};

#endif /* AROS_TOUCHSCREEN_H */
