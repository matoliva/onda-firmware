#ifndef ONDA_WIFI_H
#define ONDA_WIFI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_UNCONFIGURED,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_OFFLINE,
    WIFI_STATE_ERROR,
} wifi_state_t;

typedef void (*wifi_state_callback_t)(wifi_state_t state,
                                      const char *proof_of_possession,
                                      void *context);

/**
 * Initialise Wi-Fi station mode and start either automatic connection or BLE
 * provisioning. State changes are delivered asynchronously through callback.
 */
esp_err_t wifi_start(wifi_state_callback_t callback, void *context);

/**
 * Clear stored Wi-Fi station credentials and enter BLE provisioning again.
 * The device proof-of-possession is retained.
 */
esp_err_t wifi_request_reprovision(void);

/** Stop Wi-Fi/BLE provisioning activity before entering deep sleep. */
esp_err_t wifi_stop(void);

#ifdef __cplusplus
}
#endif

#endif
