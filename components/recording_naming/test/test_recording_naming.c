#include <stdlib.h>
#include <time.h>

#include "recording_naming.h"
#include "unity.h"

TEST_CASE("recording names use Auckland paths only for reliable clocks", "[recording_naming]")
{
    char path[RECORDING_NAMING_PATH_MAX];
    const time_t timestamp = 1724545125; /* 2024-08-25T00:18:45Z */

    TEST_ASSERT_EQUAL_INT(0, setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1));
    tzset();

    TEST_ASSERT_TRUE(recording_naming_time_is_reliable(timestamp));
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_make_path(timestamp, 0U, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/recordings/2024-08-25/12-18-45.wav", path);
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_make_path(timestamp, 1U, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/recordings/2024-08-25/12-18-45-0001.wav", path);

    TEST_ASSERT_FALSE(recording_naming_time_is_reliable((time_t)0));
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_make_path((time_t)0, 1U, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/recordings/unknown-date/recording-0001.wav", path);
}

TEST_CASE("recording names apply Auckland daylight saving transitions", "[recording_naming]")
{
    char path[RECORDING_NAMING_PATH_MAX];

    TEST_ASSERT_EQUAL_INT(0, setenv("TZ", "NZST-12NZDT,M9.5.0,M4.1.0/3", 1));
    tzset();
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_make_path((time_t)1727531940, 0U, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/recordings/2024-09-29/01-59-00.wav", path);
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_make_path((time_t)1727532000, 0U, path, sizeof(path)));
    TEST_ASSERT_EQUAL_STRING("/sdcard/recordings/2024-09-29/03-00-00.wav", path);
}

TEST_CASE("recording names reject invalid collision ordinals", "[recording_naming]")
{
    char path[RECORDING_NAMING_PATH_MAX];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      recording_naming_make_path((time_t)0, 0U, path, sizeof(path)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      recording_naming_make_path((time_t)0,
                                                 RECORDING_NAMING_MAX_ORDINAL + 1U,
                                                 path,
                                                 sizeof(path)));
}

TEST_CASE("recording durations use static display formats", "[recording_naming]")
{
    char duration[12U];

    TEST_ASSERT_EQUAL(ESP_OK, recording_naming_format_duration(42U * 32000U, duration, sizeof(duration)));
    TEST_ASSERT_EQUAL_STRING("00:42", duration);
    TEST_ASSERT_EQUAL(ESP_OK,
                      recording_naming_format_duration(3723U * 32000U, duration, sizeof(duration)));
    TEST_ASSERT_EQUAL_STRING("01:02:03", duration);
}
