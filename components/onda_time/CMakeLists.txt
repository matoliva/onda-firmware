idf_component_register(
    SRCS "onda_time.c"
    REQUIRES esp_netif lwip
    PRIV_REQUIRES freertos
    INCLUDE_DIRS "include")
