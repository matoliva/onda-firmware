#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"

#define BYTES_PER_MEBIBYTE (1024U * 1024U)
#define EXPECTED_FLASH_SIZE_MB 8U
#define EXPECTED_PSRAM_SIZE_MB 8U
#define AUDIO_COMMAND_TOGGLE BIT0
#define AUDIO_DIAGNOSTIC_TASK_STACK_SIZE 4096U
#define AUDIO_DIAGNOSTIC_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define AUDIO_BUFFER_SAMPLES 512U
#define AUDIO_LEVEL_LOG_INTERVAL_US (1000LL * 1000LL)

static const char *TAG = "ONDA";
static TaskHandle_t s_audio_diagnostic_task;
static DMA_ATTR int16_t s_audio_buffer[AUDIO_BUFFER_SAMPLES];

static void onda_audio_diagnostic_task(void *context);
static uint32_t onda_peak_level(const int16_t *samples, size_t sample_count);

static void onda_handle_button_event(button_event_t event, void *context)
{
    (void)context;

    const char *button_name = event.id == BUTTON_ID_BOOT ? "BOOT" : "PWR";
    const char *event_name =
        event.type == BUTTON_EVENT_SHORT_PRESS ? "short press" : "long press";
    ESP_LOGI("ONDA_BUTTON", "%s %s", button_name, event_name);

    if (event.id == BUTTON_ID_BOOT && event.type == BUTTON_EVENT_SHORT_PRESS &&
        s_audio_diagnostic_task != NULL) {
        xTaskNotify(s_audio_diagnostic_task, AUDIO_COMMAND_TOGGLE, eSetBits);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Onda firmware");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const unsigned major_revision = chip_info.revision / 100;
    const unsigned minor_revision = chip_info.revision % 100;
    ESP_LOGI(TAG,
             "Target: %s, cores: %u, silicon revision: v%u.%u",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             major_revision,
             minor_revision);

    bool hardware_matches = true;

    if (chip_info.model != CHIP_ESP32S3) {
        ESP_LOGE(TAG, "Unexpected chip model; ESP32-S3 required");
        hardware_matches = false;
    }

    uint32_t flash_size_bytes = 0;
    const esp_err_t flash_result = esp_flash_get_size(NULL, &flash_size_bytes);
    if (flash_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read flash size: %s", esp_err_to_name(flash_result));
        hardware_matches = false;
    } else {
        const uint32_t flash_size_mb = flash_size_bytes / BYTES_PER_MEBIBYTE;
        ESP_LOGI(TAG, "Flash: %" PRIu32 " MB", flash_size_mb);

        if (flash_size_mb != EXPECTED_FLASH_SIZE_MB) {
            ESP_LOGE(TAG,
                     "Unexpected flash size; expected %u MB",
                     EXPECTED_FLASH_SIZE_MB);
            hardware_matches = false;
        }
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized");
        hardware_matches = false;
    } else {
        const size_t psram_size_mb = esp_psram_get_size() / BYTES_PER_MEBIBYTE;
        ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)psram_size_mb);

        if (psram_size_mb != EXPECTED_PSRAM_SIZE_MB) {
            ESP_LOGE(TAG,
                     "Unexpected PSRAM size; expected %u MB",
                     EXPECTED_PSRAM_SIZE_MB);
            hardware_matches = false;
        }
    }

    if (!hardware_matches) {
        ESP_LOGE(TAG, "Board bring-up checks failed");
        return;
    }

    ESP_LOGI(TAG, "Board bring-up complete");

    const esp_err_t display_init_result = display_init();
    if (display_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Display initialisation failed: %s", esp_err_to_name(display_init_result));
        return;
    }

    const esp_err_t display_ready_result = display_show_ready();
    if (display_ready_result != ESP_OK) {
        ESP_LOGE(TAG, "Display ready screen failed: %s", esp_err_to_name(display_ready_result));
        return;
    }

    const esp_err_t buttons_init_result = buttons_init(onda_handle_button_event, NULL);
    if (buttons_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Button input initialisation failed: %s", esp_err_to_name(buttons_init_result));
        return;
    }

    const esp_err_t audio_init_result = audio_init();
    if (audio_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Audio input unavailable: %s", esp_err_to_name(audio_init_result));
    } else if (xTaskCreate(onda_audio_diagnostic_task,
                           "onda_audio",
                           AUDIO_DIAGNOSTIC_TASK_STACK_SIZE,
                           NULL,
                           AUDIO_DIAGNOSTIC_TASK_PRIORITY,
                           &s_audio_diagnostic_task) != pdPASS) {
        s_audio_diagnostic_task = NULL;
        ESP_LOGE(TAG, "Failed to create audio diagnostic task");
    }

    ESP_LOGI(TAG, "Onda ready");
}

static void onda_audio_diagnostic_task(void *context)
{
    (void)context;

    for (;;) {
        uint32_t command = 0;
        xTaskNotifyWait(0, UINT32_MAX, &command, portMAX_DELAY);

        if ((command & AUDIO_COMMAND_TOGGLE) == 0U) {
            continue;
        }

        esp_err_t result = audio_start();
        if (result != ESP_OK) {
            ESP_LOGE("ONDA_AUDIO", "Capture start failed: %s", esp_err_to_name(result));
            continue;
        }

        int64_t next_log_at_us = esp_timer_get_time() + AUDIO_LEVEL_LOG_INTERVAL_US;
        uint32_t peak_level = 0;
        bool capture_requested = true;

        while (capture_requested) {
            size_t bytes_read = 0;
            result = audio_read(s_audio_buffer,
                                sizeof(s_audio_buffer),
                                &bytes_read,
                                pdMS_TO_TICKS(100));
            if (bytes_read > 0U) {
                const uint32_t buffer_peak =
                    onda_peak_level(s_audio_buffer, bytes_read / sizeof(s_audio_buffer[0]));
                if (buffer_peak > peak_level) {
                    peak_level = buffer_peak;
                }
            }
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
                ESP_LOGE("ONDA_AUDIO", "Capture read failed: %s", esp_err_to_name(result));
                capture_requested = false;
            }

            uint32_t pending_command = 0;
            if (xTaskNotifyWait(0, UINT32_MAX, &pending_command, 0) == pdTRUE &&
                (pending_command & AUDIO_COMMAND_TOGGLE) != 0U) {
                capture_requested = false;
            }

            if (!capture_requested) {
                break;
            }

            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_log_at_us) {
                ESP_LOGI("ONDA_AUDIO", "Level %" PRIu32, peak_level);
                peak_level = 0;
                next_log_at_us = now_us + AUDIO_LEVEL_LOG_INTERVAL_US;
            }
        }

        result = audio_stop();
        if (result != ESP_OK) {
            ESP_LOGE("ONDA_AUDIO", "Capture stop failed: %s", esp_err_to_name(result));
        }
    }
}

static uint32_t onda_peak_level(const int16_t *samples, size_t sample_count)
{
    uint32_t peak = 0;

    for (size_t index = 0; index < sample_count; ++index) {
        const int32_t sample = samples[index];
        if (sample == INT16_MIN) {
            continue;
        }
        const uint32_t magnitude = sample < 0 ? (uint32_t)(-sample) : (uint32_t)sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    return peak;
}
