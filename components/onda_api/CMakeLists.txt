idf_component_register(
    SRCS "onda_api.c"
    REQUIRES esp_http_client json mbedtls recording_metadata storage
    PRIV_REQUIRES freertos
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include")

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/private_include/onda_api_config_local.h")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ONDA_API_LOCAL_CONFIG_AVAILABLE)
endif()
