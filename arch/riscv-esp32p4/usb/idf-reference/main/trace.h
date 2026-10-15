#ifndef HS_REFERENCE_TRACE_H
#define HS_REFERENCE_TRACE_H

#include "journal.h"

#define HS_TRACE_RECORD 256u
#define HS_TRACE_TEXT_MAX 236u
#define HS_TRACE_SLOTS 16u

struct hs_trace_state {
    unsigned next;
    bool active;
};

enum hs_trace_result {
    HS_TRACE_WRITTEN = 1,
    HS_TRACE_PRESERVED = 2,
    HS_TRACE_ARG = -1,
    HS_TRACE_BOARD = -2,
    HS_TRACE_ENCRYPTED = -3,
    HS_TRACE_PREIMAGE = -4,
    HS_TRACE_IMAGE = -5,
    HS_TRACE_READ = -6,
    HS_TRACE_UNCERTAIN = -7,
    HS_TRACE_FULL = -8,
    HS_TRACE_INACTIVE = -9
};

/* Development-only bounded trace. The caller supplies one full sector of
 * scratch space. CRCs detect corruption; they do not authenticate records. */
int hs_trace_begin(const struct hs_journal_store *, struct hs_trace_state *,
                   const char *, size_t, uint8_t *, size_t);
int hs_trace_append(const struct hs_journal_store *, struct hs_trace_state *,
                    const char *, size_t, uint8_t *, size_t);
size_t hs_trace_record_length(const uint8_t *, size_t, unsigned);
const char *hs_trace_result_name(int);

#endif
