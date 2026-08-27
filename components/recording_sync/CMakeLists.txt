idf_component_register(
    SRCS "recording_sync.c"
    REQUIRES onda_api recording_metadata
    PRIV_REQUIRES freertos
    INCLUDE_DIRS "include")
