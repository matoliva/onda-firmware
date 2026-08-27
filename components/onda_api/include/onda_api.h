#ifndef ONDA_API_H
#define ONDA_API_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "recording_metadata.h"

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

#define ONDA_API_MEETING_ID_MAX_LENGTH RECORDING_METADATA_MEETING_ID_MAX
#define ONDA_API_SYNCED_AT_MAX_LENGTH 31U

typedef enum {
    ONDA_API_SYNC_SUCCESS,
    ONDA_API_SYNC_RECORD_FAILURE,
    ONDA_API_SYNC_CONNECTION_FAILURE,
    ONDA_API_SYNC_AUTH_FAILURE,
} onda_api_sync_outcome_t;

typedef struct {
    onda_api_sync_outcome_t outcome;
    char meeting_id[ONDA_API_MEETING_ID_MAX_LENGTH + 1U];
    char synced_at[ONDA_API_SYNCED_AT_MAX_LENGTH + 1U];
} onda_api_sync_result_t;

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

/** Execute one BFF initiate → signed Blob PUT → complete transfer synchronously. */
esp_err_t onda_api_sync_recording(const recording_metadata_record_t *record,
                                  onda_api_sync_result_t *result);

/** Parse a bounded BFF uploaded response before committing local metadata. */
esp_err_t onda_api_parse_sync_success(const char *response,
                                      size_t response_length,
                                      const char *recording_id,
                                      onda_api_sync_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
