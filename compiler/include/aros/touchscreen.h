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

#define KRN_TOUCHSCREEN_OPS_VERSION 4
#define KRN_TOUCHSCREEN_MAX_CONTACTS 10

/* The D1001 GSL3670 image is an array of little-endian <offset,value>
   records.  Keeping the expected board-specific record count in the public
   contract lets the file-owning HIDD reject a wrong image before any byte is
   sent to the controller.  Controllers that run from their own ROM (GT911)
   take no image at all. */
#define KRN_TOUCHSCREEN_FW_RECORD_BYTES 8U
#define KRN_TOUCHSCREEN_FW_RECORDS      4356U
#define KRN_TOUCHSCREEN_FW_BYTES \
    (KRN_TOUCHSCREEN_FW_RECORDS * KRN_TOUCHSCREEN_FW_RECORD_BYTES)

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

    /* Load and start one already validated, external little-endian record
       image.  Zero is success.  failed_record and status are always filled;
       the platform owns reset, transport and final 0x5a5a5a5a status
       validation.  This call is task-context only and never retains data.
       NULL when the controller needs no firmware; acquire() then starts it.
       raw_width/raw_height are the range read_contacts() reports in. */
    LONG (*load_firmware)(const UBYTE *data, ULONG bytes,
                          ULONG *failed_record, ULONG *status);

    /* The platform transport is singleton-backed.  A worker acquires one
       bounded polling session and releases it on every exit. */
    BOOL (*acquire)(void);
    VOID (*release)(void);

    /* TRUE means one coherent report was read successfully.  count == 0 is a
       successful release/idle frame, not an error. */
    BOOL (*read_contacts)(struct KrnTouchScreenFrame *frame);
};

#endif /* AROS_TOUCHSCREEN_H */
