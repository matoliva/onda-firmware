#ifndef ONDA_API_H
#define ONDA_API_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONDA_API_DEVICE_ID_MAX_LENGTH 64U
#define ONDA_API_DEVICE_NAME_MAX_LENGTH 64U
#define ONDA_API_SERVER_TIME_MAX_LENGTH 32U

typedef enum {
    ONDA_API_NOT_CHECKED,
    ONDA_API_CHECKING,
    ONDA_API_AUTHENTICATED,
    ONDA_API_UNAUTHORIZED,
    ONDA_API_ERROR,
} onda_api_state_t;

typedef struct {
    char device_id[ONDA_API_DEVICE_ID_MAX_LENGTH + 1U];
    char name[ONDA_API_DEVICE_NAME_MAX_LENGTH + 1U];
    char server_time[ONDA_API_SERVER_TIME_MAX_LENGTH + 1U];
} onda_api_device_identity_t;

/** Initialise the device API worker. This does not issue a network request. */
esp_err_t onda_api_init(void);

/** Queue the single permitted identity request for this boot. */
esp_err_t onda_api_get_device_identity_async(void);

/** Return the latest backend authentication state, independently of Wi-Fi. */
onda_api_state_t onda_api_get_state(void);

/** Parse the required identity fields from a bounded successful-response body. */
esp_err_t onda_api_parse_device_identity(const char *response,
                                         size_t response_length,
                                         onda_api_device_identity_t *identity);

/** Resolve the exposed state from a completed HTTP request result. */
onda_api_state_t onda_api_resolve_state(esp_err_t request_result,
                                        int http_status,
                                        bool response_is_valid);

#ifdef __cplusplus
}
#endif

#endif
