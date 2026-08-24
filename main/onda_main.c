#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

#include "audio.h"
#include "audio_recorder.h"
#include "buttons.h"
#include "device_ui.h"
#include "recording_naming.h"
#include "storage.h"
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
#define ONDA_SAVED_SCREEN_TIMEOUT_MS 20000U

static const char *TAG = "ONDA";

typedef enum {
    ONDA_STATE_READY,
    ONDA_STATE_RECORDING,
    ONDA_STATE_FINALIZING,
    ONDA_STATE_SAVED,
    ONDA_STATE_STORAGE_ERROR,
    ONDA_STATE_AUDIO_ERROR,
    ONDA_STATE_ERROR,
} onda_state_t;

typedef enum {
    ONDA_COMMAND_TOGGLE_RECORDING,
    ONDA_COMMAND_RESET_WIFI,
    ONDA_COMMAND_WIFI_STATE_CHANGED,
    ONDA_COMMAND_DISPLAY_FAILURE,
    ONDA_COMMAND_RECORDER_COMPLETED,
    ONDA_COMMAND_SAVED_TIMEOUT,
} onda_command_t;

typedef struct {
    onda_command_t command;
    wifi_state_t wifi_state;
    audio_recorder_completion_t recorder_completion;
    char proof_of_possession[DEVICE_UI_PROOF_OF_POSSESSION_LENGTH + 1U];
} onda_application_command_t;

typedef struct {
    onda_state_t recording_state;
    wifi_state_t network_state;
    device_ui_storage_status_t storage_status;
    char proof_of_possession[DEVICE_UI_PROOF_OF_POSSESSION_LENGTH + 1U];
    char saved_duration[DEVICE_UI_SAVED_DURATION_LENGTH + 1U];
    char saved_filename[DEVICE_UI_SAVED_FILENAME_LENGTH + 1U];
} onda_application_model_t;

typedef struct {
    device_ui_state_t state;
} onda_display_request_t;

static QueueHandle_t s_application_command_queue;
static QueueHandle_t s_display_state_queue;
static TimerHandle_t s_saved_timer;
static StaticTimer_t s_saved_timer_buffer;
static device_ui_storage_status_t s_initial_storage_status = DEVICE_UI_STORAGE_UNAVAILABLE;
static bool s_initial_audio_ready;

static void onda_application_task(void *context);
static void onda_display_task(void *context);
static void onda_saved_timer_callback(TimerHandle_t timer);
static void onda_handle_button_event(button_event_t event, void *context);
static void onda_handle_wifi_state(wifi_state_t state, const char *proof_of_possession, void *context);
static void onda_handle_recorder_completion(const audio_recorder_completion_t *completion, void *context);
static void onda_handle_command(onda_application_model_t *model, const onda_application_command_t *command);
static void onda_start_recording(onda_application_model_t *model);
static void onda_stop_recording(onda_application_model_t *model);
static void onda_retry_storage(onda_application_model_t *model);
static void onda_enter_error(onda_application_model_t *model,
                             onda_state_t state,
                             const char *operation,
                             esp_err_t error,
                             bool stop_recorder);
static esp_err_t onda_prepare_recording_config(audio_recorder_config_t *config);
static esp_err_t onda_schedule_display(const onda_application_model_t *model);
static esp_err_t onda_render_display(const device_ui_state_t *state);
static device_ui_primary_state_t onda_select_ui_primary_state(const onda_application_model_t *model);
static device_ui_wifi_status_t onda_select_ui_wifi_status(wifi_state_t state);
static const char *onda_state_name(onda_state_t state);
static const char *onda_wifi_state_name(wifi_state_t state);

static void onda_handle_button_event(button_event_t event, void *context)
{
    (void)context;
    const char *button_name = event.id == BUTTON_ID_BOOT ? "BOOT" : "PWR";
    const char *event_name = event.type == BUTTON_EVENT_SHORT_PRESS ? "short press" :
                             event.type == BUTTON_EVENT_LONG_PRESS ? "long press" :
                             event.type == BUTTON_EVENT_VERY_LONG_PRESS ? "very long press" :
                                                                          "unknown press";
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

static void onda_handle_wifi_state(wifi_state_t state, const char *proof_of_possession, void *context)
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
        strncpy(command.proof_of_possession, proof_of_possession,
                sizeof(command.proof_of_possession) - 1U);
    }
    if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
        ESP_LOGE(TAG, "Wi-Fi state command queue is full");
    }
}

static void onda_handle_recorder_completion(const audio_recorder_completion_t *completion, void *context)
{
    (void)context;
    if (completion == NULL || s_application_command_queue == NULL) {
        ESP_LOGE(TAG, "Recorder completed before application startup");
        return;
    }
    const onda_application_command_t command = {
        .command = ONDA_COMMAND_RECORDER_COMPLETED,
        .recorder_completion = *completion,
    };
    if (xQueueSend(s_application_command_queue, &command, pdMS_TO_TICKS(100U)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to report recorder completion");
    }
}

static void onda_saved_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_application_command_queue == NULL) {
        return;
    }
    const onda_application_command_t command = {.command = ONDA_COMMAND_SAVED_TIMEOUT};
    if (xQueueSend(s_application_command_queue, &command, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to report Saved timeout");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Onda firmware");
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Target: %s, cores: %u, silicon revision: v%u.%u", CONFIG_IDF_TARGET,
             chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);

    bool hardware_matches = chip_info.model == CHIP_ESP32S3;
    if (!hardware_matches) {
        ESP_LOGE(TAG, "Unexpected chip model; ESP32-S3 required");
    }
    uint32_t flash_size_bytes = 0;
    const esp_err_t flash_result = esp_flash_get_size(NULL, &flash_size_bytes);
    if (flash_result != ESP_OK || flash_size_bytes / BYTES_PER_MEBIBYTE != EXPECTED_FLASH_SIZE_MB) {
        ESP_LOGE(TAG, "Unexpected flash configuration");
        hardware_matches = false;
    }
    if (!esp_psram_is_initialized() ||
        esp_psram_get_size() / BYTES_PER_MEBIBYTE != EXPECTED_PSRAM_SIZE_MB) {
        ESP_LOGE(TAG, "Unexpected PSRAM configuration");
        hardware_matches = false;
    }
    if (!hardware_matches) {
        ESP_LOGE(TAG, "Board bring-up checks failed");
        return;
    }

    const esp_err_t storage_result = storage_init();
    if (storage_result == ESP_OK && storage_verify() == ESP_OK) {
        s_initial_storage_status = DEVICE_UI_STORAGE_AVAILABLE;
    } else {
        ESP_LOGW(TAG, "SD card unavailable during startup");
    }
    const esp_err_t audio_result = audio_init();
    s_initial_audio_ready = audio_result == ESP_OK;
    if (!s_initial_audio_ready) {
        ESP_LOGE(TAG, "Audio input unavailable: %s", esp_err_to_name(audio_result));
    }

    s_application_command_queue =
        xQueueCreate(ONDA_APPLICATION_COMMAND_QUEUE_LENGTH, sizeof(onda_application_command_t));
    s_display_state_queue = xQueueCreate(ONDA_DISPLAY_STATE_QUEUE_LENGTH, sizeof(onda_display_request_t));
    s_saved_timer = xTimerCreateStatic("onda_saved", pdMS_TO_TICKS(ONDA_SAVED_SCREEN_TIMEOUT_MS),
                                       pdFALSE, NULL, onda_saved_timer_callback, &s_saved_timer_buffer);
    if (s_application_command_queue == NULL || s_display_state_queue == NULL || s_saved_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create application resources");
        return;
    }
    if (xTaskCreate(onda_display_task, "onda_display", ONDA_DISPLAY_TASK_STACK_SIZE, NULL,
                    ONDA_DISPLAY_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(onda_application_task, "onda_app", ONDA_APPLICATION_TASK_STACK_SIZE, NULL,
                    ONDA_APPLICATION_TASK_PRIORITY, NULL) != pdPASS) {
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
        const onda_application_command_t command = {
            .command = ONDA_COMMAND_WIFI_STATE_CHANGED,
            .wifi_state = WIFI_STATE_ERROR,
        };
        (void)xQueueSend(s_application_command_queue, &command, 0);
    }
    ESP_LOGI(TAG, "Onda ready");
}

static void onda_application_task(void *context)
{
    (void)context;
    onda_application_model_t model = {
        .recording_state = !s_initial_audio_ready ? ONDA_STATE_AUDIO_ERROR :
                           s_initial_storage_status == DEVICE_UI_STORAGE_AVAILABLE ? ONDA_STATE_READY :
                                                                                       ONDA_STATE_STORAGE_ERROR,
        .network_state = WIFI_STATE_UNCONFIGURED,
        .storage_status = s_initial_storage_status,
    };
    if (model.recording_state != ONDA_STATE_READY) {
        (void)onda_schedule_display(&model);
    }
    for (;;) {
        onda_application_command_t command;
        if (xQueueReceive(s_application_command_queue, &command, portMAX_DELAY) == pdTRUE) {
            onda_handle_command(&model, &command);
        }
    }
}

static void onda_display_task(void *context)
{
    (void)context;
    device_ui_state_t last_rendered_state = {0};
    bool has_rendered_state = false;
    for (;;) {
        onda_display_request_t request;
        xQueueReceive(s_display_state_queue, &request, portMAX_DELAY);
        if (!device_ui_should_refresh(has_rendered_state ? &last_rendered_state : NULL, &request.state)) {
            ESP_LOGI(TAG, "Skipping display refresh for unchanged primary UI state");
            continue;
        }
        const esp_err_t result = onda_render_display(&request.state);
        if (result == ESP_OK) {
            last_rendered_state = request.state;
            has_rendered_state = true;
            continue;
        }
        ESP_LOGE(TAG, "Failed to render display: %s", esp_err_to_name(result));
        if (request.state.primary_state != DEVICE_UI_PRIMARY_ERROR &&
            s_application_command_queue != NULL) {
            const onda_application_command_t command = {.command = ONDA_COMMAND_DISPLAY_FAILURE};
            (void)xQueueSend(s_application_command_queue, &command, 0);
        }
    }
}

static void onda_handle_command(onda_application_model_t *model, const onda_application_command_t *command)
{
    if (model == NULL || command == NULL) {
        ESP_LOGE(TAG, "Invalid application command context");
        return;
    }
    switch (command->command) {
    case ONDA_COMMAND_TOGGLE_RECORDING:
        if (model->recording_state == ONDA_STATE_READY) {
            onda_start_recording(model);
        } else if (model->recording_state == ONDA_STATE_RECORDING) {
            onda_stop_recording(model);
        } else if (model->recording_state == ONDA_STATE_SAVED) {
            (void)xTimerStop(s_saved_timer, 0);
            model->recording_state = ONDA_STATE_READY;
            (void)onda_schedule_display(model);
        } else if (model->recording_state == ONDA_STATE_STORAGE_ERROR) {
            onda_retry_storage(model);
        } else {
            ESP_LOGW(TAG, "Ignoring BOOT while in %s", onda_state_name(model->recording_state));
        }
        break;
    case ONDA_COMMAND_RESET_WIFI:
        if (model->recording_state != ONDA_STATE_READY) {
            ESP_LOGW(TAG, "Ignoring Wi-Fi reset unless recording is READY");
        } else if (wifi_request_reprovision() != ESP_OK) {
            model->network_state = WIFI_STATE_ERROR;
            (void)onda_schedule_display(model);
        }
        break;
    case ONDA_COMMAND_WIFI_STATE_CHANGED:
        model->network_state = command->wifi_state;
        if (command->proof_of_possession[0] != '\0') {
            memcpy(model->proof_of_possession, command->proof_of_possession,
                   sizeof(model->proof_of_possession));
        }
        ESP_LOGI(TAG, "Wi-Fi state: %s", onda_wifi_state_name(model->network_state));
        (void)onda_schedule_display(model);
        break;
    case ONDA_COMMAND_DISPLAY_FAILURE:
        if (model->recording_state != ONDA_STATE_ERROR) {
            onda_enter_error(model, ONDA_STATE_ERROR, "Display update", ESP_FAIL,
                             model->recording_state == ONDA_STATE_RECORDING);
        }
        break;
    case ONDA_COMMAND_RECORDER_COMPLETED:
        if (model->recording_state != ONDA_STATE_RECORDING &&
            model->recording_state != ONDA_STATE_FINALIZING) {
            ESP_LOGW(TAG, "Ignoring recorder completion outside active recording state");
            break;
        }
        if (command->recorder_completion.kind != AUDIO_RECORDER_COMPLETION_SUCCESS) {
            const onda_state_t error_state =
                command->recorder_completion.kind == AUDIO_RECORDER_COMPLETION_AUDIO_ERROR
                    ? ONDA_STATE_AUDIO_ERROR
                    : ONDA_STATE_STORAGE_ERROR;
            if (error_state == ONDA_STATE_STORAGE_ERROR) {
                model->storage_status = DEVICE_UI_STORAGE_ERROR;
            }
            onda_enter_error(model, error_state, "Recording finalization",
                             command->recorder_completion.error, false);
            break;
        }
        if (recording_naming_format_duration(command->recorder_completion.data_bytes,
                                             model->saved_duration,
                                             sizeof(model->saved_duration)) != ESP_OK) {
            onda_enter_error(model, ONDA_STATE_ERROR, "Recording duration", ESP_ERR_INVALID_SIZE, false);
            break;
        }
        const char *const basename = strrchr(command->recorder_completion.final_path, '/');
        if (basename == NULL || basename[1] == '\0') {
            onda_enter_error(model, ONDA_STATE_ERROR, "Recording filename", ESP_ERR_INVALID_ARG, false);
            break;
        }
        strncpy(model->saved_filename, basename + 1U, sizeof(model->saved_filename) - 1U);
        model->recording_state = ONDA_STATE_SAVED;
        model->storage_status = DEVICE_UI_STORAGE_AVAILABLE;
        ESP_LOGI("ONDA_RECORDING", "Saved successfully: %s, duration %s, %u PCM bytes",
                 command->recorder_completion.final_path, model->saved_duration,
                 (unsigned)command->recorder_completion.data_bytes);
        if (onda_schedule_display(model) != ESP_OK) {
            onda_enter_error(model, ONDA_STATE_ERROR, "Saved display request", ESP_FAIL, false);
        } else if (xTimerReset(s_saved_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start Saved timeout");
        }
        break;
    case ONDA_COMMAND_SAVED_TIMEOUT:
        if (model->recording_state == ONDA_STATE_SAVED) {
            model->recording_state = ONDA_STATE_READY;
            (void)onda_schedule_display(model);
            ESP_LOGI(TAG, "Saved confirmation timeout elapsed");
        }
        break;
    default:
        onda_enter_error(model, ONDA_STATE_ERROR, "Command handling", ESP_ERR_INVALID_ARG, false);
        break;
    }
}

static void onda_start_recording(onda_application_model_t *model)
{
    audio_recorder_config_t config = {0};
    const esp_err_t preparation_result = onda_prepare_recording_config(&config);
    if (preparation_result != ESP_OK) {
        model->storage_status = DEVICE_UI_STORAGE_ERROR;
        onda_enter_error(model, ONDA_STATE_STORAGE_ERROR, "Recording path preparation",
                         preparation_result, false);
        return;
    }
    config.completion_callback = onda_handle_recorder_completion;
    const esp_err_t result = audio_recorder_start(&config);
    if (result != ESP_OK) {
        onda_enter_error(model, ONDA_STATE_ERROR, "Recording start", result, false);
        return;
    }
    model->recording_state = ONDA_STATE_RECORDING;
    if (onda_schedule_display(model) != ESP_OK) {
        (void)audio_recorder_stop();
        onda_enter_error(model, ONDA_STATE_ERROR, "Recording display request", ESP_FAIL, false);
        return;
    }
    ESP_LOGI(TAG, "State: %s", onda_state_name(model->recording_state));
}

static void onda_stop_recording(onda_application_model_t *model)
{
    model->recording_state = ONDA_STATE_FINALIZING;
    ESP_LOGI("ONDA_RECORDING", "Stop requested; finalizing WAV");
    if (onda_schedule_display(model) != ESP_OK) {
        onda_enter_error(model, ONDA_STATE_ERROR, "Finalizing display request", ESP_FAIL, true);
        return;
    }
    const esp_err_t result = audio_recorder_stop();
    if (result != ESP_OK) {
        onda_enter_error(model, ONDA_STATE_ERROR, "Recording stop", result, false);
    }
}

static void onda_retry_storage(onda_application_model_t *model)
{
    ESP_LOGI("ONDA_STORAGE", "Retrying SD card mount and verification");
    const esp_err_t deinit_result = storage_deinit();
    if (deinit_result != ESP_OK) {
        ESP_LOGW("ONDA_STORAGE", "Failed to release old SD mount: %s", esp_err_to_name(deinit_result));
    }
    const esp_err_t init_result = storage_init();
    const esp_err_t verify_result = init_result == ESP_OK ? storage_verify() : init_result;
    if (verify_result != ESP_OK) {
        model->storage_status = DEVICE_UI_STORAGE_UNAVAILABLE;
        ESP_LOGW("ONDA_STORAGE", "SD retry failed: %s", esp_err_to_name(verify_result));
        (void)onda_schedule_display(model);
        return;
    }
    model->storage_status = DEVICE_UI_STORAGE_AVAILABLE;
    model->recording_state = ONDA_STATE_READY;
    (void)onda_schedule_display(model);
    ESP_LOGI(TAG, "State: %s", onda_state_name(model->recording_state));
}

static void onda_enter_error(onda_application_model_t *model,
                             onda_state_t state,
                             const char *operation,
                             esp_err_t error,
                             bool stop_recorder)
{
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    if (stop_recorder) {
        const esp_err_t stop_result = audio_recorder_stop();
        if (stop_result != ESP_OK) {
            ESP_LOGE(TAG, "Recorder cleanup request failed: %s", esp_err_to_name(stop_result));
        }
    }
    model->recording_state = state;
    if (onda_schedule_display(model) != ESP_OK) {
        ESP_LOGE(TAG, "Error display request failed");
    }
    ESP_LOGE(TAG, "State: %s", onda_state_name(model->recording_state));
}

static esp_err_t onda_prepare_recording_config(audio_recorder_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t now = time(NULL);
    const uint32_t first_ordinal = recording_naming_time_is_reliable(now) ? 0U : 1U;
    for (uint32_t ordinal = first_ordinal; ordinal <= RECORDING_NAMING_MAX_ORDINAL; ++ordinal) {
        const esp_err_t name_result = recording_naming_make_path(now, ordinal, config->final_path,
                                                                  sizeof(config->final_path));
        if (name_result != ESP_OK) {
            return name_result;
        }
        const esp_err_t path_result = storage_recording_prepare_paths(config->final_path,
                                                                        config->staging_path,
                                                                        sizeof(config->staging_path));
        if (path_result == ESP_OK) {
            ESP_LOGI("ONDA_RECORDING", "File %s", config->final_path);
            return ESP_OK;
        }
        if (path_result != ESP_ERR_NOT_FOUND) {
            return path_result;
        }
    }
    ESP_LOGE("ONDA_RECORDING", "No collision-safe recording filename is available");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t onda_schedule_display(const onda_application_model_t *model)
{
    if (model == NULL || s_display_state_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    onda_display_request_t request = {
        .state = {
            .primary_state = onda_select_ui_primary_state(model),
            .wifi_status = onda_select_ui_wifi_status(model->network_state),
            .storage_status = model->storage_status,
            .battery_status = DEVICE_UI_BATTERY_UNKNOWN,
        },
    };
    if (request.state.primary_state == DEVICE_UI_PRIMARY_WIFI_SETUP) {
        strncpy(request.state.proof_of_possession, model->proof_of_possession,
                sizeof(request.state.proof_of_possession) - 1U);
    }
    if (request.state.primary_state == DEVICE_UI_PRIMARY_SAVED) {
        strncpy(request.state.saved_duration, model->saved_duration,
                sizeof(request.state.saved_duration) - 1U);
        strncpy(request.state.saved_filename, model->saved_filename,
                sizeof(request.state.saved_filename) - 1U);
    }
    return xQueueOverwrite(s_display_state_queue, &request) == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t onda_render_display(const device_ui_state_t *state)
{
    return state == NULL ? ESP_ERR_INVALID_ARG : device_ui_show(state);
}

static device_ui_primary_state_t onda_select_ui_primary_state(const onda_application_model_t *model)
{
    switch (model->recording_state) {
    case ONDA_STATE_RECORDING:
        return DEVICE_UI_PRIMARY_RECORDING;
    case ONDA_STATE_FINALIZING:
        return DEVICE_UI_PRIMARY_FINALIZING;
    case ONDA_STATE_SAVED:
        return DEVICE_UI_PRIMARY_SAVED;
    case ONDA_STATE_STORAGE_ERROR:
        return DEVICE_UI_PRIMARY_STORAGE_ERROR;
    case ONDA_STATE_AUDIO_ERROR:
        return DEVICE_UI_PRIMARY_AUDIO_ERROR;
    case ONDA_STATE_ERROR:
        return DEVICE_UI_PRIMARY_ERROR;
    case ONDA_STATE_READY:
    default:
        break;
    }
    return model->network_state == WIFI_STATE_UNCONFIGURED ||
                   model->network_state == WIFI_STATE_PROVISIONING
               ? DEVICE_UI_PRIMARY_WIFI_SETUP
               : DEVICE_UI_PRIMARY_READY;
}

static device_ui_wifi_status_t onda_select_ui_wifi_status(wifi_state_t state)
{
    return state == WIFI_STATE_CONNECTED ? DEVICE_UI_WIFI_CONNECTED : DEVICE_UI_WIFI_OFFLINE;
}

static const char *onda_state_name(onda_state_t state)
{
    switch (state) {
    case ONDA_STATE_READY: return "READY";
    case ONDA_STATE_RECORDING: return "RECORDING";
    case ONDA_STATE_FINALIZING: return "FINALIZING";
    case ONDA_STATE_SAVED: return "SAVED";
    case ONDA_STATE_STORAGE_ERROR: return "STORAGE_ERROR";
    case ONDA_STATE_AUDIO_ERROR: return "AUDIO_ERROR";
    case ONDA_STATE_ERROR: return "ERROR";
    default: return "INVALID";
    }
}

static const char *onda_wifi_state_name(wifi_state_t state)
{
    switch (state) {
    case WIFI_STATE_UNCONFIGURED: return "UNCONFIGURED";
    case WIFI_STATE_PROVISIONING: return "PROVISIONING";
    case WIFI_STATE_CONNECTING: return "CONNECTING";
    case WIFI_STATE_CONNECTED: return "CONNECTED";
    case WIFI_STATE_OFFLINE: return "OFFLINE";
    case WIFI_STATE_ERROR: return "ERROR";
    default: return "INVALID";
    }
}
