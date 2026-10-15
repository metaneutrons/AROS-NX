#ifndef HS_REFERENCE_FLASH_WINDOW_H
#define HS_REFERENCE_FLASH_WINDOW_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Adapter supplies task identity; must return zero in interrupt context. */
uintptr_t hs_flash_current_owner(void);
enum hs_flash_operation { HS_FLASH_IDLE=0, HS_FLASH_ERASE=1, HS_FLASH_WRITE=2 };
bool hs_flash_window_begin(enum hs_flash_operation);
void hs_flash_window_end(void);
#endif
