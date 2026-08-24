#ifndef ONDA_AUDIO_RECORDER_H
#define ONDA_AUDIO_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_RECORDER_WAV_HEADER_SIZE 44U

typedef void (*audio_recorder_completion_callback_t)(esp_err_t result,
                                                      size_t data_bytes,
                                                      void *context);

/** Start asynchronous microphone-to-SD recording at /sdcard/test.wav. */
esp_err_t audio_recorder_start(audio_recorder_completion_callback_t callback, void *context);

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

#ifdef __cplusplus
}
#endif

#endif
