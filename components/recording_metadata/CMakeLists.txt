idf_component_register(
    SRCS "recording_metadata.c"
    REQUIRES json recording_naming storage
    PRIV_REQUIRES esp_hw_support
    INCLUDE_DIRS "include")
