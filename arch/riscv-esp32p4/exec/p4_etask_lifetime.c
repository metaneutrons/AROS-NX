/* Metadata leaf protocol. Topology/Exec binding is not selected yet. */
#include <stddef.h>
#include "p4_etask_lifetime.h"

int p4_etask_lifetime_valid(const struct p4_etask_lifetime *s)
{
    if (s == NULL || s->initialized != 1u || s->task_ref > 1u ||
        s->result_ref > 1u || s->closing > 1u || s->expunge > 1u ||
        s->reclaim_claimed > 1u)
        return 0;
    if (s->task_ref)
        return !s->result_ref && !s->expunge && !s->reclaim_claimed;
    if (!s->closing || s->child_edges)
        return 0;
    if (s->result_ref)
        return !s->expunge && !s->reclaim_claimed;
    return s->expunge && (!s->reclaim_claimed || !s->readers);
}

int p4_etask_lifetime_init(struct p4_etask_lifetime *s)
{
    if (s == NULL || s->initialized || s->task_ref || s->result_ref ||
        s->child_edges || s->readers || s->closing || s->expunge ||
        s->reclaim_claimed)
        return 0;
    s->initialized = 1;
    s->task_ref = 1;
    return 1;
}

int p4_etask_lifetime_add_edge(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || !s->task_ref || s->closing ||
        s->child_edges == UINT32_MAX)
        return 0;
    ++s->child_edges;
    return 1;
}

int p4_etask_lifetime_drop_edge(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || !s->child_edges)
        return 0;
    --s->child_edges;
    return 1;
}

int p4_etask_lifetime_read_pin(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || s->expunge ||
        (s->task_ref && s->closing) || s->readers == UINT32_MAX)
        return 0;
    ++s->readers;
    return 1;
}

static enum p4_etask_release reclaim_if_drained(struct p4_etask_lifetime *s)
{
    if (s->expunge && !s->readers)
    {
        s->reclaim_claimed = 1;
        return P4_ETASK_RECLAIM;
    }
    return P4_ETASK_RETAINED;
}

enum p4_etask_release p4_etask_lifetime_read_unpin(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || !s->readers || s->reclaim_claimed)
        return P4_ETASK_REFUSED;
    --s->readers;
    return reclaim_if_drained(s);
}

int p4_etask_lifetime_begin_cleanup(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || !s->task_ref || s->closing)
        return 0;
    s->closing = 1;
    return 1;
}

enum p4_etask_release p4_etask_lifetime_finish_cleanup(
    struct p4_etask_lifetime *s, int retain_result)
{
    if (!p4_etask_lifetime_valid(s) || !s->task_ref || !s->closing ||
        s->child_edges || (retain_result != 0 && retain_result != 1))
        return P4_ETASK_REFUSED;
    s->task_ref = 0;
    s->result_ref = (uint32_t)retain_result;
    s->expunge = (uint32_t)!retain_result;
    return reclaim_if_drained(s);
}

enum p4_etask_release p4_etask_lifetime_drop_result(struct p4_etask_lifetime *s)
{
    if (!p4_etask_lifetime_valid(s) || !s->result_ref || s->reclaim_claimed)
        return P4_ETASK_REFUSED;
    s->result_ref = 0;
    s->expunge = 1;
    return reclaim_if_drained(s);
}
