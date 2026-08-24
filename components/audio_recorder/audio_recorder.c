#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio.h"
#include "audio_recorder.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"

#define AUDIO_RECORDER_FINAL_PATH "/sdcard/test.wav"
#define AUDIO_RECORDER_STAGING_PATH "/sdcard/test.wav.part"
#define AUDIO_RECORDER_SAMPLE_RATE_HZ 16000U
#define AUDIO_RECORDER_CHANNEL_COUNT 1U
#define AUDIO_RECORDER_BITS_PER_SAMPLE 16U
#define AUDIO_RECORDER_BYTES_PER_SAMPLE (AUDIO_RECORDER_BITS_PER_SAMPLE / CHAR_BIT)
#define AUDIO_RECORDER_STEREO_FRAME_BYTES (AUDIO_RECORDER_BYTES_PER_SAMPLE * 2U)
#define AUDIO_RECORDER_CAPTURE_SAMPLES 1024U
#define AUDIO_RECORDER_READ_TIMEOUT_MS 100U
#define AUDIO_RECORDER_TASK_STACK_SIZE 4096U
#define AUDIO_RECORDER_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define AUDIO_RECORDER_BYTES_PER_SECOND \
    (AUDIO_RECORDER_SAMPLE_RATE_HZ * AUDIO_RECORDER_CHANNEL_COUNT * AUDIO_RECORDER_BYTES_PER_SAMPLE)

static const char *TAG = "ONDA_RECORDER";

static TaskHandle_t s_task_handle;
static audio_recorder_completion_callback_t s_completion_callback;
static void *s_completion_context;
static DMA_ATTR int16_t s_capture_buffer[AUDIO_RECORDER_CAPTURE_SAMPLES];
static int16_t s_mono_buffer[AUDIO_RECORDER_CAPTURE_SAMPLES / 2U];
static StackType_t s_task_stack[AUDIO_RECORDER_TASK_STACK_SIZE];
static StaticTask_t s_task_buffer;

static void audio_recorder_task(void *context);
static esp_err_t audio_recorder_write_header(storage_file_t *file, uint32_t data_bytes);
static void audio_recorder_write_u16_le(uint8_t *destination, uint16_t value);
static void audio_recorder_write_u32_le(uint8_t *destination, uint32_t value);
static void audio_recorder_complete(esp_err_t result, size_t data_bytes);

esp_err_t audio_recorder_start(audio_recorder_completion_callback_t callback, void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_completion_callback = callback;
    s_completion_context = context;
    s_task_handle = xTaskCreateStatic(audio_recorder_task,
                                      "onda_recorder",
                                      AUDIO_RECORDER_TASK_STACK_SIZE,
                                      NULL,
                                      AUDIO_RECORDER_TASK_PRIORITY,
                                      s_task_stack,
                                      &s_task_buffer);
    if (s_task_handle == NULL) {
        s_completion_callback = NULL;
        s_completion_context = NULL;
        ESP_LOGE(TAG, "Failed to create recorder task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t audio_recorder_stop(void)
{
    if (s_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xTaskNotifyGive(s_task_handle);
    return ESP_OK;
}

void audio_recorder_make_wav_header(uint8_t header[AUDIO_RECORDER_WAV_HEADER_SIZE],
                                    uint32_t data_bytes)
{
    if (header == NULL) {
        return;
    }

    memset(header, 0, AUDIO_RECORDER_WAV_HEADER_SIZE);
    memcpy(&header[0], "RIFF", 4U);
    audio_recorder_write_u32_le(&header[4], data_bytes + 36U);
    memcpy(&header[8], "WAVEfmt ", 8U);
    audio_recorder_write_u32_le(&header[16], 16U);
    audio_recorder_write_u16_le(&header[20], 1U);
    audio_recorder_write_u16_le(&header[22], AUDIO_RECORDER_CHANNEL_COUNT);
    audio_recorder_write_u32_le(&header[24], AUDIO_RECORDER_SAMPLE_RATE_HZ);
    audio_recorder_write_u32_le(&header[28],
                                AUDIO_RECORDER_SAMPLE_RATE_HZ *
                                    AUDIO_RECORDER_CHANNEL_COUNT *
                                    AUDIO_RECORDER_BYTES_PER_SAMPLE);
    audio_recorder_write_u16_le(&header[32],
                                AUDIO_RECORDER_CHANNEL_COUNT *
                                    AUDIO_RECORDER_BYTES_PER_SAMPLE);
    audio_recorder_write_u16_le(&header[34], AUDIO_RECORDER_BITS_PER_SAMPLE);
    memcpy(&header[36], "data", 4U);
    audio_recorder_write_u32_le(&header[40], data_bytes);
}

size_t audio_recorder_downmix_stereo(const int16_t *input,
                                     size_t input_bytes,
                                     int16_t *output,
                                     size_t output_capacity_bytes)
{
    if (input == NULL || output == NULL) {
        return 0U;
    }

    const size_t input_frames = input_bytes / AUDIO_RECORDER_STEREO_FRAME_BYTES;
    const size_t output_frames = output_capacity_bytes / sizeof(output[0]);
    const size_t frame_count = input_frames < output_frames ? input_frames : output_frames;

    for (size_t frame = 0; frame < frame_count; ++frame) {
        const int32_t combined = (int32_t)input[frame * 2U] + (int32_t)input[frame * 2U + 1U];
        output[frame] = (int16_t)(combined / 2);
    }

    return frame_count * sizeof(output[0]);
}

static void audio_recorder_task(void *context)
{
    (void)context;

    storage_file_t *file = NULL;
    size_t data_bytes = 0U;
    size_t next_progress_bytes = AUDIO_RECORDER_BYTES_PER_SECOND;
    bool capture_started = false;
    esp_err_t result = storage_file_create(AUDIO_RECORDER_STAGING_PATH, &file);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create staging file: %s", esp_err_to_name(result));
        goto finish;
    }

    result = audio_recorder_write_header(file, 0U);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write WAV header: %s", esp_err_to_name(result));
        goto cleanup_file;
    }

    result = audio_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start microphone capture: %s", esp_err_to_name(result));
        goto cleanup_file;
    }
    capture_started = true;
    ESP_LOGI(TAG, "Recording started: %s", AUDIO_RECORDER_STAGING_PATH);

    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, 0) != 0U) {
            break;
        }

        size_t bytes_read = 0U;
        const esp_err_t read_result = audio_read(s_capture_buffer,
                                                 sizeof(s_capture_buffer),
                                                 &bytes_read,
                                                 pdMS_TO_TICKS(AUDIO_RECORDER_READ_TIMEOUT_MS));
        if (read_result != ESP_OK && read_result != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Microphone capture failed: %s", esp_err_to_name(read_result));
            result = read_result;
            goto cleanup_capture;
        }
        if (bytes_read == 0U) {
            continue;
        }
        if (bytes_read % AUDIO_RECORDER_STEREO_FRAME_BYTES != 0U) {
            ESP_LOGW(TAG, "Discarding incomplete stereo frame");
        }

        const size_t mono_bytes = audio_recorder_downmix_stereo(s_capture_buffer,
                                                                 bytes_read,
                                                                 s_mono_buffer,
                                                                 sizeof(s_mono_buffer));
        if (mono_bytes == 0U) {
            continue;
        }
        if (data_bytes > UINT32_MAX - mono_bytes) {
            ESP_LOGE(TAG, "WAV data exceeds the 4 GiB PCM limit");
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup_capture;
        }

        result = storage_file_write(file, s_mono_buffer, mono_bytes);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "SD write failed: %s", esp_err_to_name(result));
            goto cleanup_capture;
        }
        data_bytes += mono_bytes;
        while (data_bytes >= next_progress_bytes) {
            ESP_LOGI(TAG, "Recording progress: %u s", (unsigned)(next_progress_bytes /
                                                                    AUDIO_RECORDER_BYTES_PER_SECOND));
            next_progress_bytes += AUDIO_RECORDER_BYTES_PER_SECOND;
        }
    }

cleanup_capture:
    if (capture_started) {
        const esp_err_t stop_result = audio_stop();
        if (stop_result != ESP_OK && result == ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop microphone capture: %s", esp_err_to_name(stop_result));
            result = stop_result;
        }
        capture_started = false;
    }
    if (result != ESP_OK) {
        goto cleanup_file;
    }

    result = storage_file_sync(file);
    if (result == ESP_OK) {
        result = audio_recorder_write_header(file, (uint32_t)data_bytes);
    }
    if (result == ESP_OK) {
        result = storage_file_sync(file);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to finalize WAV file: %s", esp_err_to_name(result));
        goto cleanup_file;
    }

    result = storage_file_close(file);
    file = NULL;
    if (result != ESP_OK) {
        goto remove_staging;
    }

    result = storage_file_replace(AUDIO_RECORDER_STAGING_PATH, AUDIO_RECORDER_FINAL_PATH);
    if (result != ESP_OK) {
        goto remove_staging;
    }

    ESP_LOGI(TAG, "Recording complete: %u bytes", (unsigned)data_bytes);
    goto finish;

cleanup_file:
    if (file != NULL) {
        const esp_err_t close_result = storage_file_close(file);
        if (result == ESP_OK && close_result != ESP_OK) {
            result = close_result;
        }
        file = NULL;
    }

remove_staging:
    if (storage_file_remove(AUDIO_RECORDER_STAGING_PATH) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove incomplete staging file");
    }

finish:
    audio_recorder_complete(result, data_bytes);
    vTaskDelete(NULL);
}

static esp_err_t audio_recorder_write_header(storage_file_t *file, uint32_t data_bytes)
{
    uint8_t header[AUDIO_RECORDER_WAV_HEADER_SIZE];
    audio_recorder_make_wav_header(header, data_bytes);

    esp_err_t result = storage_file_seek(file, 0L);
    if (result == ESP_OK) {
        result = storage_file_write(file, header, sizeof(header));
    }
    return result;
}

static void audio_recorder_write_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)(value >> 8U);
}

static void audio_recorder_write_u32_le(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xffU);
    destination[1] = (uint8_t)((value >> 8U) & 0xffU);
    destination[2] = (uint8_t)((value >> 16U) & 0xffU);
    destination[3] = (uint8_t)(value >> 24U);
}

static void audio_recorder_complete(esp_err_t result, size_t data_bytes)
{
    const audio_recorder_completion_callback_t callback = s_completion_callback;
    void *const context = s_completion_context;
    s_task_handle = NULL;
    s_completion_callback = NULL;
    s_completion_context = NULL;

    if (callback != NULL) {
        callback(result, data_bytes, context);
    }
}
