idf_component_register(
    SRCS "audio_recorder.c"
    PRIV_REQUIRES audio storage freertos
    INCLUDE_DIRS "include")
