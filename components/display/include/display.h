#ifndef ONDA_DISPLAY_H
#define ONDA_DISPLAY_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_COLOR_BLACK,
    DISPLAY_COLOR_WHITE,
    DISPLAY_COLOR_YELLOW,
    DISPLAY_COLOR_RED,
} display_color_t;

typedef enum {
    DISPLAY_WIFI_OFFLINE,
    DISPLAY_WIFI_CONNECTED,
} display_wifi_status_t;

typedef enum {
    DISPLAY_STORAGE_AVAILABLE,
    DISPLAY_STORAGE_UNAVAILABLE,
    DISPLAY_STORAGE_ERROR,
} display_storage_status_t;

typedef enum {
    DISPLAY_BATTERY_UNKNOWN,
    DISPLAY_BATTERY_HIGH,
    DISPLAY_BATTERY_MEDIUM,
    DISPLAY_BATTERY_LOW,
    DISPLAY_BATTERY_CRITICAL,
} display_battery_status_t;

/**
 * Content for Onda's fixed e-Paper layout. Text pointers must remain valid for
 * the duration of display_show().
 */
typedef struct {
    const char *title;
    const char *detail_first_line;
    const char *detail_second_line;
    const char *detail_third_line;
    const char *proof_of_possession;
    display_color_t title_color;
    display_color_t accent_color;
    display_wifi_status_t wifi_status;
    display_storage_status_t storage_status;
    display_battery_status_t battery_status;
    bool show_recording_indicator;
} display_screen_t;

/** Draw the fixed Onda layout, refresh it once, and sleep the panel. */
esp_err_t display_show(const display_screen_t *screen);

#ifdef __cplusplus
}
#endif

#endif
