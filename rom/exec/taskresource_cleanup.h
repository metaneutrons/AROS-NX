/* Private matched-core/task.resource cleanup interface; not a public LVO. */
#ifndef EXEC_TASKRESOURCE_CLEANUP_H
#define EXEC_TASKRESOURCE_CLEANUP_H

#include <exec/libraries.h>

struct Task;
#define TASKRESOURCE_CLEANUP_CONTRACT 0x50345233UL

/* This is the first member of the selected resource base. The resource is
 * resident after publication; only the drained service owner calls cleanup. */
struct taskresource_cleanup_binding
{
    struct Library library;
    ULONG contract;
    BOOL (*cleanup)(struct Task *, APTR);
    /* TRUE means task.resource metadata was registered before publication. */
    BOOL (*prelaunch)(struct Task *, APTR);
};

#endif
