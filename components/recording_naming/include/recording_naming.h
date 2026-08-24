#ifndef ONDA_RECORDING_NAMING_H
#define ONDA_RECORDING_NAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECORDING_NAMING_PATH_MAX 96U
#define RECORDING_NAMING_MAX_ORDINAL 9999U

/** Return whether a UTC clock value is trustworthy enough for recording paths. */
bool recording_naming_time_is_reliable(time_t value);

/**
 * Format one recording path under /sdcard/recordings.
 * Ordinal zero is the canonical timestamp name; later values add a -NNNN
 * suffix. An unreliable clock produces unknown-date/recording-NNNN.wav and
 * requires an ordinal from 1 through RECORDING_NAMING_MAX_ORDINAL.
 */
esp_err_t recording_naming_make_path(time_t utc_time,
                                     uint32_t ordinal,
                                     char *path,
                                     size_t path_size);

/** Format recorded PCM duration as MM:SS or HH:MM:SS. */
esp_err_t recording_naming_format_duration(size_t pcm_bytes,
                                           char *duration,
                                           size_t duration_size);

#ifdef __cplusplus
}
#endif

#endif
