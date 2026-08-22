#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** Initialise the board microphone path without starting capture. */
esp_err_t audio_init(void);

/** Enable ES8311 microphone capture. */
esp_err_t audio_start(void);

/** Read PCM capture data into a caller-provided bounded buffer. */
esp_err_t audio_read(void *buffer, size_t buffer_size, size_t *bytes_read, TickType_t timeout);

/** Stop microphone capture while retaining the initialized audio component. */
esp_err_t audio_stop(void);
