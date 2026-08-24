idf_component_register(
    SRCS "storage.c"
    PRIV_REQUIRES esp_driver_sdmmc fatfs sdmmc
    INCLUDE_DIRS "include")
