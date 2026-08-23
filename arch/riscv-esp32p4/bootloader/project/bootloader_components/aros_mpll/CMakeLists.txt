# Calibrate the MSPI PLL in the bootloader, where ESP-IDF's own code for it
# can be used. See aros_mpll.c for why this is here and not in the kernel.
#
# WHOLE_ARCHIVE because bootloader_after_init is a weak symbol: nothing in the
# bootloader references this object, so without it the linker drops the
# component and the hook silently does not run.
idf_component_register(SRCS "aros_mpll.c"
                       PRIV_REQUIRES bootloader_support esp_hw_support hal
                                     esp_hal_clock esp_rom soc log
                       WHOLE_ARCHIVE)
