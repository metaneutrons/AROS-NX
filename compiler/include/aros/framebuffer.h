#ifndef AROS_FRAMEBUFFER_H
#define AROS_FRAMEBUFFER_H

/*
 * Optional platform contract behind KATTR_FrameBufferOps.
 *
 * A linear boot framebuffer can continue to use KATTR_FrameBuffer directly.
 * Platforms whose public display orientation differs from their DMA surface
 * can instead publish this table.  The graphics HIDD owns logical bitmaps;
 * the kernel remains the sole owner of scanout, cache publication and the
 * frame-boundary handoff.
 */

#include <exec/types.h>

#define KRN_FRAMEBUFFER_OPS_VERSION 2

struct KrnFrameBufferStats
{
    ULONG frames;
    ULONG swaps;
    ULONG faults;
    ULONG rejects;
    IPTR  active;
    IPTR  pending;
};

struct KrnFrameBufferOps
{
    ULONG version;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG bytes_per_pixel;
    ULONG logical_pitch;
    IPTR  physical_front;
    IPTR  physical_back;
    ULONG physical_size;

    /* TRUE means the pixels have been copied and queued. The caller may
     * immediately change/free logical storage. Presentation may be deferred. */
    BOOL (*update_rect)(CONST_APTR logical, ULONG logical_pitch,
                        LONG x, LONG y, LONG width, LONG height);
    BOOL (*clear)(ULONG pixel);
    VOID (*get_stats)(struct KrnFrameBufferStats *stats);
    /* Bounded task-context wait for previously queued pixels to be displayed.
     * Never required after each ordinary drawing operation. */
    BOOL (*flush)(VOID);
};

#endif /* AROS_FRAMEBUFFER_H */
