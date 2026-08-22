#ifndef ONDA_DISPLAY_H
#define ONDA_DISPLAY_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the board's e-Paper transport and wake the panel.
 *
 * The function is safe to call again after a previous display operation put
 * the panel to sleep.
 */
esp_err_t display_init(void);

/** Draw the Onda ready screen, refresh it once, and sleep the panel. */
esp_err_t display_show_ready(bool wifi_connected);

/** Draw the static Onda recording screen, refresh it once, and sleep the panel. */
esp_err_t display_show_recording(void);

/** Draw the static Onda error screen, refresh it once, and sleep the panel. */
esp_err_t display_show_error(void);

/** Draw the BLE Wi-Fi setup screen with the required proof-of-possession. */
esp_err_t display_show_wifi_setup(const char *proof_of_possession);

/** Draw Wi-Fi connection progress. */
esp_err_t display_show_wifi_connecting(void);

/** Draw the offline recording-available status. */
esp_err_t display_show_offline(void);

/** Draw a recoverable Wi-Fi error screen. */
esp_err_t display_show_wifi_error(void);

#ifdef __cplusplus
}
#endif

#endif
