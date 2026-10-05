/*
    Copyright (c) 2026, The AROS Development Team. All rights reserved.

    Desc: The identities the A5 proof files print.

    One place for the strings, because three things have to agree about them:
    the file on the card, the line on the UART and the image manifest.  Any
    two of those matching by accident would prove nothing, so the literal is
    defined once and checked wherever it appears.

    These are deliberately not derived from a timestamp or a build counter.
    The A3 image is byte-reproducible and has to stay that way; an identity
    that changed on every build would make the image change on every build
    and the manifest hashes worthless.  What makes the files unique is their
    content hash, which the manifest already records.
*/

#ifndef ESP32P4_PROOF_ID_H
#define ESP32P4_PROOF_ID_H

#define SDBOOT_TEST_ID      "A5-sdboot-test-1"
#define SDPROOF_LIBRARY_ID  "A5-sdproof-library-1"

/* What SDProofQuery() answers, and the value it mixes into the marker so
   neither the library nor the caller can satisfy the check with a constant. */
#define SDPROOF_Q_MARKER    0
#define SDPROOF_Q_ID_ADDR   1
#define SDPROOF_Q_BASE      2

#define SDPROOF_MARKER      0x5d9200f1UL
#define SDPROOF_Q_SALT      0xa5000a05UL
#define SDPROOF_EXPECTED    (SDPROOF_MARKER ^ SDPROOF_Q_SALT)

#endif /* ESP32P4_PROOF_ID_H */
