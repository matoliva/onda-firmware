#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
#include "wifi.h"

#define BYTES_PER_MEBIBYTE (1024U * 1024U)
#define EXPECTED_FLASH_SIZE_MB 8U
#define EXPECTED_PSRAM_SIZE_MB 8U

#define ONDA_APPLICATION_COMMAND_QUEUE_LENGTH 12U
#define ONDA_DISPLAY_STATE_QUEUE_LENGTH 1U
#define ONDA_APPLICATION_TASK_STACK_SIZE 4096U
#define ONDA_APPLICATION_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_DISPLAY_TASK_STACK_SIZE 4096U
#define ONDA_DISPLAY_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_AUDIO_BUFFER_SAMPLES 512U
#define ONDA_AUDIO_READ_TIMEOUT_MS 100U
#define ONDA_WIFI_POP_LENGTH 8U

static const char *TAG = "ONDA";

typedef enum {
    ONDA_STATE_READY,
    ONDA_STATE_RECORDING,
    ONDA_STATE_ERROR,
} onda_state_t;

typedef enum {
    ONDA_COMMAND_TOGGLE_RECORDING,
    ONDA_COMMAND_RESET_WIFI,
    ONDA_COMMAND_WIFI_STATE_CHANGED,
    ONDA_COMMAND_DISPLAY_FAILURE,
} onda_command_t;

typedef enum {
    ONDA_DISPLAY_READY,
    ONDA_DISPLAY_RECORDING,
    ONDA_DISPLAY_ERROR,
    ONDA_DISPLAY_WIFI_SETUP,
    ONDA_DISPLAY_WIFI_CONNECTING,
    ONDA_DISPLAY_OFFLINE,
    ONDA_DISPLAY_WIFI_ERROR,
} onda_display_state_t;

typedef struct {
    onda_command_t command;
    wifi_state_t wifi_state;
    char proof_of_possession[ONDA_WIFI_POP_LENGTH + 1U];
} onda_application_command_t;

typedef struct {
    onda_display_state_t state;
    char proof_of_possession[ONDA_WIFI_POP_LENGTH + 1U];
} onda_display_request_t;

static QueueHandle_t s_application_command_queue;
static QueueHandle_t s_display_state_queue;
static DMA_ATTR int16_t s_audio_buffer[ONDA_AUDIO_BUFFER_SAMPLES];

static void onda_application_task(void *context);
static void onda_display_task(void *context);
static void onda_handle_button_event(button_event_t event, void *context);
static void onda_handle_wifi_state(wifi_state_t state,
                                   const char *proof_of_possession,
                                   void *context);
static void onda_handle_command(onda_state_t *recording_state,
                                wifi_state_t *network_state,
                                char *proof_of_possession,
                                const onda_application_command_t *command);
static void onda_start_recording(onda_state_t *state,
                                 wifi_state_t network_state,
                                 const char *proof_of_possession);
static void onda_stop_recording(onda_state_t *state,
                                wifi_state_t network_state,
                                const char *proof_of_possession);
static void onda_enter_error(onda_state_t *state,
                             const char *operation,
                             esp_err_t error,
                             bool stop_audio);
static esp_err_t onda_schedule_display(onda_state_t recording_state,
                                       wifi_state_t network_state,
                                       const char *proof_of_possession);
static esp_err_t onda_render_display(const onda_display_request_t *request);
static onda_display_state_t onda_select_display_state(onda_state_t recording_state,
                                                      wifi_state_t network_state);
static const char *onda_state_name(onda_state_t state);
static const char *onda_wifi_state_name(wifi_state_t state);

static void onda_handle_button_event(button_event_t event, void *context)
{
    (void)context;

    const char *button_name = event.id == BUTTON_ID_BOOT ? "BOOT" : "PWR";
    const char *event_name;
    switch (event.type) {
    case BUTTON_EVENT_SHORT_PRESS:
        event_name = "short press";
        break;
    case BUTTON_EVENT_LONG_PRESS:
        event_name = "long press";
        break;
    case BUTTON_EVENT_VERY_LONG_PRESS:
        event_name = "very long press";
        break;
    default:
        event_name = "unknown press";
        break;
    }
    ESP_LOGI("ONDA_BUTTON", "%s %s", button_name, event_name);

    if (s_application_command_queue == NULL) {
        ESP_LOGW(TAG, "Ignoring button input before application startup");
        return;
    }

    onda_application_command_t command = {0};
    if (event.id == BUTTON_ID_BOOT && event.type == BUTTON_EVENT_SHORT_PRESS) {
        command.command = ONDA_COMMAND_TOGGLE_RECORDING;
    } else if (event.id == BUTTON_ID_PWR && event.type == BUTTON_EVENT_VERY_LONG_PRESS) {
        command.command = ONDA_COMMAND_RESET_WIFI;
    } else {
        return;
    }

    if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
        ESP_LOGE(TAG, "Application command queue is full");
    }
}

static void onda_handle_wifi_state(wifi_state_t state,
                                   const char *proof_of_possession,
                                   void *context)
{
    (void)context;

    if (s_application_command_queue == NULL) {
        ESP_LOGW(TAG, "Ignoring Wi-Fi state before application startup");
        return;
    }

    onda_application_command_t command = {
        .command = ONDA_COMMAND_WIFI_STATE_CHANGED,
        .wifi_state = state,
    };
    if (proof_of_possession != NULL) {
        strncpy(command.proof_of_possession,
                proof_of_possession,
                sizeof(command.proof_of_possession) - 1U);
    }

    if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi state command queue is full");
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
            ESP_LOGE(TAG, "Unexpected flash size; expected %u MB", EXPECTED_FLASH_SIZE_MB);
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
            ESP_LOGE(TAG, "Unexpected PSRAM size; expected %u MB", EXPECTED_PSRAM_SIZE_MB);
            hardware_matches = false;
        }
    }

    if (!hardware_matches) {
        ESP_LOGE(TAG, "Board bring-up checks failed");
        return;
    }

    const esp_err_t audio_result = audio_init();
    if (audio_result != ESP_OK) {
        ESP_LOGE(TAG, "Audio input unavailable: %s", esp_err_to_name(audio_result));
        return;
    }

    s_application_command_queue =
        xQueueCreate(ONDA_APPLICATION_COMMAND_QUEUE_LENGTH, sizeof(onda_application_command_t));
    s_display_state_queue = xQueueCreate(ONDA_DISPLAY_STATE_QUEUE_LENGTH, sizeof(onda_display_request_t));
    if (s_application_command_queue == NULL || s_display_state_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create application queues");
        return;
    }

    if (xTaskCreate(onda_display_task,
                    "onda_display",
                    ONDA_DISPLAY_TASK_STACK_SIZE,
                    NULL,
                    ONDA_DISPLAY_TASK_PRIORITY,
                    NULL) != pdPASS ||
        xTaskCreate(onda_application_task,
                    "onda_app",
                    ONDA_APPLICATION_TASK_STACK_SIZE,
                    NULL,
                    ONDA_APPLICATION_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create application task");
        return;
    }

    const esp_err_t buttons_result = buttons_init(onda_handle_button_event, NULL);
    if (buttons_result != ESP_OK) {
        ESP_LOGE(TAG, "Button input initialisation failed: %s", esp_err_to_name(buttons_result));
        return;
    }

    const esp_err_t wifi_result = wifi_start(onda_handle_wifi_state, NULL);
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable: %s", esp_err_to_name(wifi_result));
        const esp_err_t display_result =
            onda_schedule_display(ONDA_STATE_READY, WIFI_STATE_ERROR, NULL);
        if (display_result != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi error display request failed: %s", esp_err_to_name(display_result));
        }
    }
    ESP_LOGI(TAG, "Onda ready");
}

static void onda_application_task(void *context)
{
    (void)context;

    onda_state_t recording_state = ONDA_STATE_READY;
    wifi_state_t network_state = WIFI_STATE_UNCONFIGURED;
    char proof_of_possession[ONDA_WIFI_POP_LENGTH + 1U] = {0};

    for (;;) {
        onda_application_command_t command;

        if (recording_state == ONDA_STATE_RECORDING) {
            size_t bytes_read = 0;
            const esp_err_t result = audio_read(s_audio_buffer,
                                                sizeof(s_audio_buffer),
                                                &bytes_read,
                                                pdMS_TO_TICKS(ONDA_AUDIO_READ_TIMEOUT_MS));
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
                onda_enter_error(&recording_state, "Recording read", result, true);
            }
        }

        const TickType_t wait_time =
            recording_state == ONDA_STATE_RECORDING ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_application_command_queue, &command, wait_time) == pdTRUE) {
            onda_handle_command(&recording_state,
                                &network_state,
                                proof_of_possession,
                                &command);
        }
    }
}

static void onda_display_task(void *context)
{
    (void)context;

    for (;;) {
        onda_display_request_t request;
        xQueueReceive(s_display_state_queue, &request, portMAX_DELAY);

        const esp_err_t result = onda_render_display(&request);
        if (result == ESP_OK) {
            continue;
        }

        ESP_LOGE(TAG, "Failed to render display: %s", esp_err_to_name(result));
        if (request.state == ONDA_DISPLAY_ERROR || s_application_command_queue == NULL) {
            continue;
        }

        const onda_application_command_t command = {.command = ONDA_COMMAND_DISPLAY_FAILURE};
        if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to report display failure");
        }
    }
}

static void onda_handle_command(onda_state_t *recording_state,
                                wifi_state_t *network_state,
                                char *proof_of_possession,
                                const onda_application_command_t *command)
{
    if (recording_state == NULL || network_state == NULL || proof_of_possession == NULL ||
        command == NULL) {
        ESP_LOGE(TAG, "Invalid application command context");
        return;
    }

    switch (command->command) {
    case ONDA_COMMAND_TOGGLE_RECORDING:
        if (*recording_state == ONDA_STATE_READY) {
            onda_start_recording(recording_state, *network_state, proof_of_possession);
        } else if (*recording_state == ONDA_STATE_RECORDING) {
            onda_stop_recording(recording_state, *network_state, proof_of_possession);
        } else {
            ESP_LOGW(TAG, "Ignoring recording toggle while in ERROR state");
        }
        break;
    case ONDA_COMMAND_RESET_WIFI:
        if (*recording_state != ONDA_STATE_READY) {
            ESP_LOGW(TAG, "Ignoring Wi-Fi reset unless recording is READY");
            break;
        }
        ESP_LOGI(TAG, "Requesting Wi-Fi reconfiguration");
        if (wifi_request_reprovision() != ESP_OK) {
            *network_state = WIFI_STATE_ERROR;
        }
        (void)onda_schedule_display(*recording_state, *network_state, proof_of_possession);
        break;
    case ONDA_COMMAND_WIFI_STATE_CHANGED:
        *network_state = command->wifi_state;
        if (command->proof_of_possession[0] != '\0') {
            memcpy(proof_of_possession,
                   command->proof_of_possession,
                   ONDA_WIFI_POP_LENGTH + 1U);
        }
        ESP_LOGI(TAG, "Wi-Fi state: %s", onda_wifi_state_name(*network_state));
        if (*network_state != WIFI_STATE_UNCONFIGURED) {
            (void)onda_schedule_display(*recording_state, *network_state, proof_of_possession);
        }
        break;
    case ONDA_COMMAND_DISPLAY_FAILURE:
        if (*recording_state != ONDA_STATE_ERROR) {
            onda_enter_error(recording_state,
                             "Display update",
                             ESP_FAIL,
                             *recording_state == ONDA_STATE_RECORDING);
        }
        break;
    default:
        onda_enter_error(recording_state, "Command handling", ESP_ERR_INVALID_ARG, false);
        break;
    }
}

static void onda_start_recording(onda_state_t *state,
                                 wifi_state_t network_state,
                                 const char *proof_of_possession)
{
    ESP_LOGI(TAG, "Transition READY -> RECORDING");
    const esp_err_t result = audio_start();
    if (result != ESP_OK) {
        onda_enter_error(state, "Recording start", result, false);
        return;
    }

    *state = ONDA_STATE_RECORDING;
    const esp_err_t display_result = onda_schedule_display(*state,
                                                            network_state,
                                                            proof_of_possession);
    if (display_result != ESP_OK) {
        onda_enter_error(state, "Recording display request", display_result, true);
        return;
    }
    ESP_LOGI(TAG, "State: %s", onda_state_name(*state));
}

static void onda_stop_recording(onda_state_t *state,
                                wifi_state_t network_state,
                                const char *proof_of_possession)
{
    ESP_LOGI(TAG, "Transition RECORDING -> READY");
    const esp_err_t result = audio_stop();
    if (result != ESP_OK) {
        onda_enter_error(state, "Recording stop", result, true);
        return;
    }

    *state = ONDA_STATE_READY;
    const esp_err_t display_result = onda_schedule_display(*state,
                                                            network_state,
                                                            proof_of_possession);
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
    const esp_err_t display_result = onda_schedule_display(*state, WIFI_STATE_ERROR, NULL);
    if (display_result != ESP_OK) {
        ESP_LOGE(TAG, "Error display request failed: %s", esp_err_to_name(display_result));
    }
    ESP_LOGE(TAG, "State: %s", onda_state_name(*state));
}

static esp_err_t onda_schedule_display(onda_state_t recording_state,
                                       wifi_state_t network_state,
                                       const char *proof_of_possession)
{
    if (s_display_state_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    onda_display_request_t request = {
        .state = onda_select_display_state(recording_state, network_state),
    };
    if (proof_of_possession != NULL && request.state == ONDA_DISPLAY_WIFI_SETUP) {
        strncpy(request.proof_of_possession,
                proof_of_possession,
                sizeof(request.proof_of_possession) - 1U);
    }

    return xQueueOverwrite(s_display_state_queue, &request) == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t onda_render_display(const onda_display_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t init_result = display_init();
    if (init_result != ESP_OK) {
        return init_result;
    }

    switch (request->state) {
    case ONDA_DISPLAY_READY:
        return display_show_ready(true);
    case ONDA_DISPLAY_RECORDING:
        return display_show_recording();
    case ONDA_DISPLAY_ERROR:
        return display_show_error();
    case ONDA_DISPLAY_WIFI_SETUP:
        return display_show_wifi_setup(request->proof_of_possession);
    case ONDA_DISPLAY_WIFI_CONNECTING:
        return display_show_wifi_connecting();
    case ONDA_DISPLAY_OFFLINE:
        return display_show_offline();
    case ONDA_DISPLAY_WIFI_ERROR:
        return display_show_wifi_error();
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static onda_display_state_t onda_select_display_state(onda_state_t recording_state,
                                                      wifi_state_t network_state)
{
    if (recording_state == ONDA_STATE_RECORDING) {
        return ONDA_DISPLAY_RECORDING;
    }
    if (recording_state == ONDA_STATE_ERROR) {
        return ONDA_DISPLAY_ERROR;
    }

    switch (network_state) {
    case WIFI_STATE_UNCONFIGURED:
    case WIFI_STATE_PROVISIONING:
        return ONDA_DISPLAY_WIFI_SETUP;
    case WIFI_STATE_CONNECTING:
        return ONDA_DISPLAY_WIFI_CONNECTING;
    case WIFI_STATE_CONNECTED:
        return ONDA_DISPLAY_READY;
    case WIFI_STATE_OFFLINE:
        return ONDA_DISPLAY_OFFLINE;
    case WIFI_STATE_ERROR:
    default:
        return ONDA_DISPLAY_WIFI_ERROR;
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

static const char *onda_wifi_state_name(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_UNCONFIGURED:
        return "UNCONFIGURED";
    case WIFI_STATE_PROVISIONING:
        return "PROVISIONING";
    case WIFI_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_STATE_CONNECTED:
        return "CONNECTED";
    case WIFI_STATE_OFFLINE:
        return "OFFLINE";
    case WIFI_STATE_ERROR:
        return "ERROR";
    default:
        return "INVALID";
    }
}
