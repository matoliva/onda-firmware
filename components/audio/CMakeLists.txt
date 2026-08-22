idf_component_register(
    SRCS "audio.c"
    PRIV_REQUIRES driver esp_codec_dev freertos
    INCLUDE_DIRS "include")
