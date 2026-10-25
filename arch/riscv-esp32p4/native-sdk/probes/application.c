#include <exec/lists.h>
#include <proto/alib.h>

volatile unsigned sdk_probe_anchor;

int main(void)
{
    struct List list;

    /* NewList must be linked from the selected SDK, not a header-only probe. */
    NewList(&list);
    sdk_probe_anchor = IsListEmpty(&list) ? 0U : 1U;
    return (int)sdk_probe_anchor;
}
