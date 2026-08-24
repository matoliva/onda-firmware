#include <stdio.h>
#include <string.h>

#include "recording_naming.h"

#define RECORDING_NAMING_EARLIEST_UTC 1704067200LL /* 2024-01-01T00:00:00Z */
#define RECORDING_NAMING_LATEST_UTC 4102444800LL   /* 2100-01-01T00:00:00Z */
#define RECORDING_NAMING_BYTES_PER_SECOND 32000U

bool recording_naming_time_is_reliable(time_t value)
{
    return (int64_t)value >= RECORDING_NAMING_EARLIEST_UTC &&
           (int64_t)value < RECORDING_NAMING_LATEST_UTC;
}

esp_err_t recording_naming_make_path(time_t utc_time,
                                     uint32_t ordinal,
                                     char *path,
                                     size_t path_size)
{
    if (path == NULL || path_size == 0U || ordinal > RECORDING_NAMING_MAX_ORDINAL) {
        return ESP_ERR_INVALID_ARG;
    }

    int written;
    if (!recording_naming_time_is_reliable(utc_time)) {
        if (ordinal == 0U) {
            return ESP_ERR_INVALID_ARG;
        }
        written = snprintf(path,
                           path_size,
                           "/sdcard/recordings/unknown-date/recording-%04u.wav",
                           (unsigned)ordinal);
    } else {
        struct tm timestamp;
        if (gmtime_r(&utc_time, &timestamp) == NULL) {
            return ESP_FAIL;
        }

        char date[11U];
        char clock[9U];
        if (strftime(date, sizeof(date), "%Y-%m-%d", &timestamp) != 10U ||
            strftime(clock, sizeof(clock), "%H-%M-%S", &timestamp) != 8U) {
            return ESP_FAIL;
        }
        if (ordinal == 0U) {
            written = snprintf(path,
                               path_size,
                               "/sdcard/recordings/%s/%s.wav",
                               date,
                               clock);
        } else {
            written = snprintf(path,
                               path_size,
                               "/sdcard/recordings/%s/%s-%04u.wav",
                               date,
                               clock,
                               (unsigned)ordinal);
        }
    }

    if (written < 0 || (size_t)written >= path_size) {
        path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t recording_naming_format_duration(size_t pcm_bytes,
                                           char *duration,
                                           size_t duration_size)
{
    if (duration == NULL || duration_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t total_seconds = pcm_bytes / RECORDING_NAMING_BYTES_PER_SECOND;
    const size_t hours = total_seconds / 3600U;
    const size_t minutes = (total_seconds / 60U) % 60U;
    const size_t seconds = total_seconds % 60U;
    const int written = hours == 0U
                            ? snprintf(duration, duration_size, "%02u:%02u", (unsigned)minutes,
                                       (unsigned)seconds)
                            : snprintf(duration,
                                       duration_size,
                                       "%02u:%02u:%02u",
                                       (unsigned)hours,
                                       (unsigned)minutes,
                                       (unsigned)seconds);
    if (written < 0 || (size_t)written >= duration_size) {
        duration[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
