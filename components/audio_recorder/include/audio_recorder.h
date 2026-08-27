#ifndef ONDA_AUDIO_RECORDER_H
#define ONDA_AUDIO_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "recording_naming.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_RECORDER_WAV_HEADER_SIZE 44U
#define AUDIO_RECORDER_MAX_DURATION_MS 7200000U
#define AUDIO_RECORDER_MAX_PCM_BYTES 230400000U
#define AUDIO_RECORDER_MAX_FILE_SIZE_BYTES (AUDIO_RECORDER_WAV_HEADER_SIZE + AUDIO_RECORDER_MAX_PCM_BYTES)

typedef enum {
    AUDIO_RECORDER_COMPLETION_SUCCESS,
    AUDIO_RECORDER_COMPLETION_STORAGE_ERROR,
    AUDIO_RECORDER_COMPLETION_AUDIO_ERROR,
} audio_recorder_completion_kind_t;

typedef struct {
    audio_recorder_completion_kind_t kind;
    esp_err_t error;
    size_t data_bytes;
    char final_path[RECORDING_NAMING_PATH_MAX];
} audio_recorder_completion_t;

typedef void (*audio_recorder_completion_callback_t)(const audio_recorder_completion_t *completion,
                                                      void *context);

typedef struct {
    char final_path[RECORDING_NAMING_PATH_MAX];
    char staging_path[RECORDING_NAMING_PATH_MAX];
    audio_recorder_completion_callback_t completion_callback;
    void *completion_context;
} audio_recorder_config_t;

/** Start asynchronous microphone-to-SD recording at prevalidated storage paths. */
esp_err_t audio_recorder_start(const audio_recorder_config_t *config);

/** Request finalization of the current recording. */
esp_err_t audio_recorder_stop(void);

/** Serialize a canonical 16 kHz, 16-bit mono PCM WAV header. */
void audio_recorder_make_wav_header(uint8_t header[AUDIO_RECORDER_WAV_HEADER_SIZE],
                                    uint32_t data_bytes);

/** Downmix complete interleaved stereo int16 frames into mono int16 samples. */
size_t audio_recorder_downmix_stereo(const int16_t *input,
                                     size_t input_bytes,
                                     int16_t *output,
                                     size_t output_capacity_bytes);

/** Return whether a closed WAV file contains exactly its header and PCM payload. */
bool audio_recorder_has_valid_file_size(size_t file_size, size_t data_bytes);

#ifdef __cplusplus
}
#endif

#endif
