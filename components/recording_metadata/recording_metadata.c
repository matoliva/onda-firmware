#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"

#include "recording_metadata.h"
#include "storage.h"

#define RECORDING_METADATA_SEED_SUFFIX ".json.seed"
#define RECORDING_METADATA_STAGING_SUFFIX ".json.part"
#define RECORDING_METADATA_FINAL_SUFFIX ".json"

static const char *TAG = "ONDA_METADATA";

typedef struct {
    const char *suffix;
} recording_metadata_recovery_context_t;

static esp_err_t recording_metadata_derive_paths(const char *wav_path,
                                                   recording_metadata_session_t *session);
static esp_err_t recording_metadata_write_file(const char *path, const char *text);
static esp_err_t recording_metadata_make_seed_json(const recording_metadata_session_t *session,
                                                    char *output,
                                                    size_t output_size);
static esp_err_t recording_metadata_make_final_json(const recording_metadata_session_t *session,
                                                     uint32_t duration_ms,
                                                     char *output,
                                                     size_t output_size);
static esp_err_t recording_metadata_parse_seed(const char *text,
                                                size_t length,
                                                recording_metadata_session_t *session);
static esp_err_t recording_metadata_recover_entry(const char *path, void *context);
static bool recording_metadata_has_suffix(const char *text, const char *suffix);
static esp_err_t recording_metadata_validate_final_wav(const char *path, size_t *pcm_data_bytes);

static bool recording_metadata_copy_string(const cJSON *object,
                                           const char *key,
                                           char *destination,
                                           size_t destination_size);
static esp_err_t recording_metadata_validate_published_json(const char *path,
                                                            const recording_metadata_session_t *session);

esp_err_t recording_metadata_make_id(const uint8_t random_bytes[16],
                                     char id[RECORDING_METADATA_ID_LENGTH + 1U])
{
    if (random_bytes == NULL || id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char hex[] = "0123456789abcdef";
    memcpy(id, "rec_", 4U);
    for (size_t index = 0U; index < 16U; ++index) {
        id[4U + index * 2U] = hex[random_bytes[index] >> 4U];
        id[5U + index * 2U] = hex[random_bytes[index] & 0x0fU];
    }
    id[RECORDING_METADATA_ID_LENGTH] = '\0';
    return ESP_OK;
}

esp_err_t recording_metadata_format_created_at(time_t value, char *output, size_t output_size)
{
    if (output == NULL || output_size < 26U || !recording_naming_time_is_reliable(value)) {
        return ESP_ERR_INVALID_ARG;
    }
    struct tm local_time;
    if (localtime_r(&value, &local_time) == NULL) {
        return ESP_FAIL;
    }
    char compact[32U];
    if (strftime(compact, sizeof(compact), "%Y-%m-%dT%H:%M:%S%z", &local_time) != 24U) {
        return ESP_FAIL;
    }
    const int written = snprintf(output, output_size, "%.22s:%.2s", compact, compact + 22U);
    return written < 0 || (size_t)written >= output_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t recording_metadata_duration_ms(size_t pcm_data_bytes, uint32_t *duration_ms)
{
    if (duration_ms == NULL || pcm_data_bytes == 0U || pcm_data_bytes / 32U > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *duration_ms = (uint32_t)(pcm_data_bytes / 32U);
    return ESP_OK;
}

esp_err_t recording_metadata_prepare(const char *final_wav_path,
                                     time_t created_at,
                                     recording_metadata_session_t *session)
{
    if (final_wav_path == NULL || session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(session, 0, sizeof(*session));
    const esp_err_t path_result = recording_metadata_derive_paths(final_wav_path, session);
    if (path_result != ESP_OK) {
        return path_result;
    }
    session->created_at = created_at;
    session->has_created_at = recording_naming_time_is_reliable(created_at);
    if (session->has_created_at &&
        recording_metadata_format_created_at(created_at, session->created_at_text,
                                             sizeof(session->created_at_text)) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t random_bytes[16U];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    const esp_err_t id_result = recording_metadata_make_id(random_bytes, session->id);
    if (id_result != ESP_OK) {
        return id_result;
    }
    bool exists = false;
    if (storage_file_exists(session->metadata_path, &exists) != ESP_OK || exists ||
        storage_file_exists(session->seed_path, &exists) != ESP_OK || exists) {
        return ESP_ERR_INVALID_STATE;
    }
    char seed[RECORDING_METADATA_JSON_MAX];
    const esp_err_t serialization_result = recording_metadata_make_seed_json(session, seed, sizeof(seed));
    if (serialization_result != ESP_OK) {
        return serialization_result;
    }
    const esp_err_t write_result = recording_metadata_write_file(session->seed_path, seed);
    if (write_result != ESP_OK) {
        memset(session, 0, sizeof(*session));
    }
    return write_result;
}

esp_err_t recording_metadata_finalize(const recording_metadata_session_t *session, size_t pcm_data_bytes)
{
    if (session == NULL || session->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t verified_pcm_bytes = 0U;
    esp_err_t result = recording_metadata_validate_final_wav(session->final_wav_path, &verified_pcm_bytes);
    if (result != ESP_OK || verified_pcm_bytes != pcm_data_bytes) {
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    uint32_t duration_ms = 0U;
    result = recording_metadata_duration_ms(pcm_data_bytes, &duration_ms);
    if (result != ESP_OK) {
        return result;
    }
    char json[RECORDING_METADATA_JSON_MAX];
    result = recording_metadata_make_final_json(session, duration_ms, json, sizeof(json));
    if (result != ESP_OK) {
        return result;
    }
    bool metadata_exists = false;
    result = storage_file_exists(session->metadata_path, &metadata_exists);
    if (result != ESP_OK) {
        return result;
    }
    if (metadata_exists) {
        result = recording_metadata_validate_published_json(session->metadata_path, session);
        return result == ESP_OK ? storage_file_remove(session->seed_path) : result;
    }
    char staging_path[RECORDING_METADATA_PATH_MAX];
    const int written = snprintf(staging_path, sizeof(staging_path), "%s%s", session->final_wav_path,
                                 RECORDING_METADATA_STAGING_SUFFIX);
    if (written < 0 || (size_t)written >= sizeof(staging_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    (void)storage_file_remove(staging_path);
    result = recording_metadata_write_file(staging_path, json);
    if (result == ESP_OK) {
        result = storage_file_publish(staging_path, session->metadata_path);
    }
    if (result != ESP_OK) {
        (void)storage_file_remove(staging_path);
        return result;
    }
    result = storage_file_remove(session->seed_path);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Metadata published but seed cleanup failed");
    }
    return ESP_OK;
}

esp_err_t recording_metadata_cancel(const recording_metadata_session_t *session)
{
    return session == NULL || session->seed_path[0] == '\0' ? ESP_ERR_INVALID_ARG :
        storage_file_remove(session->seed_path);
}

esp_err_t recording_metadata_recover(void)
{
    const recording_metadata_recovery_context_t context = {.suffix = RECORDING_METADATA_SEED_SUFFIX};
    return storage_recordings_iterate(recording_metadata_recover_entry, (void *)&context);
}

static esp_err_t recording_metadata_derive_paths(const char *wav_path,
                                                   recording_metadata_session_t *session)
{
    const size_t path_length = strnlen(wav_path, RECORDING_NAMING_PATH_MAX);
    if (path_length == 0U || path_length == RECORDING_NAMING_PATH_MAX ||
        path_length < 4U || strcmp(wav_path + path_length - 4U, ".wav") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(session->final_wav_path, wav_path, path_length + 1U);
    const int metadata_written = snprintf(session->metadata_path, sizeof(session->metadata_path),
                                          "%.*s%s", (int)(path_length - 4U), wav_path,
                                          RECORDING_METADATA_FINAL_SUFFIX);
    const int seed_written = snprintf(session->seed_path, sizeof(session->seed_path), "%s%s", wav_path,
                                      RECORDING_METADATA_SEED_SUFFIX);
    return metadata_written < 0 || (size_t)metadata_written >= sizeof(session->metadata_path) ||
                   seed_written < 0 || (size_t)seed_written >= sizeof(session->seed_path)
               ? ESP_ERR_INVALID_SIZE
               : ESP_OK;
}

static esp_err_t recording_metadata_write_file(const char *path, const char *text)
{
    storage_file_t *file = NULL;
    const size_t length = strlen(text);
    esp_err_t result = storage_file_create_exclusive(path, &file);
    if (result == ESP_OK) {
        result = storage_file_write(file, text, length);
    }
    if (result == ESP_OK) {
        result = storage_file_sync(file);
    }
    if (file != NULL) {
        const esp_err_t close_result = storage_file_close(file);
        if (result == ESP_OK) {
            result = close_result;
        }
    }
    if (result != ESP_OK) {
        (void)storage_file_remove(path);
    }
    return result;
}

static esp_err_t recording_metadata_make_seed_json(const recording_metadata_session_t *session,
                                                    char *output,
                                                    size_t output_size)
{
    char created_at[32U] = {0};
    if (session->has_created_at) {
        if (session->created_at_text[0] != '\0') {
            strncpy(created_at, session->created_at_text, sizeof(created_at) - 1U);
        } else if (recording_metadata_format_created_at(session->created_at, created_at,
                                                        sizeof(created_at)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    const char *basename = strrchr(session->final_wav_path, '/');
    if (basename == NULL || basename[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = session->has_created_at
                            ? snprintf(output, output_size, "{\"id\":\"%s\",\"file\":\"%s\",\"createdAt\":\"%s\"}",
                                       session->id, basename + 1U, created_at)
                            : snprintf(output, output_size, "{\"id\":\"%s\",\"file\":\"%s\",\"createdAt\":null}",
                                       session->id, basename + 1U);
    return written < 0 || (size_t)written >= output_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t recording_metadata_make_final_json(const recording_metadata_session_t *session,
                                                     uint32_t duration_ms,
                                                     char *output,
                                                     size_t output_size)
{
    char seed[RECORDING_METADATA_JSON_MAX];
    const esp_err_t seed_result = recording_metadata_make_seed_json(session, seed, sizeof(seed));
    if (seed_result != ESP_OK) {
        return seed_result;
    }
    const size_t prefix_length = strlen(seed) - 1U;
    memcpy(output, seed, prefix_length);
    const int written = snprintf(output + prefix_length, output_size - prefix_length,
                                 ",\"durationMs\":%u,\"status\":\"pending\",\"meetingId\":null,\"syncedAt\":null}",
                                 (unsigned)duration_ms);
    if (written < 0 || (size_t)written >= output_size - prefix_length) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t recording_metadata_parse_seed(const char *text,
                                                size_t length,
                                                recording_metadata_session_t *session)
{
    if (text == NULL || length == 0U || session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, length, &end, false);
    const cJSON *created_at = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "createdAt");
    if (root == NULL || end == NULL || !cJSON_IsObject(root) || *end != '\0' ||
        !recording_metadata_copy_string(root, "id", session->id, sizeof(session->id)) ||
        !recording_metadata_copy_string(root, "file", session->final_wav_path, sizeof(session->final_wav_path)) ||
        (!cJSON_IsNull(created_at) && !cJSON_IsString(created_at))) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    session->has_created_at = cJSON_IsString(created_at);
    if (session->has_created_at) {
        const size_t created_length = strnlen(created_at->valuestring, sizeof(session->created_at_text));
        if (created_length == 0U || created_length == sizeof(session->created_at_text)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(session->created_at_text, created_at->valuestring, created_length + 1U);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t recording_metadata_recover_entry(const char *path, void *context)
{
    const recording_metadata_recovery_context_t *recovery = context;
    if (recovery == NULL || !recording_metadata_has_suffix(path, recovery->suffix)) {
        return ESP_OK;
    }
    char seed[RECORDING_METADATA_JSON_MAX];
    size_t seed_size = 0U;
    esp_err_t result = storage_file_get_size(path, &seed_size);
    if (result != ESP_OK || seed_size == 0U || seed_size >= sizeof(seed)) {
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    size_t seed_length = 0U;
    result = storage_file_read(path, seed, seed_size, &seed_length);
    if (result != ESP_OK) {
        return result;
    }
    seed[seed_length] = '\0';
    recording_metadata_session_t session = {0};
    result = recording_metadata_parse_seed(seed, seed_length, &session);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Malformed metadata seed %s", path);
        return result;
    }
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(recovery->suffix);
    if (path_length <= suffix_length || path_length - suffix_length >= sizeof(session.final_wav_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(session.final_wav_path, path, path_length - suffix_length);
    session.final_wav_path[path_length - suffix_length] = '\0';
    result = recording_metadata_derive_paths(session.final_wav_path, &session);
    if (result != ESP_OK) {
        return result;
    }
    size_t pcm_data_bytes = 0U;
    result = recording_metadata_validate_final_wav(session.final_wav_path, &pcm_data_bytes);
    if (result == ESP_FAIL) {
        ESP_LOGW(TAG, "Removing orphaned metadata seed %s", path);
        return storage_file_remove(path);
    }
    if (result != ESP_OK) {
        return result;
    }
    ESP_LOGI(TAG, "Recovering metadata for %s", session.final_wav_path);
    return recording_metadata_finalize(&session, pcm_data_bytes);
}

static bool recording_metadata_has_suffix(const char *text, const char *suffix)
{
    const size_t text_length = strlen(text);
    const size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length && strcmp(text + text_length - suffix_length, suffix) == 0;
}

static esp_err_t recording_metadata_validate_final_wav(const char *path, size_t *pcm_data_bytes)
{
    size_t file_size = 0U;
    const esp_err_t size_result = storage_file_get_size(path, &file_size);
    if (size_result != ESP_OK) {
        return ESP_FAIL;
    }
    if (file_size <= 44U) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t header[44U];
    size_t header_size = 0U;
    const esp_err_t read_result = storage_file_read(path, header, sizeof(header), &header_size);
    if (read_result != ESP_OK || header_size != sizeof(header) || memcmp(header, "RIFF", 4U) != 0 ||
        memcmp(&header[8], "WAVE", 4U) != 0 || memcmp(&header[36], "data", 4U) != 0) {
        return read_result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : read_result;
    }
    const uint32_t declared_size = (uint32_t)header[40] | ((uint32_t)header[41] << 8U) |
                                   ((uint32_t)header[42] << 16U) | ((uint32_t)header[43] << 24U);
    if ((size_t)declared_size != file_size - sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *pcm_data_bytes = file_size - sizeof(header);
    return ESP_OK;
}


static esp_err_t recording_metadata_validate_published_json(const char *path,
                                                            const recording_metadata_session_t *session)
{
    char json[RECORDING_METADATA_JSON_MAX];
    size_t json_size = 0U;
    esp_err_t result = storage_file_get_size(path, &json_size);
    if (result != ESP_OK || json_size == 0U || json_size >= sizeof(json)) {
        return result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
    }
    size_t length = 0U;
    result = storage_file_read(path, json, json_size, &length);
    if (result != ESP_OK) {
        return result;
    }
    json[length] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    const cJSON *duration = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "durationMs");
    const cJSON *status = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "status");
    char id[RECORDING_METADATA_ID_LENGTH + 1U] = {0};
    char file[RECORDING_NAMING_PATH_MAX] = {0};
    const char *basename = strrchr(session->final_wav_path, '/');
    const bool valid = root != NULL && end != NULL && *end == '\0' && cJSON_IsObject(root) &&
                       recording_metadata_copy_string(root, "id", id, sizeof(id)) &&
                       recording_metadata_copy_string(root, "file", file, sizeof(file)) &&
                       basename != NULL && strcmp(id, session->id) == 0 &&
                       strcmp(file, basename + 1U) == 0 && cJSON_IsNumber(duration) &&
                       duration->valuedouble > 0.0 && cJSON_IsString(status) &&
                       strcmp(status->valuestring, "pending") == 0;
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static bool recording_metadata_copy_string(const cJSON *object,
                                           const char *key,
                                           char *destination,
                                           size_t destination_size)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || value->valuestring == NULL || value->valuestring[0] == '\0') {
        return false;
    }
    const size_t length = strnlen(value->valuestring, destination_size);
    if (length == destination_size) {
        return false;
    }
    memcpy(destination, value->valuestring, length + 1U);
    return true;
}
