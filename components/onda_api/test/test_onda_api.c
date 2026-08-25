#include <string.h>
#include <stdio.h>

#include "onda_api.h"
#include "unity.h"

TEST_CASE("device identity parser accepts the BFF contract", "[onda_api]")
{
    static const char response[] =
        "{\"deviceId\":\"device-1\",\"name\":\"Onda Recorder\","
        "\"serverTime\":\"2026-08-24T08:30:00Z\"}";
    onda_api_device_identity_t identity = {0};

    TEST_ASSERT_EQUAL(ESP_OK,
                      onda_api_parse_device_identity(response, strlen(response), &identity));
    TEST_ASSERT_EQUAL_STRING("device-1", identity.device_id);
    TEST_ASSERT_EQUAL_STRING("Onda Recorder", identity.name);
    TEST_ASSERT_EQUAL_STRING("2026-08-24T08:30:00Z", identity.server_time);
}

TEST_CASE("device identity parser rejects incomplete malformed and oversized responses", "[onda_api]")
{
    static const char missing_field[] = "{\"deviceId\":\"device-1\",\"name\":\"Onda\"}";
    static const char wrong_type[] =
        "{\"deviceId\":17,\"name\":\"Onda\",\"serverTime\":\"2026-08-24T08:30:00Z\"}";
    static const char malformed[] = "{\"deviceId\":";
    char long_name[ONDA_API_DEVICE_NAME_MAX_LENGTH + 2U];
    onda_api_device_identity_t identity = {0};

    memset(long_name, 'a', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    char oversized[256U];
    TEST_ASSERT_GREATER_THAN(0,
                             snprintf(oversized, sizeof(oversized),
                                      "{\"deviceId\":\"device-1\",\"name\":\"%s\","
                                      "\"serverTime\":\"2026-08-24T08:30:00Z\"}",
                                      long_name));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      onda_api_parse_device_identity(missing_field, strlen(missing_field), &identity));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      onda_api_parse_device_identity(wrong_type, strlen(wrong_type), &identity));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      onda_api_parse_device_identity(malformed, strlen(malformed), &identity));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE,
                      onda_api_parse_device_identity(oversized, strlen(oversized), &identity));
}

TEST_CASE("device API maps HTTP outcomes to independent authentication states", "[onda_api]")
{
    TEST_ASSERT_EQUAL(ONDA_API_AUTHENTICATED,
                      onda_api_resolve_state(ESP_OK, 200, true));
    TEST_ASSERT_EQUAL(ONDA_API_ERROR,
                      onda_api_resolve_state(ESP_OK, 200, false));
    TEST_ASSERT_EQUAL(ONDA_API_UNAUTHORIZED,
                      onda_api_resolve_state(ESP_OK, 401, false));
    TEST_ASSERT_EQUAL(ONDA_API_UNAUTHORIZED,
                      onda_api_resolve_state(ESP_OK, 403, false));
    TEST_ASSERT_EQUAL(ONDA_API_UNAUTHORIZED,
                      onda_api_resolve_state(ESP_FAIL, 401, false));
    TEST_ASSERT_EQUAL(ONDA_API_ERROR,
                      onda_api_resolve_state(ESP_OK, 500, false));
    TEST_ASSERT_EQUAL(ONDA_API_ERROR,
                      onda_api_resolve_state(ESP_ERR_TIMEOUT, 0, false));
}
