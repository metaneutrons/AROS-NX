# Diagnostic-only extra bootloader component. Not in the normal project.
idf_component_register(SRCS "console.c" REQUIRES esp_rom WHOLE_ARCHIVE)
target_link_options(${COMPONENT_LIB} INTERFACE
    "LINKER:--wrap=esp_rom_install_uart_printf")
