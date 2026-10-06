/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: A5's proof that DOS can load and run code from the MicroSD card.

    This file exists in order to be absent from everywhere else.  It is not in
    the kickstart and not in the flash package, so if it runs at all it can
    only have come off the card: through Open()/Read() on the FAT handler and
    then through dos.library's own ELF loader in
    rom/dos/internalloadseg_elf.c.  That is a different loader from the
    platform one in kernel_elf.c which places the flash package, with its own
    relocator and its own cache handling, so M6's flash-package proof says
    nothing about this path.

    It proves the second half as well, by opening sdproof.library.  A command
    is loaded and jumped to; a library has to be loaded, have its resident
    found, its base allocated, its initialisation run and a jump table built
    before a caller reaches it.  Either can fail while the other works.

    What it prints is meant to be checkable rather than merely plausible:

      - the identity from proof_id.h, which the image manifest also records,
        so the file that ran is the file that was built;
      - the addresses of its own code and of a string constant, because a
        relocation that silently did nothing would still let the code run
        while leaving every pointer it uses wrong.  The judging of those
        addresses against the kickstart and flash-package ranges is done by
        the AFTERDOS probe in the kickstart, which knows where those are;
      - the library's answer to a call that mixes a value passed in with a
        value the library's own initialisation wrote, so neither side can
        satisfy it with a constant.

    Output goes through exec's RawPutChar() rather than through DOS file
    handles, and that is deliberate.  This has to be able to report from its
    first instruction, in two situations that differ: run from the shell it
    has a console on its standard output, but run from the AFTERDOS probe it
    has NIL: there, because that pass happens before any console exists.  With
    DOS handles the second case prints nothing, which is exactly the case
    where something going wrong is hardest to see - and it did go wrong once,
    silently.  RawPutChar() reaches the platform console either way.
*/

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <aros/libcall.h>

#include "proof_id.h"

/*
   The library call is written out here rather than taken from the generated
   <proto/sdproof.h>.  Two reasons, and the second is the important one.

   The build order does not guarantee that the library's published headers are
   current when this file is compiled: a changed function list showed up as an
   "implicit declaration" of a function that did exist, because the copy in
   the SDK include directory was one build old.

   More to the point, this file is a proof.  Depending on another module's
   generated build artefacts gives it a way to fail that has nothing to do
   with what it is meant to demonstrate.  One AROS_LC1 against a documented
   LVO is fewer moving parts than a header chain, and if the LVO ever moves,
   the marker check below fails loudly rather than silently calling the wrong
   entry.
*/
#define SDPROOF_QUERY_LVO   5

#define SDProofQuery(base, what)                                        \
    AROS_LC1(ULONG, SDProofQuery,                                       \
             AROS_LCA(ULONG, (what), D0),                               \
             struct Library *, (base), SDPROOF_QUERY_LVO, SDProof)

/* In .rodata, so its address says whether the data relocations were applied
   and not merely whether the entry point was reached. */
static const char id[] = SDBOOT_TEST_ID;

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

static void put_dec(ULONG value)
{
    char buf[11];
    int i = 10;

    buf[i] = '\0';
    do
    {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    } while (value && i > 0);

    put_str(&buf[i]);
}

int main(void)
{
    struct Library *SDProofBase;
    int failed = 0;

    /* Before anything else, so that "did main run at all" is answerable. */
    put_str("[sdboot] entered main\n");

    put_str("[sdboot] id ");
    put_str(id);
    put_str("\n[sdboot] main at ");
    put_hex((IPTR)&main);
    put_str("\n[sdboot] id string at ");
    put_hex((IPTR)id);
    put_str("\n");

    /*
     * lddemon has to find this on LIBS: and load it the same way; it is not
     * in the flash package either.  Version 1 is asked for explicitly, so a
     * stale or wrong file cannot satisfy the open.
     */
    put_str("[sdboot] opening sdproof.library\n");
    SDProofBase = OpenLibrary((CONST_STRPTR)"sdproof.library", 1);
    if (!SDProofBase)
    {
        put_str("[sdboot] sdproof.library did NOT open, IoErr ");
        put_dec((ULONG)IoErr());
        put_str("\n[sdboot] FAILED\n");
        return RETURN_FAIL;
    }

    put_str("[sdboot] sdproof base ");
    put_hex((IPTR)SDProofBase);
    put_str(" version ");
    put_dec(SDProofBase->lib_Version);
    put_str("\n");

    {
        ULONG marker = SDProofQuery(SDProofBase, SDPROOF_Q_MARKER);

        put_str("[sdboot] query marker ");
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

    put_str("[sdboot] sdproof id string at ");
    put_hex((IPTR)SDProofQuery(SDProofBase, SDPROOF_Q_ID_ADDR));
    put_str("\n[sdboot] sdproof base reported ");
    put_hex((IPTR)SDProofQuery(SDProofBase, SDPROOF_Q_BASE));
    put_str("\n");

    CloseLibrary(SDProofBase);

    put_str(failed ? "[sdboot] FAILED\n"
                   : "[sdboot] passed, loaded from the card\n");

    return failed ? RETURN_FAIL : RETURN_OK;
}
