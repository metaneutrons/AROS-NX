/*
    Copyright (C) 1995-2011, The AROS Development Team. All rights reserved.

    Desc: Create a new task, improved version
*/

#include <proto/arossupport.h>
#include <exec/memory.h>
#include <exec/execbase.h>

#include <string.h>

#include "exec_debug.h"
#include "exec_util.h"

struct newMemList
{
    struct Node     nml_Node;
    UWORD           nml_NumEntries;
    struct MemEntry nml_ME[4];
};

static const struct newMemList MemTemplate =
{
    { 0, },
    2,
    {
        {{MEMF_CLEAR|MEMF_PUBLIC}, sizeof(struct Task)   }, /* Task descriptor itself */
        {{MEMF_CLEAR            }, AROS_STACKSIZE        }, /* Task's stack           */
        {{MEMF_CLEAR|MEMF_PUBLIC}, 0                     }, /* Task name              */
        {{MEMF_PUBLIC           }, sizeof(struct MsgPort)}  /* Task's MsgPort         */
    }
};

/*****************************************************************************

    NAME */
#include <exec/tasks.h>
#include <proto/exec.h>

        AROS_LH1(struct Task *, NewCreateTaskA,

/*  SYNOPSIS */
        AROS_LHA(struct TagItem *, tags, A0),

/*  LOCATION */
        struct ExecBase *, SysBase, 153, Exec)

/*  FUNCTION
        Create a new task.

    INPUTS
        tags - TagList which may contain the following tags:

          TASKTAG_ERROR        (ULONG *) - a pointer to an optional location for secondary
                                           return code. The code itself will be set to
                                           TASKERROR_OK on success or TASKERROR_NOMEMORY on
                                           failure.
          TASKTAG_PC           (APTR)    - Start address of the task's code.
          TAGKTAG_FINALPC      (APTR)    - Address of the finalization routine. Defaults to
                                           SysBase->TaskExitCode.
          TASKTAG_STACKSIZE    (ULONG)   - Size of task's stack. Defaults to CPU-dependent
                                           value.
          TASKTAG_NAME         (STRPTR)  - A pointer to task name. The name will be copied.
          TASKTAG_USERDATA     (APTR)    - Anything. Will be written into tc_UserData.
          TASKTAG_PRI          (BYTE)    - Task's priority. Defaults to 0.
          TASKTAG_ARG1 ...
          TASKTAG_ARG8         (IPTR)    - Arguments (up to 8) which will be passed to task's
                                           entry function. The arguments are supplied in
                                           C-standard way.
          TASKTAG_FLAGS        (ULONG)   - Initial value for tc_Flags.
          TASKTAG_TASKMSGPORT  (struct MsgPort **)
                                         - Create a message port for the task and place its
                                           address into the location specified by ti_Data.
          TASKTAG_TCBEXTRASIZE (ULONG)   - Value which will be added to sizeof(struct Task)
                                           in order to determine final size of task structure.
                                           Can be used for appending user data to task structure.

    RESULT
        A pointer to the new task or NULL on failure.

    NOTES

    EXAMPLE

    BUGS
        Value of TASKTAG_FLAGS is actually ignored.
        There are some more tags which are currently not implemented.

    SEE ALSO

    INTERNALS

    HISTORY

******************************************************************************/
{
    AROS_LIBFUNC_INIT

    struct Task * newtask;
    struct newMemList nml = MemTemplate;
    struct MemList  * ml;
    struct TagItem *tag, *tstate = tags;
    struct MsgPort **msgPortPtr = NULL;
    ULONG *errPtr    = NULL;
    APTR   initpc    = NULL;
    APTR   finalpc   = SysBase->TaskExitCode;
    char  *taskname  = NULL;
    APTR   userdata  = NULL;
    ULONG  pri       = 0;
    ULONG  flags     = 0;
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN) || defined(EXEC_PLATFORM_CREATION_SCOPE_END) || defined(EXEC_PLATFORM_CREATION_SCOPE_REFUSED)
#if !defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN) || !defined(EXEC_PLATFORM_CREATION_SCOPE_END) || !defined(EXEC_PLATFORM_CREATION_SCOPE_REFUSED) || !defined(EXEC_PLATFORM_CREATION_OWNERSHIP_RESOLVE) || !defined(EXEC_PLATFORM_CREATION_OWNERSHIP_TAG) || !defined(EXEC_PLATFORM_CREATION_GENERATION_TAG)
#error "Creation scopes require matched Begin/End/refusal, resolver and authority tags"
#endif
#endif
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
    ULONG creationOwnership = EXEC_PLATFORM_CREATION_CALLER_OWNED;
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
    BOOL creationScopeRefused = FALSE;
    struct p4_creation_channel creationFrame = { 0 };
    ULONG creationGeneration = 0;
#endif
    struct TagItem creationTags[] = {
        { EXEC_PLATFORM_CREATION_OWNERSHIP_TAG, (IPTR)&creationOwnership },
        { EXEC_PLATFORM_CREATION_GENERATION_TAG, 0 },
        { TAG_MORE, (IPTR)tags },
        { TAG_DONE, 0 }
    };
#endif

    while ((tag = LibNextTagItem(&tstate)))
    {
        switch (tag->ti_Tag)
        {
        case TASKTAG_ERROR:
            errPtr = (ULONG *)tag->ti_Data;
            break;

        case TASKTAG_PC:
            initpc = (APTR)tag->ti_Data;
            break;

        case TASKTAG_FINALPC:
            finalpc = (APTR)tag->ti_Data;
            break;

        case TASKTAG_STACKSIZE:
            nml.nml_ME[1].me_Length = AROS_ALIGN((ULONG)tag->ti_Data);
            break;

        case TASKTAG_NAME:
            taskname = (char *)tag->ti_Data;
            nml.nml_ME[2].me_Length = strlen(taskname) + 1;
            if (nml.nml_NumEntries < 3)
                nml.nml_NumEntries = 3;
            break;

        case TASKTAG_USERDATA:
            userdata = (APTR)tag->ti_Data;
            break;

        case TASKTAG_PRI:
            pri = tag->ti_Data;
            break;

        case TASKTAG_TASKMSGPORT:
            msgPortPtr = (struct MsgPort **)tag->ti_Data;
            if (nml.nml_NumEntries < 4)
                nml.nml_NumEntries = 4;
            break;

        case TASKTAG_FLAGS:
            flags = tag->ti_Data;
            break;

        case TASKTAG_TCBEXTRASIZE:
            nml.nml_ME[0].me_Length += tag->ti_Data;
            break;
        }
    }

    /* The output port is valid only when task creation succeeds. */
    if (msgPortPtr)
        *msgPortPtr = NULL;

    /* We need a minimum stack to handle interrupt contexts. Additionally confirned on MorphOS that
       it sets a minimum fixed size of stack */
    if (nml.nml_ME[1].me_Length < AROS_STACKSIZE)
    {
        nml.nml_ME[1].me_Length = AROS_STACKSIZE;
    }


    DADDTASK("NewCreateTaskA: name %s", taskname ? taskname : "<NULL>");

    ml = NewAllocEntry((struct MemList *)&nml, NULL);
    if (ml)
    {
        struct Task *task2 = NULL;
        /* The minimal unnamed/no-port template contains only Task + stack.
         * Do not read a third MemEntry unless it was actually allocated. */
        APTR name = ml->ml_NumEntries > 2 ? ml->ml_ME[2].me_Addr : NULL;

        if (taskname)
            strcpy(name, taskname);

        newtask = ml->ml_ME[0].me_Addr;

#if defined(__AROSEXEC_SMP__) && defined(__AROSPLATFORM_SMP__)
        /* Fresh creator-owned storage, not yet registered or published.
         * The port signal is allocated before NewAddTask, so initialize the
         * canonical state lock here, before AllocTaskSignal can acquire it.
         * NewAddTask may initialize it again only while still unpublished. */
        EXEC_SPINLOCK_INIT(&newtask->tc_SpinLock);
#endif

        newtask->tc_Node.ln_Type = NT_TASK;
        newtask->tc_Node.ln_Pri  = pri;
        newtask->tc_Node.ln_Name = name;

        if (nml.nml_NumEntries == 4)
        {
            /*
             * The caller asked us to create an MsgPort for the task.
             * We can't reuse ETask's one because it's used for notifications about child tasks.
             * We don't check against msgPortPtr == NULL because in future we can have MorphOS-compatible
             * GetTaskAttr().
             */
            struct MsgPort *mp = ml->ml_ME[3].me_Addr;
            LONG sig;

            /*
             * Allocate a signal for the port.
             * Set tc_SigAlloc early for AllocTaskSignal() to work.
             */
            newtask->tc_SigAlloc = SysBase->TaskSigAlloc;
            sig = AllocTaskSignal(newtask, -1, SysBase);

            if (sig == -1)
            {
                DADDTASK("NewCreateTaskA: Failed to allocate a signal for MsgPort");
                goto fail;
            }

            InitMsgPort(mp);
            mp->mp_SigBit  = sig;
            mp->mp_SigTask = newtask;

            /* Return port address */
            if (msgPortPtr)
                *msgPortPtr = mp;
        }

        /* FIXME: NewAddTask() will reset flags to 0 */
        newtask->tc_Flags    = flags;
        newtask->tc_UserData = userdata;

        /*
         * On some architectures (PPC) stack frames must be preallocated.
         * SP_OFFSET is subtracted in order to take care of this.
         */
        newtask->tc_SPReg   = ml->ml_ME[1].me_Addr + nml.nml_ME[1].me_Length - SP_OFFSET;
        newtask->tc_SPLower = ml->ml_ME[1].me_Addr;
        newtask->tc_SPUpper = newtask->tc_SPReg;

        NEWLIST(&newtask->tc_MemEntry);
        AddHead(&newtask->tc_MemEntry, &ml->ml_Node);

        /*
         * TASKTAG_ARGx will be processed by PrepareContext()
         * TODO: process initpc and finalpc there too after ABI v1 transition
         */
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
        /* Use the public vector and retain the original tag chain: bypassing
         * it would lose task.resource's interposed prelaunch registration. */
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
        if (!EXEC_PLATFORM_CREATION_SCOPE_BEGIN(newtask, &creationFrame))
        {
            /* No vector/record publication: caller still owns this MemList.
             * Refusal policy runs only after the private allocation rollback. */
            creationScopeRefused = TRUE;
            task2 = NULL;
            goto fail;
        }
        creationGeneration = creationFrame.generation;
        creationTags[0].ti_Data = (IPTR)&creationFrame.value;
        creationTags[1].ti_Data = (IPTR)creationGeneration;
#endif
        task2 = NewAddTask (newtask, initpc, finalpc, creationTags);
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
        if (!EXEC_PLATFORM_CREATION_SCOPE_END(creationGeneration, &creationFrame))
        {
            /* Published ownership may already belong to cleanup. Do not read
             * frame/value/Task or run allocation rollback on this path. */
            EXEC_PLATFORM_CREATION_SCOPE_REFUSED();
            EXEC_PLATFORM_TASK_FATAL();
            return NULL; /* Fatal/refusal hooks must not resume this path. */
        }
        creationOwnership = creationFrame.value;
#endif
#else
        task2 = NewAddTask (newtask, initpc, finalpc, tags);
#endif

fail:   if (!task2)
        {
            /* NewAddTask can fail after the port was published for launch. */
            if (msgPortPtr)
                *msgPortPtr = NULL;
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
            if (creationOwnership == EXEC_PLATFORM_CREATION_CALLER_OWNED)
                FreeEntry (ml);
            else if (creationOwnership != EXEC_PLATFORM_CREATION_SERVICE_OWNED)
                EXEC_PLATFORM_TASK_FATAL();
#else
            FreeEntry (ml);
#endif
            newtask = NULL;
        }
#if defined(EXEC_PLATFORM_CREATION_SCOPE_BEGIN)
        if (creationScopeRefused)
            EXEC_PLATFORM_CREATION_SCOPE_REFUSED();
#endif
    }
    else
        newtask=NULL;

    /* Set secondary error code if requested */
    if (errPtr)
        *errPtr = newtask ? TASKERROR_OK : TASKERROR_NOMEMORY;

    return newtask;

    AROS_LIBFUNC_EXIT
}
