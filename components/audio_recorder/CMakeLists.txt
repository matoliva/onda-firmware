idf_component_register(
    SRCS "audio_recorder.c"
    REQUIRES recording_naming
    PRIV_REQUIRES audio storage freertos
    INCLUDE_DIRS "include")
