#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "onda_api.h"
#include "recording_metadata.h"
#include "recording_sync.h"

#define RECORDING_SYNC_QUEUE_LENGTH 1U
#define RECORDING_SYNC_TASK_STACK_SIZE 6144U
#define RECORDING_SYNC_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

static const char *TAG = "ONDA_SYNC";

typedef struct {
    recording_sync_completion_callback_t callback;
    void *context;
} recording_sync_command_t;

typedef struct {
    recording_sync_completion_t completion;
    bool stop;
} recording_sync_run_t;

static QueueHandle_t s_command_queue;
static bool s_initialised;

static void recording_sync_task(void *context);
static esp_err_t recording_sync_count_entry(recording_metadata_record_t *record, void *context);
static esp_err_t recording_sync_process_entry(recording_metadata_record_t *record, void *context);

esp_err_t recording_sync_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }
    s_command_queue = xQueueCreate(RECORDING_SYNC_QUEUE_LENGTH, sizeof(recording_sync_command_t));
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(recording_sync_task, "onda_sync", RECORDING_SYNC_TASK_STACK_SIZE, NULL,
                    RECORDING_SYNC_TASK_PRIORITY, NULL) != pdPASS) {
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialised = true;
    return ESP_OK;
}

esp_err_t recording_sync_count_pending(size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    return recording_metadata_for_each_pending(recording_sync_count_entry, count);
}

esp_err_t recording_sync_start(recording_sync_completion_callback_t callback, void *context)
{
    if (!s_initialised || s_command_queue == NULL || callback == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const recording_sync_command_t command = {.callback = callback, .context = context};
    return xQueueSend(s_command_queue, &command, 0) == pdPASS ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void recording_sync_task(void *context)
{
    (void)context;
    for (;;) {
        recording_sync_command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        recording_sync_run_t run = {.completion = {.kind = RECORDING_SYNC_RESULT_UP_TO_DATE}, .stop = false};
        const esp_err_t scan_result = recording_metadata_for_each_pending(recording_sync_process_entry, &run);
        if (scan_result != ESP_OK && !run.stop) {
            ESP_LOGW(TAG, "Pending recording scan failed: %s", esp_err_to_name(scan_result));
            run.completion.kind = RECORDING_SYNC_RESULT_PARTIAL;
        }
        if (run.completion.kind != RECORDING_SYNC_RESULT_CONNECTION_FAILURE &&
            run.completion.kind != RECORDING_SYNC_RESULT_AUTH_FAILURE) {
            run.completion.kind = run.completion.pending_count > 0U
                                      ? RECORDING_SYNC_RESULT_PARTIAL
                                      : run.completion.uploaded_count > 0U
                                            ? RECORDING_SYNC_RESULT_SYNCED
                                            : RECORDING_SYNC_RESULT_UP_TO_DATE;
        }
        if (command.callback != NULL) {
            command.callback(&run.completion, command.context);
        }
    }
}

static esp_err_t recording_sync_count_entry(recording_metadata_record_t *record, void *context)
{
    (void)record;
    size_t *count = context;
    if (count == NULL || *count == SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    ++*count;
    return ESP_OK;
}

static esp_err_t recording_sync_process_entry(recording_metadata_record_t *record, void *context)
{
    recording_sync_run_t *run = context;
    if (record == NULL || run == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ++run->completion.pending_count;
    if (recording_metadata_ensure_size(record) != ESP_OK) {
        ESP_LOGW(TAG, "Cannot prepare %s for sync", record->id);
        return ESP_OK;
    }
    onda_api_sync_result_t result = {0};
    const esp_err_t api_result = onda_api_sync_recording(record, &result);
    if (api_result != ESP_OK || result.outcome == ONDA_API_SYNC_RECORD_FAILURE) {
        ESP_LOGW(TAG, "Sync failed for %s", record->id);
        return ESP_OK;
    }
    if (result.outcome == ONDA_API_SYNC_AUTH_FAILURE || result.outcome == ONDA_API_SYNC_CONNECTION_FAILURE) {
        run->completion.kind = result.outcome == ONDA_API_SYNC_AUTH_FAILURE
                                   ? RECORDING_SYNC_RESULT_AUTH_FAILURE
                                   : RECORDING_SYNC_RESULT_CONNECTION_FAILURE;
        run->stop = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (recording_metadata_mark_synced(record, result.meeting_id, result.synced_at) != ESP_OK) {
        ESP_LOGW(TAG, "BFF confirmed %s but metadata update failed", record->id);
        return ESP_OK;
    }
    ++run->completion.uploaded_count;
    --run->completion.pending_count;
    run->completion.kind = RECORDING_SYNC_RESULT_SYNCED;
    return ESP_OK;
}
