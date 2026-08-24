#ifndef ONDA_STORAGE_H
#define ONDA_STORAGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the onboard microSD card at /sdcard. */
esp_err_t storage_init(void);

/**
 * Verify the mounted card by writing and reading Onda's fixed diagnostic file.
 * The caller must successfully call storage_init() first.
 */
esp_err_t storage_verify(void);

/** Unmount the mounted microSD card and release its filesystem resources. */
esp_err_t storage_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
