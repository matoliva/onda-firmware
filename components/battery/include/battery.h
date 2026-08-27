#ifndef ONDA_BATTERY_H
#define ONDA_BATTERY_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BATTERY_STATUS_UNKNOWN,
    BATTERY_STATUS_HIGH,
    BATTERY_STATUS_MEDIUM,
    BATTERY_STATUS_LOW,
    BATTERY_STATUS_CRITICAL,
} battery_status_t;

typedef struct {
    battery_status_t stable_status;
    battery_status_t candidate_status;
    uint8_t candidate_count;
} battery_filter_t;

typedef struct {
    uint32_t voltage_mv;
    battery_status_t status;
} battery_reading_t;

/** Initialise the calibrated ADC1 channel connected to the battery divider. */
esp_err_t battery_init(void);

/**
 * Take one averaged battery reading. The first valid value establishes the
 * level; later level changes require two consecutive valid samples.
 */
esp_err_t battery_sample(battery_reading_t *reading);

/** Convert calibrated ADC millivolts to battery millivolts through the divider. */
esp_err_t battery_convert_adc_millivolts(uint32_t adc_mv, uint32_t *battery_mv);

/** Average calibrated millivolt samples without allocating memory. */
esp_err_t battery_average_millivolts(const int *samples, size_t count, uint32_t *average_mv);

/** Classify a battery voltage using Onda's coarse, initial voltage bands. */
battery_status_t battery_classify_voltage(uint32_t voltage_mv);

/** Initialise the stable-level filter with no established reading. */
void battery_filter_init(battery_filter_t *filter);

/** Apply one classified reading and return the stable status. */
battery_status_t battery_filter_update(battery_filter_t *filter, battery_status_t status);

#ifdef __cplusplus
}
#endif

#endif
