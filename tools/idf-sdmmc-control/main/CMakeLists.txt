idf_component_register(SRCS "idf_sdmmc_control.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_sdmmc sdmmc)
