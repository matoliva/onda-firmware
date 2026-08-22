idf_component_register(
    SRCS "display.c" "vendor/font12.c"
    PRIV_REQUIRES driver
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor")
