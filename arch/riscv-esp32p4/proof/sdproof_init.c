/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: A5's proof that lddemon can load a shared library from the card.

    A second and different path from the proof command.  The command reaches
    memory through DOS/LoadSeg and is then simply jumped to.  A library has to
    survive more than that: lddemon loads it with the same LoadSeg, but then
    its resident structure has to be found in the loaded segment, its
    initialisation has to run, a base has to be allocated, and a jump table
    has to be built so callers reach its functions through negative offsets
    from that base.  Any of those can fail while a plain command still works,
    which is why proving one does not prove the other.

    Like the command, this file exists nowhere but on the card, and it has no
    dependency of its own beyond exec, so a successful OpenLibrary()
    demonstrates the loading rather than somebody else's library.
*/

#include <aros/debug.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include <exec/types.h>
#include <exec/libraries.h>
#include <proto/exec.h>

/* The cdef block in sdproof.conf lands in the generated clib protos header,
   which is what proto/sdproof.h pulls in, so this is where struct SDProofBase
   becomes a complete type for this file. */
#include <proto/sdproof.h>

#include "proof_id.h"

#include LC_LIBDEFS_FILE

/*
   In .rodata rather than as a literal inside each function, so its address is
   a statement about the data relocations and not only about the code.
*/
static const char proof_id[] = SDPROOF_LIBRARY_ID;

static int GM_UNIQUENAME(Init)(LIBBASETYPEPTR LIBBASE)
{
    LIBBASE->sdp_Marker = SDPROOF_MARKER;
    LIBBASE->sdp_Id     = (CONST_STRPTR)proof_id;

    bug("[sdproof] init: id %s, base 0x%p, marker 0x%08x, id at 0x%p\n",
        proof_id, LIBBASE, (unsigned)LIBBASE->sdp_Marker, proof_id);

    return TRUE;
}

ADD2INITLIB(GM_UNIQUENAME(Init), 0);

/*
   One function rather than several, and it takes an argument on purpose.
   A jump table that was built but wired to the wrong entry would still
   return something; a function that has to combine a value the caller
   passed in with a value its own initialisation wrote cannot.
*/
AROS_LH1(ULONG, SDProofQuery,
         AROS_LHA(ULONG, what, D0),
         LIBBASETYPEPTR, LIBBASE, 5, SDProof)
{
    AROS_LIBFUNC_INIT

    switch (what)
    {
    case SDPROOF_Q_MARKER:
        /* The marker the initialisation wrote, mixed with the caller's
           value, so neither side can be a constant. */
        return LIBBASE->sdp_Marker ^ SDPROOF_Q_SALT;

    case SDPROOF_Q_ID_ADDR:
        /* A rodata address from inside the loaded segment, for the caller to
           check against the kickstart and flash-package ranges. */
        return (ULONG)(IPTR)LIBBASE->sdp_Id;

    case SDPROOF_Q_BASE:
        return (ULONG)(IPTR)LIBBASE;

    default:
        return 0;
    }

    AROS_LIBFUNC_EXIT
}
