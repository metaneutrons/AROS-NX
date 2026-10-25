/* Host-referenced read-only fixture, not a baseline measured on the target.
 * 64 MiB SYS image, SHA-256:
 * 5f123f79d907f82dc46018d0a62934131d3883eb263cba4b736cee24a8eb397a
 * Each range spans 128 sectors. FNV-1a32 covers all bytes in order.
 * The physical card may be larger; only this fixture subset is verified.
 * READ64 is exercised, not the >4 GiB boundary qualified separately in A1.
 */
#ifndef P4_QUALIFICATION_CARD_H
#define P4_QUALIFICATION_CARD_H
#define QUAL_CARD_SECTOR_COUNT 131072UL
#define QUAL_CARD_MBR_FNV32 0x100fcd1cUL
#define QUAL_CARD_RANGE_COUNT 3
#define QUAL_CARD_RANGES \
    { 2048UL, 0x38e173fdUL }, \
    { 8192UL, 0xf3c691e8UL }, \
    { 16384UL, 0x9632bf57UL }
#endif
