#ifndef ONDA_POWER_H
#define ONDA_POWER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Retain the battery supply latch as early as possible during boot.
 *
 * This must be called before any non-essential startup work. It is harmless
 * while USB powers the board.
 */
esp_err_t power_init(void);

/**
 * Enter deep sleep while retaining the battery power latch. PWR wakes the
 * device. This function does not return after successful sleep entry.
 */
esp_err_t power_enter_sleep(void);

/**
 * Enter deep sleep without retaining battery power. This is reached only when
 * USB keeps the board powered after an otherwise immediate battery power-off.
 */
esp_err_t power_enter_off_sleep(void);

/**
 * Release the battery power latch. On battery-only power, the board turns off
 * immediately. When USB remains connected, the caller should enter sleep.
 */
esp_err_t power_release_battery_latch(void);

#ifdef __cplusplus
}
#endif

#endif
