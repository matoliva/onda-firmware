idf_component_register(SRCS "onda_main.c"
                       PRIV_REQUIRES audio audio_recorder buttons device_ui storage wifi esp_psram esp_timer spi_flash
                       INCLUDE_DIRS "")
