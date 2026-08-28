#ifndef ONDA_DEVICE_UI_H
#define ONDA_DEVICE_UI_H

#include <stdbool.h>

#include "display.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_UI_PROOF_OF_POSSESSION_LENGTH 8U

typedef enum {
    DEVICE_UI_PRIMARY_READY,
    DEVICE_UI_PRIMARY_RECORDING,
    DEVICE_UI_PRIMARY_FINALIZING,
    DEVICE_UI_PRIMARY_SAVED,
    DEVICE_UI_PRIMARY_SYNCING,
    DEVICE_UI_PRIMARY_SYNCED,
    DEVICE_UI_PRIMARY_SYNC_PARTIAL,
    DEVICE_UI_PRIMARY_UP_TO_DATE,
    DEVICE_UI_PRIMARY_NO_WIFI,
    DEVICE_UI_PRIMARY_SYNC_AUTH_ERROR,
    DEVICE_UI_PRIMARY_SYNC_CONNECTION_ERROR,
    DEVICE_UI_PRIMARY_WIFI_SETUP,
    DEVICE_UI_PRIMARY_SLEEPING,
    DEVICE_UI_PRIMARY_POWERING_OFF,
    DEVICE_UI_PRIMARY_STORAGE_ERROR,
    DEVICE_UI_PRIMARY_AUDIO_ERROR,
    DEVICE_UI_PRIMARY_ERROR,
} device_ui_primary_state_t;

typedef enum {
    DEVICE_UI_WIFI_OFFLINE,
    DEVICE_UI_WIFI_CONNECTED,
} device_ui_wifi_status_t;

typedef enum {
    DEVICE_UI_STORAGE_AVAILABLE,
    DEVICE_UI_STORAGE_UNAVAILABLE,
    DEVICE_UI_STORAGE_ERROR,
} device_ui_storage_status_t;

typedef enum {
    DEVICE_UI_BATTERY_UNKNOWN,
    DEVICE_UI_BATTERY_HIGH,
    DEVICE_UI_BATTERY_MEDIUM,
    DEVICE_UI_BATTERY_LOW,
    DEVICE_UI_BATTERY_CRITICAL,
} device_ui_battery_status_t;

#define DEVICE_UI_SAVED_DURATION_LENGTH 8U
#define DEVICE_UI_SAVED_FILENAME_LENGTH 40U
#define DEVICE_UI_SYNC_DETAIL_LENGTH 24U

typedef struct {
    device_ui_primary_state_t primary_state;
    device_ui_wifi_status_t wifi_status;
    device_ui_storage_status_t storage_status;
    device_ui_battery_status_t battery_status;
    char proof_of_possession[DEVICE_UI_PROOF_OF_POSSESSION_LENGTH + 1U];
    char saved_duration[DEVICE_UI_SAVED_DURATION_LENGTH + 1U];
    char saved_filename[DEVICE_UI_SAVED_FILENAME_LENGTH + 1U];
    char sync_detail_first_line[DEVICE_UI_SYNC_DETAIL_LENGTH + 1U];
    char sync_detail_second_line[DEVICE_UI_SYNC_DETAIL_LENGTH + 1U];
} device_ui_state_t;

/**
 * Return whether the next UI state needs a physical e-Paper refresh.
 * Status-bar-only changes are retained by the application and are rendered on
 * the next primary-state transition.
 */
bool device_ui_should_refresh(const device_ui_state_t *previous,
                              const device_ui_state_t *next);

/** Build the generic display screen associated with a declarative UI state. */
esp_err_t device_ui_describe(const device_ui_state_t *state,
                             display_screen_t *screen);

/** Render a declarative UI state through the display abstraction. */
esp_err_t device_ui_show(const device_ui_state_t *state);

#ifdef __cplusplus
}
#endif

#endif
