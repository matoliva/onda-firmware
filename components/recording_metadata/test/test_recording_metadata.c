#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recording_metadata.h"
#include "unity.h"

TEST_CASE("recording metadata IDs are stable lowercase 128-bit hex", "[recording_metadata]")
{
    const uint8_t random_bytes[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    char id[RECORDING_METADATA_ID_LENGTH + 1U];

    TEST_ASSERT_EQUAL(ESP_OK, recording_metadata_make_id(random_bytes, id));
    TEST_ASSERT_EQUAL_STRING("rec_00112233445566778899aabbccddeeff", id);
    TEST_ASSERT_EQUAL_UINT32(RECORDING_METADATA_ID_LENGTH, strlen(id));
}

TEST_CASE("recording metadata uses exact PCM millisecond durations", "[recording_metadata]")
{
    uint32_t duration_ms = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, recording_metadata_duration_ms(32000U, &duration_ms));
    TEST_ASSERT_EQUAL_UINT32(1000U, duration_ms);
    TEST_ASSERT_EQUAL(ESP_OK, recording_metadata_duration_ms(2542000U, &duration_ms));
    TEST_ASSERT_EQUAL_UINT32(79437U, duration_ms);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, recording_metadata_duration_ms(0U, &duration_ms));
}

TEST_CASE("recording metadata formats Auckland RFC 3339 timestamps only for trusted clocks", "[recording_metadata]")
{
    char timestamp[32U];

    TEST_ASSERT_EQUAL_INT(0, setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1));
    tzset();
    TEST_ASSERT_EQUAL(ESP_OK, recording_metadata_format_created_at((time_t)1724545125,
                                                                     timestamp, sizeof(timestamp)));
    TEST_ASSERT_EQUAL_STRING("2024-08-25T12:18:45+12:00", timestamp);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, recording_metadata_format_created_at((time_t)0,
                                                                                   timestamp, sizeof(timestamp)));
}
