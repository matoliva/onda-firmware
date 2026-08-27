#include <limits.h>

#include "battery.h"
#include "unity.h"

TEST_CASE("battery converts the board divider voltage", "[battery]")
{
    uint32_t battery_mv = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, battery_convert_adc_millivolts(1850U, &battery_mv));
    TEST_ASSERT_EQUAL_UINT32(3700U, battery_mv);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, battery_convert_adc_millivolts(UINT32_MAX, &battery_mv));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, battery_convert_adc_millivolts(1850U, NULL));
}

TEST_CASE("battery averages calibrated samples and rejects invalid inputs", "[battery]")
{
    const int samples[] = {1800, 1850, 1900, 1850};
    const int overflowing_samples[] = {INT_MAX, INT_MAX, INT_MAX};
    uint32_t average_mv = 0U;

    TEST_ASSERT_EQUAL(ESP_OK, battery_average_millivolts(samples, 4U, &average_mv));
    TEST_ASSERT_EQUAL_UINT32(1850U, average_mv);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, battery_average_millivolts(samples, 0U, &average_mv));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, battery_average_millivolts(NULL, 4U, &average_mv));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      battery_average_millivolts(overflowing_samples, 3U, &average_mv));
}

TEST_CASE("battery classifies every voltage band at its boundary", "[battery]")
{
    TEST_ASSERT_EQUAL(BATTERY_STATUS_HIGH, battery_classify_voltage(3900U));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_MEDIUM, battery_classify_voltage(3899U));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_MEDIUM, battery_classify_voltage(3700U));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_LOW, battery_classify_voltage(3699U));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_LOW, battery_classify_voltage(3500U));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_CRITICAL, battery_classify_voltage(3499U));
}

TEST_CASE("battery filter requires two samples for a level transition", "[battery]")
{
    battery_filter_t filter;
    battery_filter_init(&filter);

    TEST_ASSERT_EQUAL(BATTERY_STATUS_HIGH, battery_filter_update(&filter, BATTERY_STATUS_HIGH));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_HIGH, battery_filter_update(&filter, BATTERY_STATUS_MEDIUM));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_MEDIUM, battery_filter_update(&filter, BATTERY_STATUS_MEDIUM));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_MEDIUM, battery_filter_update(&filter, BATTERY_STATUS_LOW));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_MEDIUM, battery_filter_update(&filter, BATTERY_STATUS_MEDIUM));
    TEST_ASSERT_EQUAL(BATTERY_STATUS_UNKNOWN,
                      battery_filter_update(NULL, BATTERY_STATUS_CRITICAL));
}

TEST_CASE("battery sampling validates its lifecycle and output", "[battery]")
{
    battery_reading_t reading = {0};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, battery_sample(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, battery_sample(&reading));
}
