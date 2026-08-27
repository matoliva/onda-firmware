#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "recording_naming.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORDING_METADATA_ID_LENGTH 36U
#define RECORDING_METADATA_PATH_MAX (RECORDING_NAMING_PATH_MAX + 16U)
#define RECORDING_METADATA_JSON_MAX 384U
#define RECORDING_METADATA_MEETING_ID_MAX 64U

typedef struct {
    char id[RECORDING_METADATA_ID_LENGTH + 1U];
    char final_wav_path[RECORDING_NAMING_PATH_MAX];
    char metadata_path[RECORDING_METADATA_PATH_MAX];
    char seed_path[RECORDING_METADATA_PATH_MAX];
    time_t created_at;
    char created_at_text[32U];
    bool has_created_at;
} recording_metadata_session_t;

typedef enum {
    RECORDING_METADATA_STATUS_PENDING,
    RECORDING_METADATA_STATUS_SYNCED,
} recording_metadata_status_t;

typedef struct {
    char id[RECORDING_METADATA_ID_LENGTH + 1U];
    char wav_path[RECORDING_NAMING_PATH_MAX];
    char metadata_path[RECORDING_METADATA_PATH_MAX];
    char created_at[32U];
    bool has_created_at;
    uint32_t duration_ms;
    size_t size_bytes;
    recording_metadata_status_t status;
    char meeting_id[RECORDING_METADATA_MEETING_ID_MAX + 1U];
    char synced_at[32U];
} recording_metadata_record_t;

typedef esp_err_t (*recording_metadata_pending_callback_t)(recording_metadata_record_t *record,
                                                            void *context);

/** Create and sync an immutable metadata seed before microphone capture. */
esp_err_t recording_metadata_prepare(const char *final_wav_path,
                                    time_t created_at,
                                    recording_metadata_session_t *session);

/** Atomically publish pending metadata after the matching WAV is finalized. */
esp_err_t recording_metadata_finalize(const recording_metadata_session_t *session,
                                     size_t pcm_data_bytes);

/** Remove a seed after an expected unsuccessful recording attempt. */
esp_err_t recording_metadata_cancel(const recording_metadata_session_t *session);

/** Recover seeds left by an interrupted finalization without touching legacy WAVs. */
esp_err_t recording_metadata_recover(void);

/** Enumerate valid local metadata records that are still awaiting BFF confirmation. */
esp_err_t recording_metadata_for_each_pending(recording_metadata_pending_callback_t callback,
                                              void *context);

/** Persist the closed WAV's total byte count before initiate; safe for legacy Phase 013 records. */
esp_err_t recording_metadata_ensure_size(recording_metadata_record_t *record);

/** Atomically persist the BFF-confirmed meeting and synchronization time. */
esp_err_t recording_metadata_mark_synced(recording_metadata_record_t *record,
                                         const char *meeting_id,
                                         const char *synced_at);

/** Format the public `rec_` + 128-bit lowercase hexadecimal ID. */
esp_err_t recording_metadata_make_id(const uint8_t random_bytes[16],
                                     char id[RECORDING_METADATA_ID_LENGTH + 1U]);

/** Format reliable local time as RFC 3339 with a numeric UTC offset. */
esp_err_t recording_metadata_format_created_at(time_t value, char *output, size_t output_size);

/** Convert 16 kHz 16-bit mono PCM byte length to milliseconds. */
esp_err_t recording_metadata_duration_ms(size_t pcm_data_bytes, uint32_t *duration_ms);

#ifdef __cplusplus
}
#endif
