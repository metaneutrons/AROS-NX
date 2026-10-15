#ifndef HS_REFERENCE_JOURNAL_H
#define HS_REFERENCE_JOURNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HS_JOURNAL_OFFSET 0x37000u
#define HS_JOURNAL_SECTOR 4096u
#define HS_JOURNAL_TEXT_MAX 768u
struct hs_journal_store {
    void *ctx;
    uint32_t address,size,preimage_crc;
    bool encrypted,debug_connected;
    uint8_t mac[6];
    bool (*read)(void *,uint32_t,void *,size_t);
    bool (*erase)(void *,uint32_t,size_t);
    bool (*write)(void *,uint32_t,const void *,size_t);
};
enum hs_journal_result {
    HS_JOURNAL_WRITTEN=1,HS_JOURNAL_PRESERVED=2,
    HS_JOURNAL_REFUSED=-1,HS_JOURNAL_READ_ERROR=-2,
    HS_JOURNAL_WRITE_UNCERTAIN=-3
};
uint32_t hs_journal_crc(const uint8_t *,size_t);
size_t hs_journal_length(const uint8_t *,size_t);
/* Caller provides a full sector workspace, separate from text. No retries.
 * Only exact approved board/layout, no Debug/FS connection, image below the
 * sector and matching retained AROS sector CRC permit an erase/write. */
int hs_journal_save(const struct hs_journal_store *,const char *,size_t,
                    uint8_t *,size_t);
#endif
