/* Private retained-metadata PID reservations for the P4 Exec core. */
#if !defined(P4_ETASK_IDENTITY_BINDING_TEST)
#include <exec/execbase.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include "../kernel/kernel_intern.h"
#include "../kernel/p4_public_spinlock.h"
#include "etask.h"
#include "p4_etask_gate.h"
#include "p4_etask_identity.h"
#include "p4_etask_lifetime.h"
#include "p4_etask_identity_binding.h"
#endif

/* Zero initialization precedes the one-time metadata-gate preparation. */
static struct p4_etask_identity_registry identities P4_SRAMDATA;

/* The identity node is first so the existing opaque registry indexes the
 * private metadata owner without changing public ETask/ExecBase layout. */
struct p4_etask_owner_record
{
    struct p4_etask_identity_record identity;
    struct p4_etask_lifetime lifetime;
};

static struct p4_etask_owner_record *owner_find(const struct ETask *et)
{
    /* Validate the entire registry before any lifetime transition. The gate
     * remains held by every caller through lookup and state mutation. */
    return (struct p4_etask_owner_record *)
        p4_etask_identity_lookup(&identities, et);
}

int krnP4ETaskIdentityReserve(struct ETask *et, struct ExecBase *SysBase)
{
    struct p4_etask_owner_record *record;
    uint32_t token, pid;

    if (et == NULL || et->et_UniqueID != 0)
        return 0;
    record = AllocMem(sizeof(*record), MEMF_PUBLIC | MEMF_CLEAR);
    if (record == NULL)
        return 0;
    if (!p4_etask_lifetime_init(&record->lifetime))
    {
        FreeMem(record, sizeof(*record));
        return 0;
    }

    token = krnP4ETaskGateLock(SysBase);
    pid = p4_etask_identity_reserve(&identities, &record->identity, et,
                                   SysBase->ex_TaskID);
    if (pid != 0)
    {
        SysBase->ex_TaskID = pid;
        et->et_UniqueID = pid;
    }
    krnP4ETaskGateUnlock(token);
    if (pid == 0)
        FreeMem(record, sizeof(*record));
    return pid != 0;
}

void krnP4ETaskMetadataBeginCleanup(struct ETask *et, struct ExecBase *SysBase)
{
    uint32_t token = krnP4ETaskGateLock(SysBase);
    struct p4_etask_owner_record *record = owner_find(et);
    int valid = record != NULL &&
                p4_etask_lifetime_begin_cleanup(&record->lifetime);
    krnP4ETaskGateUnlock(token);
    if (!valid)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

void krnP4ETaskMetadataRetainResult(struct ETask *et, struct ExecBase *SysBase)
{
    uint32_t token = krnP4ETaskGateLock(SysBase);
    struct p4_etask_owner_record *record = owner_find(et);
    int valid = record != NULL &&
        p4_etask_lifetime_finish_cleanup(&record->lifetime, 1) ==
            P4_ETASK_RETAINED;
    krnP4ETaskGateUnlock(token);
    if (!valid)
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
}

int krnP4ETaskIdentityRelease(struct ETask *et, struct ExecBase *SysBase)
{
    struct p4_etask_identity_record *identity = NULL;
    struct p4_etask_owner_record *record;
    enum p4_etask_release release = P4_ETASK_REFUSED;
    uint32_t token = krnP4ETaskGateLock(SysBase);

    record = owner_find(et);
    if (record != NULL && p4_etask_lifetime_valid(&record->lifetime))
    {
        if (record->lifetime.task_ref)
        {
            /* Unpublished affinity rollback has no preceding CleanupETask. */
            if (record->lifetime.closing ||
                p4_etask_lifetime_begin_cleanup(&record->lifetime))
                release = p4_etask_lifetime_finish_cleanup(&record->lifetime, 0);
        }
        else
            release = p4_etask_lifetime_drop_result(&record->lifetime);
        if (release == P4_ETASK_RECLAIM)
            identity = p4_etask_identity_release(&identities, et);
    }
    krnP4ETaskGateUnlock(token);
    if (release == P4_ETASK_RETAINED)
        return 0;
    if (identity == NULL)
    {
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        return 0;
    }
    FreeMem(record, sizeof(*record));
    return 1;
}

int krnP4ETaskMetadataReadPin(struct ETask *et, struct ExecBase *SysBase)
{
    uint32_t token = krnP4ETaskGateLock(SysBase);
    struct p4_etask_owner_record *record = owner_find(et);
    int admitted = record != NULL &&
                   p4_etask_lifetime_read_pin(&record->lifetime);
    krnP4ETaskGateUnlock(token);
    return admitted;
}

void krnP4ETaskMetadataReadUnpin(struct ETask *et, struct ExecBase *SysBase)
{
    uint32_t token = krnP4ETaskGateLock(SysBase);
    struct p4_etask_owner_record *record = owner_find(et);
    struct p4_etask_identity_record *identity = NULL;
    enum p4_etask_release release = P4_ETASK_REFUSED;

    if (record != NULL)
        release = p4_etask_lifetime_read_unpin(&record->lifetime);
    if (release == P4_ETASK_RECLAIM)
        identity = p4_etask_identity_release(&identities, et);
    krnP4ETaskGateUnlock(token);
    if (release == P4_ETASK_REFUSED ||
        (release == P4_ETASK_RECLAIM && identity == NULL))
    {
        krnP4PublicSpinFatal(P4_PUBLIC_SPIN_BAD_STATE);
        return;
    }
    if (identity != NULL)
    {
        FreeMem(record, sizeof(*record));
        /* Unique last-reader claim; never recurse into IdentityRelease. */
        Exec_FreeETaskStorage(et, SysBase);
    }
}
