#ifndef ONDA_RECORDING_SYNC_H
#define ONDA_RECORDING_SYNC_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECORDING_SYNC_RESULT_SYNCED,
    RECORDING_SYNC_RESULT_PARTIAL,
    RECORDING_SYNC_RESULT_UP_TO_DATE,
    RECORDING_SYNC_RESULT_CONNECTION_FAILURE,
    RECORDING_SYNC_RESULT_AUTH_FAILURE,
} recording_sync_result_kind_t;

typedef struct {
    recording_sync_result_kind_t kind;
    size_t uploaded_count;
    size_t pending_count;
} recording_sync_completion_t;

typedef void (*recording_sync_completion_callback_t)(const recording_sync_completion_t *completion,
                                                     void *context);

/** Initialise the single manual-sync worker. */
esp_err_t recording_sync_init(void);

/** Count valid metadata sidecars that remain pending. */
esp_err_t recording_sync_count_pending(size_t *count);

/** Start one sequential sync run; only one run may be active. */
esp_err_t recording_sync_start(recording_sync_completion_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif

#endif
