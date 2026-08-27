#include "battery.h"

#include <stdbool.h>
#include <inttypes.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

/* Verified against Waveshare's 01_ADC_Test example for SKU 34586. */
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_3
#define BATTERY_ADC_ATTENUATION ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH ADC_BITWIDTH_12
#define BATTERY_SAMPLE_COUNT 8U
#define BATTERY_DIVIDER_RATIO 2U
#define BATTERY_HIGH_MIN_MV 3900U
#define BATTERY_MEDIUM_MIN_MV 3700U
#define BATTERY_LOW_MIN_MV 3500U
#define BATTERY_TRANSITION_SAMPLE_COUNT 2U

static const char *TAG = "ONDA_BATTERY";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_calibration_handle;
static battery_filter_t s_filter;
static bool s_initialised;

esp_err_t battery_convert_adc_millivolts(uint32_t adc_mv, uint32_t *battery_mv)
{
    if (battery_mv == NULL || adc_mv > UINT32_MAX / BATTERY_DIVIDER_RATIO) {
        return ESP_ERR_INVALID_ARG;
    }
    *battery_mv = adc_mv * BATTERY_DIVIDER_RATIO;
    return ESP_OK;
}

esp_err_t battery_average_millivolts(const int *samples, size_t count, uint32_t *average_mv)
{
    if (samples == NULL || average_mv == NULL || count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t total_mv = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (samples[index] < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if ((uint32_t)samples[index] > UINT32_MAX - total_mv) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_mv += (uint32_t)samples[index];
    }
    *average_mv = total_mv / count;
    return ESP_OK;
}

battery_status_t battery_classify_voltage(uint32_t voltage_mv)
{
    if (voltage_mv >= BATTERY_HIGH_MIN_MV) {
        return BATTERY_STATUS_HIGH;
    }
    if (voltage_mv >= BATTERY_MEDIUM_MIN_MV) {
        return BATTERY_STATUS_MEDIUM;
    }
    if (voltage_mv >= BATTERY_LOW_MIN_MV) {
        return BATTERY_STATUS_LOW;
    }
    return BATTERY_STATUS_CRITICAL;
}

void battery_filter_init(battery_filter_t *filter)
{
    if (filter == NULL) {
        return;
    }
    *filter = (battery_filter_t){
        .stable_status = BATTERY_STATUS_UNKNOWN,
        .candidate_status = BATTERY_STATUS_UNKNOWN,
        .candidate_count = 0U,
    };
}

battery_status_t battery_filter_update(battery_filter_t *filter, battery_status_t status)
{
    if (filter == NULL || status == BATTERY_STATUS_UNKNOWN) {
        return BATTERY_STATUS_UNKNOWN;
    }
    if (filter->stable_status == BATTERY_STATUS_UNKNOWN) {
        filter->stable_status = status;
        filter->candidate_status = BATTERY_STATUS_UNKNOWN;
        filter->candidate_count = 0U;
        return filter->stable_status;
    }
    if (status == filter->stable_status) {
        filter->candidate_status = BATTERY_STATUS_UNKNOWN;
        filter->candidate_count = 0U;
        return filter->stable_status;
    }
    if (filter->candidate_status != status) {
        filter->candidate_status = status;
        filter->candidate_count = 1U;
        return filter->stable_status;
    }
    if (filter->candidate_count < BATTERY_TRANSITION_SAMPLE_COUNT) {
        ++filter->candidate_count;
    }
    if (filter->candidate_count >= BATTERY_TRANSITION_SAMPLE_COUNT) {
        filter->stable_status = status;
        filter->candidate_status = BATTERY_STATUS_UNKNOWN;
        filter->candidate_count = 0U;
    }
    return filter->stable_status;
}

esp_err_t battery_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t result = adc_oneshot_new_unit(&unit_config, &s_adc_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise ADC unit: %s", esp_err_to_name(result));
        return result;
    }

    const adc_oneshot_chan_cfg_t channel_config = {
        .atten = BATTERY_ADC_ATTENUATION,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    result = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &channel_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure battery ADC channel: %s", esp_err_to_name(result));
        return result;
    }

    const adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = ADC_UNIT_1,
        .atten = BATTERY_ADC_ATTENUATION,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    result = adc_cali_create_scheme_curve_fitting(&calibration_config, &s_calibration_handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Battery ADC calibration unavailable: %s", esp_err_to_name(result));
        return result;
    }

    battery_filter_init(&s_filter);
    s_initialised = true;
    ESP_LOGI(TAG, "Battery ADC initialised");
    return ESP_OK;
}

esp_err_t battery_sample(battery_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    int calibrated_samples[BATTERY_SAMPLE_COUNT] = {0};
    for (size_t index = 0U; index < BATTERY_SAMPLE_COUNT; ++index) {
        int raw_sample = 0;
        esp_err_t result = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &raw_sample);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC read failed: %s", esp_err_to_name(result));
            return result;
        }
        result = adc_cali_raw_to_voltage(s_calibration_handle, raw_sample, &calibrated_samples[index]);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Battery ADC calibration failed: %s", esp_err_to_name(result));
            return result;
        }
    }

    uint32_t average_adc_mv = 0U;
    esp_err_t result = battery_average_millivolts(calibrated_samples, BATTERY_SAMPLE_COUNT,
                                                   &average_adc_mv);
    if (result != ESP_OK) {
        return result;
    }

    uint32_t battery_mv = 0U;
    result = battery_convert_adc_millivolts(average_adc_mv, &battery_mv);
    if (result != ESP_OK) {
        return result;
    }

    const battery_status_t measured_status = battery_classify_voltage(battery_mv);
    reading->voltage_mv = battery_mv;
    reading->status = battery_filter_update(&s_filter, measured_status);
    ESP_LOGI(TAG, "Battery %" PRIu32 " mV", battery_mv);
    return ESP_OK;
}
