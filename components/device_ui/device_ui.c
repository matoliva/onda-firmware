#include <string.h>

#include "device_ui.h"

bool device_ui_should_refresh(const device_ui_state_t *previous,
                              const device_ui_state_t *next)
{
    if (next == NULL) {
        return false;
    }

    if (previous == NULL) {
        return true;
    }

    return previous->primary_state != next->primary_state;
}

esp_err_t device_ui_describe(const device_ui_state_t *state,
                             display_screen_t *screen)
{
    if (state == NULL || screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(screen, 0, sizeof(*screen));
    screen->wifi_status = state->wifi_status == DEVICE_UI_WIFI_CONNECTED
                              ? DISPLAY_WIFI_CONNECTED
                              : DISPLAY_WIFI_OFFLINE;

    switch (state->battery_status) {
    case DEVICE_UI_BATTERY_UNKNOWN:
        screen->battery_status = DISPLAY_BATTERY_UNKNOWN;
        break;
    case DEVICE_UI_BATTERY_HIGH:
        screen->battery_status = DISPLAY_BATTERY_HIGH;
        break;
    case DEVICE_UI_BATTERY_MEDIUM:
        screen->battery_status = DISPLAY_BATTERY_MEDIUM;
        break;
    case DEVICE_UI_BATTERY_LOW:
        screen->battery_status = DISPLAY_BATTERY_LOW;
        break;
    case DEVICE_UI_BATTERY_CRITICAL:
        screen->battery_status = DISPLAY_BATTERY_CRITICAL;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    switch (state->primary_state) {
    case DEVICE_UI_PRIMARY_READY:
        screen->title = "Ready";
        screen->title_color = DISPLAY_COLOR_BLACK;
        break;
    case DEVICE_UI_PRIMARY_RECORDING:
        screen->title = "Recording";
        screen->title_color = DISPLAY_COLOR_RED;
        screen->show_recording_indicator = true;
        break;
    case DEVICE_UI_PRIMARY_WIFI_SETUP:
        if (state->proof_of_possession[0] == '\0') {
            return ESP_ERR_INVALID_STATE;
        }
        screen->title = "Wi-Fi setup";
        screen->title_color = DISPLAY_COLOR_BLACK;
        screen->detail_first_line = "Use your phone";
        screen->detail_second_line = "to configure Onda";
        screen->proof_of_possession = state->proof_of_possession;
        screen->accent_color = DISPLAY_COLOR_YELLOW;
        break;
    case DEVICE_UI_PRIMARY_ERROR:
        screen->title = "Something went";
        screen->title_color = DISPLAY_COLOR_BLACK;
        screen->detail_first_line = "wrong";
        screen->detail_second_line = "Try again";
        screen->accent_color = DISPLAY_COLOR_RED;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t device_ui_show(const device_ui_state_t *state)
{
    display_screen_t screen;
    const esp_err_t result = device_ui_describe(state, &screen);
    if (result != ESP_OK) {
        return result;
    }

    return display_show(&screen);
}
