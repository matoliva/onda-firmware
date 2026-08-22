idf_component_register(
    SRCS "wifi.c"
    REQUIRES esp_event esp_netif esp_wifi nvs_flash nvs_sec_provider wifi_provisioning
    INCLUDE_DIRS "include")
