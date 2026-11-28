/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: A5's load proof without the C startup.

    The same claim as sdboot-test, with one fewer thing in the way.

    sdboot-test is a normal AROS command and reaches its entry point
    correctly - the startup's own narration shows __startup_entry_body()
    running with the right argument string and SysBase - but it does not reach
    main().  Somewhere in the PROGRAM_ENTRIES chain between the two it stops,
    on a target where no program had ever run before.  That is a real problem
    and worth fixing, but it is not what A5 is about: A5 asks whether
    DOS/LoadSeg can fetch code off the card, relocate it and run it.

    So this file asks exactly that and nothing else.  It is entered the same
    way, through the same trampoline the same loader built, and then it is on
    its own: no task.resource, no symbol set walk, no command line parsing, no
    stdio.  __startup places it in .aros.startup so the linker puts it at the
    front of .text, which is the hunk the loader makes the entry.

    SysBase is assigned by hand because nothing else will do it here.  Every
    call below goes through it, so if the assignment or the relocations were
    wrong this would fault rather than mislead.
*/

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <aros/system.h>
#include <aros/asmcall.h>
#include <aros/libcall.h>
#include <proto/exec.h>

#include "proof_id.h"

#define SDPROOF_QUERY_LVO   5
#define SDProofQuery(base, what)                                        \
    AROS_LC1(ULONG, SDProofQuery,                                       \
             AROS_LCA(ULONG, (what), D0),                               \
             struct Library *, (base), SDPROOF_QUERY_LVO, SDProof)

/* No startup code to set this up, so it is ours to fill in. */
struct ExecBase *SysBase;

/* In .rodata: its address says whether the data relocations were applied,
   not merely whether the entry point was reached. */
static const char id[] = SDBOOT_TEST_ID "-nostartup";

static void put_str(const char *s)
{
    while (*s)
        RawPutChar(*s++);
}

static void put_hex(IPTR value)
{
    static const char digits[] = "0123456789abcdef";
    unsigned int i;

    put_str("0x");
    for (i = 0; i < sizeof(IPTR) * 2; i++)
        RawPutChar(digits[(value >> ((sizeof(IPTR) * 8 - 4) - i * 4)) & 0xF]);
}

__startup AROS_PROCH(sdload_entry, argstr, argsize, sysBase)
{
    AROS_PROCFUNC_INIT

    struct Library *SDProofBase;
    int failed = 0;

    SysBase = sysBase;

    put_str("[sdload] entered, id ");
    put_str(id);
    put_str("\n[sdload] entry at ");
    put_hex((IPTR)&sdload_entry);
    put_str("\n[sdload] id string at ");
    put_hex((IPTR)id);
    put_str("\n[sdload] argsize ");
    put_hex((IPTR)argsize);
    put_str(" SysBase ");
    put_hex((IPTR)sysBase);
    put_str("\n");

    SDProofBase = OpenLibrary((CONST_STRPTR)"sdproof.library", 1);
    if (!SDProofBase)
    {
        put_str("[sdload] sdproof.library did NOT open\n[sdload] FAILED\n");
        return RETURN_FAIL;
    }

    put_str("[sdload] sdproof base ");
    put_hex((IPTR)SDProofBase);
    put_str("\n");

    {
        ULONG marker = SDProofQuery(SDProofBase, SDPROOF_Q_MARKER);

        put_str("[sdload] query marker ");
        put_hex(marker);
        put_str(" expected ");
        put_hex(SDPROOF_EXPECTED);
        if (marker == SDPROOF_EXPECTED)
            put_str("  match\n");
        else
        {
            put_str("  MISMATCH\n");
            failed = 1;
        }
    }

    CloseLibrary(SDProofBase);

    put_str(failed ? "[sdload] FAILED\n"
                   : "[sdload] passed, loaded from the card without the C startup\n");

    return failed ? RETURN_FAIL : RETURN_OK;

    AROS_PROCFUNC_EXIT
}
