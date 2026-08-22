idf_component_register(
    SRCS "buttons.c"
    PRIV_REQUIRES driver esp_timer freertos
    INCLUDE_DIRS "include")
