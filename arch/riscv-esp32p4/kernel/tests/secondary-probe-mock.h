/* Host-only register/cache/cycle substitutions for the actual controller. */
#include <stddef.h>
#ifndef P4_FB_BASE
/* Host-only framebuffer boundary used to check the E2 scratch publication. */
#define P4_FB_BASE 0x49f00000u
#endif
static uint32_t p4_r32(uint32_t);
static void p4_w32(uint32_t, uint32_t);
static uint32_t mock_cycles(void);
static void krnP4CacheSyncData(void *address, unsigned long size)
{ (void)address; (void)size; }
static void krnP4SyncCode(void *address, unsigned long size)
{ (void)address; (void)size; }
static void krnP4PutStr(const char *text) { (void)text; }
static void krnP4PutHex32(uint32_t value) { (void)value; }
