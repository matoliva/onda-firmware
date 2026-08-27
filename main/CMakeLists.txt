idf_component_register(SRCS "onda_main.c"
                       PRIV_REQUIRES audio audio_recorder battery buttons device_ui onda_api onda_time power recording_metadata recording_naming recording_sync storage wifi esp_psram esp_timer spi_flash
                       INCLUDE_DIRS "")
