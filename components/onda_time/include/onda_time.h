#ifndef ONDA_TIME_H
#define ONDA_TIME_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ONDA_TIME_UNSYNCED,
    ONDA_TIME_SYNCING,
    ONDA_TIME_SYNCED,
    ONDA_TIME_FAILED,
} onda_time_state_t;

/** Configure the device's local timezone and time-service resources. */
esp_err_t onda_time_init(void);

/** Begin or refresh SNTP after the application has confirmed Wi-Fi connectivity. */
esp_err_t onda_time_start_sync(void);

/** Return the current time-service state. */
onda_time_state_t onda_time_get_state(void);

/** Return whether this boot has obtained a plausible SNTP timestamp. */
bool onda_time_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif
