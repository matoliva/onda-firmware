#ifndef ONDA_DISPLAY_H
#define ONDA_DISPLAY_H

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

/** Draw the static Onda ready screen, refresh it once, and sleep the panel. */
esp_err_t display_show_ready(void);

/** Draw the static Onda recording screen, refresh it once, and sleep the panel. */
esp_err_t display_show_recording(void);

/** Draw the static Onda error screen, refresh it once, and sleep the panel. */
esp_err_t display_show_error(void);

#ifdef __cplusplus
}
#endif

#endif
