idf_component_register(SRCS "onda_main.c"
                       PRIV_REQUIRES audio audio_recorder buttons device_ui onda_api onda_time recording_metadata recording_naming storage wifi esp_psram esp_timer spi_flash
                       INCLUDE_DIRS "")
