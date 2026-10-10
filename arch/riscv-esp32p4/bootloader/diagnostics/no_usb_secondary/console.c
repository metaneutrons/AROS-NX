/* Diagnostic: preserve ROM UART setup, remove only the secondary callback.
 * GNU ld wrapping intercepts the bootloader's flash-resident call, not ROM
 * internals. The build must verify the console-init call targets this wrapper.
 * No print here: that would disturb the output-path comparison.
 */
#include <stddef.h>
#include "esp_rom_sys.h"

void __real_esp_rom_install_uart_printf(void);

void __wrap_esp_rom_install_uart_printf(void)
{
    __real_esp_rom_install_uart_printf();
    esp_rom_install_channel_putc(2, NULL);
}
