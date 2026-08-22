#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"

#define BYTES_PER_MEBIBYTE (1024U * 1024U)
#define EXPECTED_FLASH_SIZE_MB 8U
#define EXPECTED_PSRAM_SIZE_MB 8U

#define ONDA_APPLICATION_COMMAND_QUEUE_LENGTH 8U
#define ONDA_DISPLAY_STATE_QUEUE_LENGTH 1U
#define ONDA_APPLICATION_TASK_STACK_SIZE 4096U
#define ONDA_APPLICATION_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_DISPLAY_TASK_STACK_SIZE 4096U
#define ONDA_DISPLAY_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_AUDIO_BUFFER_SAMPLES 512U
#define ONDA_AUDIO_READ_TIMEOUT_MS 100U

static const char *TAG = "ONDA";

typedef enum {
    ONDA_STATE_READY,
    ONDA_STATE_RECORDING,
    ONDA_STATE_ERROR,
} onda_state_t;

typedef enum {
    ONDA_COMMAND_TOGGLE_RECORDING,
    ONDA_COMMAND_DISPLAY_FAILURE,
} onda_command_t;

static QueueHandle_t s_application_command_queue;
static QueueHandle_t s_display_state_queue;
static DMA_ATTR int16_t s_audio_buffer[ONDA_AUDIO_BUFFER_SAMPLES];

static void onda_application_task(void *context);
static void onda_display_task(void *context);
static void onda_handle_button_event(button_event_t event, void *context);
static void onda_handle_command(onda_state_t *state, onda_command_t command);
static void onda_start_recording(onda_state_t *state);
static void onda_stop_recording(onda_state_t *state);
static void onda_enter_error(onda_state_t *state,
                             const char *operation,
                             esp_err_t error,
                             bool stop_audio);
static esp_err_t onda_schedule_display_state(onda_state_t state);
static esp_err_t onda_render_state(onda_state_t state);
static const char *onda_state_name(onda_state_t state);

static void onda_handle_button_event(button_event_t event, void *context)
{
    (void)context;

    const char *button_name = event.id == BUTTON_ID_BOOT ? "BOOT" : "PWR";
    const char *event_name =
        event.type == BUTTON_EVENT_SHORT_PRESS ? "short press" : "long press";
    ESP_LOGI("ONDA_BUTTON", "%s %s", button_name, event_name);

    if (event.id != BUTTON_ID_BOOT || event.type != BUTTON_EVENT_SHORT_PRESS) {
        return;
    }

    if (s_application_command_queue == NULL) {
        ESP_LOGW(TAG, "Ignoring recording toggle before application startup");
        return;
    }

    const onda_command_t command = ONDA_COMMAND_TOGGLE_RECORDING;
    if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
        ESP_LOGE(TAG, "Recording command queue is full");
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

    esp_err_t result = onda_render_state(ONDA_STATE_READY);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Initial ready screen failed: %s", esp_err_to_name(result));
        return;
    }

    result = audio_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Audio input unavailable: %s", esp_err_to_name(result));
        const esp_err_t display_result = onda_render_state(ONDA_STATE_ERROR);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Error screen failed: %s", esp_err_to_name(display_result));
        }
        return;
    }

    s_application_command_queue =
        xQueueCreate(ONDA_APPLICATION_COMMAND_QUEUE_LENGTH, sizeof(onda_command_t));
    s_display_state_queue = xQueueCreate(ONDA_DISPLAY_STATE_QUEUE_LENGTH, sizeof(onda_state_t));
    if (s_application_command_queue == NULL || s_display_state_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create application queues");
        const esp_err_t display_result = onda_render_state(ONDA_STATE_ERROR);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Error screen failed: %s", esp_err_to_name(display_result));
        }
        return;
    }

    if (xTaskCreate(onda_display_task,
                    "onda_display",
                    ONDA_DISPLAY_TASK_STACK_SIZE,
                    NULL,
                    ONDA_DISPLAY_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        const esp_err_t display_result = onda_render_state(ONDA_STATE_ERROR);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Error screen failed: %s", esp_err_to_name(display_result));
        }
        return;
    }

    if (xTaskCreate(onda_application_task,
                    "onda_app",
                    ONDA_APPLICATION_TASK_STACK_SIZE,
                    NULL,
                    ONDA_APPLICATION_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create application task");
        const esp_err_t display_result = onda_schedule_display_state(ONDA_STATE_ERROR);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Error display request failed: %s", esp_err_to_name(display_result));
        }
        return;
    }

    result = buttons_init(onda_handle_button_event, NULL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Button input initialisation failed: %s", esp_err_to_name(result));
        const esp_err_t display_result = onda_schedule_display_state(ONDA_STATE_ERROR);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Error display request failed: %s", esp_err_to_name(display_result));
        }
        return;
    }

    ESP_LOGI(TAG, "Onda ready");
}

static void onda_application_task(void *context)
{
    (void)context;

    onda_state_t state = ONDA_STATE_READY;

    for (;;) {
        onda_command_t command;

        if (state == ONDA_STATE_RECORDING) {
            size_t bytes_read = 0;
            const esp_err_t result = audio_read(s_audio_buffer,
                                                sizeof(s_audio_buffer),
                                                &bytes_read,
                                                pdMS_TO_TICKS(ONDA_AUDIO_READ_TIMEOUT_MS));
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
                onda_enter_error(&state, "Recording read", result, true);
            }
        }

        const TickType_t wait_time =
            state == ONDA_STATE_RECORDING ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_application_command_queue, &command, wait_time) == pdTRUE) {
            onda_handle_command(&state, command);
        }
    }
}

static void onda_display_task(void *context)
{
    (void)context;

    for (;;) {
        onda_state_t state;
        xQueueReceive(s_display_state_queue, &state, portMAX_DELAY);

        const esp_err_t result = onda_render_state(state);
        if (result == ESP_OK) {
            continue;
        }

        ESP_LOGE(TAG,
                 "Failed to render %s state: %s",
                 onda_state_name(state),
                 esp_err_to_name(result));
        if (state == ONDA_STATE_ERROR || s_application_command_queue == NULL) {
            continue;
        }

        const onda_command_t command = ONDA_COMMAND_DISPLAY_FAILURE;
        if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to report display failure");
        }
    }
}

static void onda_handle_command(onda_state_t *state, onda_command_t command)
{
    if (state == NULL) {
        ESP_LOGE(TAG, "Invalid recording state context");
        return;
    }

    switch (command) {
    case ONDA_COMMAND_TOGGLE_RECORDING:
        switch (*state) {
        case ONDA_STATE_READY:
            onda_start_recording(state);
            break;
        case ONDA_STATE_RECORDING:
            onda_stop_recording(state);
            break;
        case ONDA_STATE_ERROR:
            ESP_LOGW(TAG, "Ignoring recording toggle while in ERROR state");
            break;
        default:
            ESP_LOGE(TAG, "Invalid application state");
            onda_enter_error(state, "State transition", ESP_ERR_INVALID_STATE, false);
            break;
        }
        break;
    case ONDA_COMMAND_DISPLAY_FAILURE:
        if (*state != ONDA_STATE_ERROR) {
            onda_enter_error(state,
                             "Display update",
                             ESP_FAIL,
                             *state == ONDA_STATE_RECORDING);
        }
        break;
    default:
        ESP_LOGE(TAG, "Invalid application command");
        onda_enter_error(state, "Command handling", ESP_ERR_INVALID_ARG, false);
        break;
    }
}

static void onda_start_recording(onda_state_t *state)
{
    ESP_LOGI(TAG, "Transition READY -> RECORDING");

    const esp_err_t result = audio_start();
    if (result != ESP_OK) {
        onda_enter_error(state, "Recording start", result, false);
        return;
    }

    *state = ONDA_STATE_RECORDING;
    const esp_err_t display_result = onda_schedule_display_state(*state);
    if (display_result != ESP_OK) {
        onda_enter_error(state, "Recording display request", display_result, true);
        return;
    }

    ESP_LOGI(TAG, "State: %s", onda_state_name(*state));
}

static void onda_stop_recording(onda_state_t *state)
{
    ESP_LOGI(TAG, "Transition RECORDING -> READY");

    const esp_err_t result = audio_stop();
    if (result != ESP_OK) {
        onda_enter_error(state, "Recording stop", result, true);
        return;
    }

    *state = ONDA_STATE_READY;
    const esp_err_t display_result = onda_schedule_display_state(*state);
    if (display_result != ESP_OK) {
        onda_enter_error(state, "Ready display request", display_result, false);
        return;
    }

    ESP_LOGI(TAG, "State: %s", onda_state_name(*state));
}

static void onda_enter_error(onda_state_t *state,
                             const char *operation,
                             esp_err_t error,
                             bool stop_audio)
{
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));

    if (stop_audio) {
        const esp_err_t stop_result = audio_stop();
        if (stop_result != ESP_OK) {
            ESP_LOGE(TAG, "Audio cleanup failed: %s", esp_err_to_name(stop_result));
        }
    }

    *state = ONDA_STATE_ERROR;
    const esp_err_t display_result = onda_schedule_display_state(*state);
    if (display_result != ESP_OK) {
        ESP_LOGE(TAG, "Error display request failed: %s", esp_err_to_name(display_result));
    }
    ESP_LOGE(TAG, "State: %s", onda_state_name(*state));
}

static esp_err_t onda_schedule_display_state(onda_state_t state)
{
    if (s_display_state_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueOverwrite(s_display_state_queue, &state) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t onda_render_state(onda_state_t state)
{
    esp_err_t result = display_init();
    if (result != ESP_OK) {
        return result;
    }

    switch (state) {
    case ONDA_STATE_READY:
        return display_show_ready();
    case ONDA_STATE_RECORDING:
        return display_show_recording();
    case ONDA_STATE_ERROR:
        return display_show_error();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static const char *onda_state_name(onda_state_t state)
{
    switch (state) {
    case ONDA_STATE_READY:
        return "READY";
    case ONDA_STATE_RECORDING:
        return "RECORDING";
    case ONDA_STATE_ERROR:
        return "ERROR";
    default:
        return "INVALID";
    }
}
