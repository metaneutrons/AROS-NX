/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: SMP check of an affinity given from outside, as the Affinity
          command or a launcher gives it: the process has the expected
          set of CPUs, its children inherit it, and they run on those CPUs
          and no others.

          SMP-AffCheck EXPECT=ANY|<cpu>[,<cpu>...]

          Four busy children run for half a second each and note the CPUs
          they found themselves on. With ANY and more than one CPU, both
          (all) CPUs must have been used; with a list, no CPU outside it.
*/

#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/rdargs.h>
#include <utility/tagitem.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#if defined(__AROSEXEC_SMP__)
#define DEBUG 1
#include <aros/debug.h>

#include <resources/task.h>
#include <proto/kernel.h>
#include <proto/task.h>

APTR KernelBase;
APTR TaskResBase;

#define CHILDREN        4
#define BUSY_TICKS      25          /* half a second of work each */

struct Child
{
    volatile ULONG  c_Done;
    volatile ULONG  c_Affinity;     /* bit per CPU */
    volatile ULONG  c_Seen;         /* bit per CPU it ran on */
};

static struct Child g_Child[CHILDREN];
static int g_NumCPUs = 1;

static ULONG affinity_bits(struct Task *t)
{
    APTR m = KrnAllocCPUMask();
    struct TagItem tags[] =
    {
        { TaskTag_CPUAffinity, (IPTR)m },
        { TAG_DONE,            0       }
    };
    ULONG bits = 0;
    int cpu;

    if (!m)
        return 0;
    KrnClearCPUMask(m);
    QueryTaskTagList(t, tags);
    for (cpu = 0; cpu < g_NumCPUs && cpu < 32; cpu++)
        if (KrnCPUInMask(cpu, m))
            bits |= 1UL << cpu;
    KrnFreeCPUMask(m);
    return bits;
}

static void ChildEntry(void)
{
    struct Child *c = (struct Child *)FindTask(NULL)->tc_UserData;
    struct DateStamp start, now;

    c->c_Affinity = affinity_bits(FindTask(NULL));
    DateStamp(&start);
    do
    {
        ULONG i;

        for (i = 0; i < 20000; i++)
            c->c_Seen |= 1UL << KrnGetCPUNumber();
        DateStamp(&now);
    } while ((now.ds_Minute - start.ds_Minute) * 3000 +
             (now.ds_Tick - start.ds_Tick) < BUSY_TICKS);
    c->c_Done = 1;
}

/* "ANY" in any case, without utility.library */
static BOOL is_any(CONST_STRPTR s)
{
    return s && (s[0] | 0x20) == 'a' && (s[1] | 0x20) == 'n' &&
           (s[2] | 0x20) == 'y' && s[3] == '\0';
}

/* EXPECT as a bit set: ANY is every online CPU */
static ULONG parse_expect(CONST_STRPTR arg)
{
    ULONG bits = 0;
    CONST_STRPTR p = arg;

    if (is_any(arg))
        return (g_NumCPUs >= 32) ? 0xffffffff : ((1UL << g_NumCPUs) - 1);

    while (*p)
    {
        LONG cpu, used = StrToLong(p, &cpu);

        if (used <= 0 || cpu < 0 || cpu >= g_NumCPUs)
            return 0;
        bits |= 1UL << cpu;
        p += used;
        if (*p == ',')
            p++;
        else if (*p)
            return 0;
    }
    return bits;
}

int main(void)
{
    IPTR args[1] = { 0 };
    struct RDArgs *rda;
    ULONG expect, own, seen = 0;
    int k, t, ok = 1;

    rda = ReadArgs("EXPECT/A", args, NULL);
    if (!rda)
    {
        bug("[smpaffcheck] usage: SMP-AffCheck EXPECT=ANY|<cpu>[,<cpu>...]\n");
        return RETURN_FAIL;
    }

    KernelBase = OpenResource("kernel.resource");
    TaskResBase = OpenResource("task.resource");
    if (!KernelBase || !TaskResBase)
    {
        bug("[smpaffcheck] DONE: 0 PASS, 0 FAIL, 1 INVALID (no kernel or task resource)\n");
        FreeArgs(rda);
        return RETURN_OK;
    }
    g_NumCPUs = KrnGetCPUCount();
    expect = parse_expect((CONST_STRPTR)args[0]);
    if (!expect)
    {
        bug("[smpaffcheck] DONE: 0 PASS, 0 FAIL, 1 INVALID (EXPECT '%s')\n",
            (char *)args[0]);
        FreeArgs(rda);
        return RETURN_OK;
    }

    own = affinity_bits(FindTask(NULL));
    bug("[smpaffcheck] expect 0x%lx, own affinity 0x%lx, cpus %d\n",
        (unsigned long)expect, (unsigned long)own, g_NumCPUs);
    if (own != expect)
    {
        bug("[smpaffcheck] *** FAIL *** own affinity 0x%lx, expected 0x%lx\n",
            (unsigned long)own, (unsigned long)expect);
        ok = 0;
    }

    memset(g_Child, 0, sizeof(g_Child));
    for (k = 0; k < CHILDREN; k++)
    {
        if (!CreateNewProcTags(NP_Entry,     (IPTR)ChildEntry,
                               NP_Name,      (IPTR)"smpaffcheck.child",
                               NP_StackSize, 16384,
                               NP_UserData,  (IPTR)&g_Child[k],
                               TAG_DONE))
        {
            bug("[smpaffcheck] DONE: 0 PASS, 0 FAIL, 1 INVALID (child not created)\n");
            FreeArgs(rda);
            return RETURN_OK;
        }
    }
    for (t = 0; t < 500; t++)
    {
        int done = 0;

        for (k = 0; k < CHILDREN; k++)
            if (g_Child[k].c_Done)
                done++;
        if (done == CHILDREN)
            break;
        Delay(1);
    }
    for (k = 0; k < CHILDREN; k++)
    {
        if (!g_Child[k].c_Done)
        {
            bug("[smpaffcheck] *** FAIL *** child %d did not finish\n", k);
            ok = 0;
            continue;
        }
        seen |= g_Child[k].c_Seen;
        if (g_Child[k].c_Affinity != own)
        {
            bug("[smpaffcheck] *** FAIL *** child %d affinity 0x%lx, parent 0x%lx\n",
                k, (unsigned long)g_Child[k].c_Affinity, (unsigned long)own);
            ok = 0;
        }
        if (g_Child[k].c_Seen & ~expect)
        {
            bug("[smpaffcheck] *** FAIL *** child %d ran on 0x%lx, outside 0x%lx\n",
                k, (unsigned long)g_Child[k].c_Seen, (unsigned long)expect);
            ok = 0;
        }
    }
    bug("[smpaffcheck] children ran on 0x%lx\n", (unsigned long)seen);
    if (seen != expect)
    {
        bug("[smpaffcheck] *** FAIL *** not every allowed CPU was used: 0x%lx of 0x%lx\n",
            (unsigned long)seen, (unsigned long)expect);
        ok = 0;
    }

    Delay(10);      /* let the children's processes end */
    bug("[smpaffcheck] DONE: %d PASS, %d FAIL, 0 INVALID\n", ok, !ok);
    FreeArgs(rda);
    return ok ? RETURN_OK : RETURN_FAIL;
}
#else
int main(void)
{
    return RETURN_FAIL;
}
#endif
