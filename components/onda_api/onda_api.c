#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "onda_api.h"
#include "onda_api_config.h"
#include "storage.h"

#define ONDA_API_COMMAND_QUEUE_LENGTH 1U
#define ONDA_API_TASK_STACK_SIZE 4096U
#define ONDA_API_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_API_REQUEST_TIMEOUT_MS 5000U
#define ONDA_API_UPLOAD_TIMEOUT_MS (60U * 1000U)
#define ONDA_API_ENDPOINT_MAX_LENGTH 192U
#define ONDA_API_TOKEN_MAX_LENGTH 128U
#define ONDA_API_AUTHORIZATION_MAX_LENGTH (sizeof("Bearer ") + ONDA_API_TOKEN_MAX_LENGTH)
#define ONDA_API_RESPONSE_MAX_LENGTH 512U
#define ONDA_API_SYNC_RESPONSE_MAX_LENGTH 1536U
#define ONDA_API_BLOB_URL_MAX_LENGTH 1152U
#define ONDA_API_UPLOAD_BUFFER_SIZE 4096U
#define ONDA_API_UPLOAD_REQUEST_BUFFER_SIZE 1536U
#define ONDA_API_UPLOAD_HEADER_COUNT_MAX 8U
#define ONDA_API_UPLOAD_HEADER_NAME_MAX 64U
#define ONDA_API_UPLOAD_HEADER_VALUE_MAX 128U

static const char *TAG = "ONDA_API";

typedef enum {
    ONDA_API_COMMAND_GET_DEVICE_IDENTITY,
} onda_api_command_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflowed;
} onda_api_response_capture_t;

typedef struct {
    char name[ONDA_API_UPLOAD_HEADER_NAME_MAX];
    char value[ONDA_API_UPLOAD_HEADER_VALUE_MAX];
} onda_api_upload_header_t;

typedef struct {
    char url[ONDA_API_BLOB_URL_MAX_LENGTH];
    onda_api_upload_header_t headers[ONDA_API_UPLOAD_HEADER_COUNT_MAX];
    size_t header_count;
} onda_api_upload_descriptor_t;

static uint8_t s_upload_buffer[ONDA_API_UPLOAD_BUFFER_SIZE];
static char s_identity_response_data[ONDA_API_RESPONSE_MAX_LENGTH + 1U];
static char s_sync_response_data[ONDA_API_SYNC_RESPONSE_MAX_LENGTH + 1U];
static onda_api_upload_descriptor_t s_upload_descriptor;

static QueueHandle_t s_command_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static onda_api_state_t s_state = ONDA_API_NOT_CHECKED;
static bool s_initialized;
static bool s_identity_check_requested;

static void onda_api_task(void *context);
static esp_err_t onda_api_get_device_identity(void);
static esp_err_t onda_api_build_endpoint(char *endpoint, size_t endpoint_size);
static esp_err_t onda_api_build_endpoint_path(const char *path, char *endpoint, size_t endpoint_size);
static esp_err_t onda_api_build_authorization(char *authorization, size_t authorization_size);
static esp_err_t onda_api_response_event_handler(esp_http_client_event_t *event);
static bool onda_api_copy_json_string(const cJSON *object,
                                      const char *key,
                                      char *destination,
                                      size_t destination_size);
static bool onda_api_remaining_is_whitespace(const char *text);
static void onda_api_set_state(onda_api_state_t state);
static esp_err_t onda_api_post_json(const char *path,
                                    const char *body,
                                    onda_api_response_capture_t *response,
                                    int *status);
static esp_err_t onda_api_parse_initiate(const char *response,
                                         size_t response_length,
                                         const char *recording_id,
                                         onda_api_upload_descriptor_t *upload,
                                         onda_api_sync_result_t *result,
                                         bool *upload_required);
static esp_err_t onda_api_upload_wav(const recording_metadata_record_t *record,
                                     const onda_api_upload_descriptor_t *upload,
                                     int *status);
static bool onda_api_copy_bounded_string(const cJSON *value, char *destination, size_t destination_size);
static bool onda_api_is_rfc3339(const char *value);
static bool onda_api_is_safe_identifier(const char *value, size_t maximum_length);

esp_err_t onda_api_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_command_queue = xQueueCreate(ONDA_API_COMMAND_QUEUE_LENGTH, sizeof(onda_api_command_t));
    if (s_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create API command queue");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(onda_api_task, "onda_api", ONDA_API_TASK_STACK_SIZE, NULL,
                    ONDA_API_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create API worker");
        vQueueDelete(s_command_queue);
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Device API initialized");
    return ESP_OK;
}

esp_err_t onda_api_get_device_identity_async(void)
{
    if (!s_initialized || s_command_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (s_identity_check_requested) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_identity_check_requested = true;
    s_state = ONDA_API_CHECKING;
    portEXIT_CRITICAL(&s_state_lock);

    const onda_api_command_t command = ONDA_API_COMMAND_GET_DEVICE_IDENTITY;
    if (xQueueSend(s_command_queue, &command, 0) != pdPASS) {
        onda_api_set_state(ONDA_API_ERROR);
        ESP_LOGE(TAG, "Failed to queue device identity check");
        return ESP_FAIL;
    }
    return ESP_OK;
}

onda_api_state_t onda_api_get_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const onda_api_state_t state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

esp_err_t onda_api_parse_device_identity(const char *response,
                                         size_t response_length,
                                         onda_api_device_identity_t *identity)
{
    if (response == NULL || response_length == 0U || identity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(identity, 0, sizeof(*identity));
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(response, response_length, &parse_end, false);
    if (root == NULL || parse_end == NULL || !cJSON_IsObject(root) ||
        !onda_api_remaining_is_whitespace(parse_end) ||
        !onda_api_copy_json_string(root, "deviceId", identity->device_id, sizeof(identity->device_id)) ||
        !onda_api_copy_json_string(root, "name", identity->name, sizeof(identity->name)) ||
        !onda_api_copy_json_string(root, "serverTime", identity->server_time, sizeof(identity->server_time))) {
        cJSON_Delete(root);
        memset(identity, 0, sizeof(*identity));
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

onda_api_state_t onda_api_resolve_state(esp_err_t request_result,
                                        int http_status,
                                        bool response_is_valid)
{
    if (http_status == 401 || http_status == 403) {
        return ONDA_API_UNAUTHORIZED;
    }
    if (request_result != ESP_OK) {
        return ONDA_API_ERROR;
    }
    if (http_status == 200 && response_is_valid) {
        return ONDA_API_AUTHENTICATED;
    }
    return ONDA_API_ERROR;
}

esp_err_t onda_api_sync_recording(const recording_metadata_record_t *record,
                                  onda_api_sync_result_t *result)
{
    if (record == NULL || result == NULL || record->status != RECORDING_METADATA_STATUS_PENDING ||
        record->size_bytes == 0U || record->duration_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    result->outcome = ONDA_API_SYNC_RECORD_FAILURE;
    char created_at[256U];
    const int body_written = record->has_created_at
                                 ? snprintf(created_at, sizeof(created_at),
                                            "{\"recordingId\":\"%s\",\"createdAt\":\"%s\",\"durationMs\":%u,\"sizeBytes\":%zu}",
                                            record->id, record->created_at, (unsigned)record->duration_ms,
                                            record->size_bytes)
                                 : snprintf(created_at, sizeof(created_at),
                                            "{\"recordingId\":\"%s\",\"createdAt\":null,\"durationMs\":%u,\"sizeBytes\":%zu}",
                                            record->id, (unsigned)record->duration_ms, record->size_bytes);
    if (body_written < 0 || (size_t)body_written >= sizeof(created_at)) {
        return ESP_ERR_INVALID_SIZE;
    }
    onda_api_response_capture_t response = {
        .data = s_sync_response_data,
        .capacity = sizeof(s_sync_response_data),
    };
    int status = 0;
    esp_err_t request_result = onda_api_post_json("/api/device/recordings/initiate", created_at, &response, &status);
    ESP_LOGI(TAG, "Recording initiate returned HTTP %d", status);
    if (status == 401 || status == 403) {
        result->outcome = ONDA_API_SYNC_AUTH_FAILURE;
        return ESP_OK;
    }
    if (request_result != ESP_OK || status >= 500 || status == 0) {
        result->outcome = ONDA_API_SYNC_CONNECTION_FAILURE;
        return ESP_OK;
    }
    if (status != 200 || response.overflowed) {
        return ESP_OK;
    }
    memset(&s_upload_descriptor, 0, sizeof(s_upload_descriptor));
    bool upload_required = false;
    const esp_err_t initiate_parse_result = onda_api_parse_initiate(response.data, response.length, record->id,
                                                                      &s_upload_descriptor, result, &upload_required);
    if (initiate_parse_result != ESP_OK) {
        ESP_LOGW(TAG, "Recording initiate response rejected: %s", esp_err_to_name(initiate_parse_result));
        return ESP_OK;
    }
    if (!upload_required) {
        ESP_LOGI(TAG, "Recording was already uploaded");
        result->outcome = ONDA_API_SYNC_SUCCESS;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Uploading %u WAV bytes to signed Blob URL", (unsigned)record->size_bytes);
    request_result = onda_api_upload_wav(record, &s_upload_descriptor, &status);
    ESP_LOGI(TAG, "Signed Blob upload returned HTTP %d", status);
    if (request_result != ESP_OK || status == 0) {
        ESP_LOGW(TAG, "Signed Blob upload failed: %s", esp_err_to_name(request_result));
        if (status < 0) {
            return ESP_OK;
        }
        result->outcome = ONDA_API_SYNC_CONNECTION_FAILURE;
        return ESP_OK;
    }
    if (status < 200 || status >= 300) {
        return ESP_OK;
    }
    const int complete_written = snprintf(created_at, sizeof(created_at), "{\"recordingId\":\"%s\"}", record->id);
    if (complete_written < 0 || (size_t)complete_written >= sizeof(created_at)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(s_sync_response_data, 0, sizeof(s_sync_response_data));
    response.data = s_sync_response_data;
    response.capacity = sizeof(s_sync_response_data);
    response.length = 0U;
    response.overflowed = false;
    status = 0;
    request_result = onda_api_post_json("/api/device/recordings/complete", created_at, &response, &status);
    ESP_LOGI(TAG, "Recording complete returned HTTP %d", status);
    if (status == 401 || status == 403) {
        result->outcome = ONDA_API_SYNC_AUTH_FAILURE;
        return ESP_OK;
    }
    if (request_result != ESP_OK || status >= 500 || status == 0) {
        result->outcome = ONDA_API_SYNC_CONNECTION_FAILURE;
        return ESP_OK;
    }
    const esp_err_t complete_parse_result =
        status == 200 && !response.overflowed
            ? onda_api_parse_sync_success(response.data, response.length, record->id, result)
            : ESP_ERR_INVALID_RESPONSE;
    if (complete_parse_result == ESP_OK) {
        result->outcome = ONDA_API_SYNC_SUCCESS;
    } else {
        ESP_LOGW(TAG, "Recording complete response rejected: %s", esp_err_to_name(complete_parse_result));
    }
    return ESP_OK;
}

static void onda_api_task(void *context)
{
    (void)context;
    for (;;) {
        onda_api_command_t command;
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command == ONDA_API_COMMAND_GET_DEVICE_IDENTITY) {
            (void)onda_api_get_device_identity();
        }
    }
}

static esp_err_t onda_api_get_device_identity(void)
{
    char endpoint[ONDA_API_ENDPOINT_MAX_LENGTH];
    char authorization[ONDA_API_AUTHORIZATION_MAX_LENGTH];
    esp_err_t result = onda_api_build_endpoint(endpoint, sizeof(endpoint));
    if (result != ESP_OK) {
        onda_api_set_state(ONDA_API_ERROR);
        ESP_LOGE(TAG, "Device API endpoint configuration is invalid");
        return result;
    }
    result = onda_api_build_authorization(authorization, sizeof(authorization));
    if (result != ESP_OK) {
        onda_api_set_state(ONDA_API_ERROR);
        ESP_LOGE(TAG, "Device API credential configuration is invalid");
        return result;
    }

    memset(s_identity_response_data, 0, sizeof(s_identity_response_data));
    onda_api_response_capture_t response = {
        .data = s_identity_response_data,
        .capacity = sizeof(s_identity_response_data),
    };
    const esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ONDA_API_REQUEST_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .buffer_size = 256,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = onda_api_response_event_handler,
        .user_data = &response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        onda_api_set_state(ONDA_API_ERROR);
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_ERR_NO_MEM;
    }

    result = esp_http_client_set_header(client, "Authorization", authorization);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Checking device identity");
        ESP_LOGI(TAG, "GET /api/device/me");
        result = esp_http_client_perform(client);
    }
    const int status = esp_http_client_get_status_code(client);

    onda_api_device_identity_t identity = {0};
    const bool response_valid = result == ESP_OK && status == 200 && !response.overflowed &&
                                onda_api_parse_device_identity(response.data, response.length, &identity) == ESP_OK;
    const onda_api_state_t state = onda_api_resolve_state(result, status, response_valid);
    onda_api_set_state(state);

    if (status > 0) {
        ESP_LOGI(TAG, "Response %d", status);
    }
    if (result != ESP_OK && state != ONDA_API_UNAUTHORIZED) {
        ESP_LOGW(TAG, "Device identity request failed: %s", esp_err_to_name(result));
    }
    if (state == ONDA_API_AUTHENTICATED) {
        ESP_LOGI(TAG, "Device authenticated");
        ESP_LOGI(TAG, "Device ID: %s", identity.device_id);
        ESP_LOGI(TAG, "Device name: %s", identity.name);
        ESP_LOGI(TAG, "Server time: %s", identity.server_time);
    } else if (state == ONDA_API_UNAUTHORIZED) {
        if (status == 403) {
            ESP_LOGW(TAG, "Device not allowed");
        } else {
            ESP_LOGW(TAG, "Unauthorized");
        }
    } else {
        ESP_LOGW(TAG, "Device API check failed");
    }

    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t onda_api_build_endpoint(char *endpoint, size_t endpoint_size)
{
    return onda_api_build_endpoint_path("/api/device/me", endpoint, endpoint_size);
}

static esp_err_t onda_api_build_endpoint_path(const char *path, char *endpoint, size_t endpoint_size)
{
    if (path == NULL || path[0] != '/' || endpoint == NULL || endpoint_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t base_length = strlen(ONDA_API_BASE_URL);
    if (base_length == 0U || base_length >= ONDA_API_ENDPOINT_MAX_LENGTH ||
        (strncmp(ONDA_API_BASE_URL, "http://", 7U) != 0 &&
         strncmp(ONDA_API_BASE_URL, "https://", 8U) != 0) ||
        strpbrk(ONDA_API_BASE_URL, "?#") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t trimmed_length = base_length;
    while (trimmed_length > 0U && ONDA_API_BASE_URL[trimmed_length - 1U] == '/') {
        --trimmed_length;
    }
    if (trimmed_length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(endpoint, endpoint_size, "%.*s%s",
                                 (int)trimmed_length, ONDA_API_BASE_URL, path);
    return written < 0 || (size_t)written >= endpoint_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t onda_api_build_authorization(char *authorization, size_t authorization_size)
{
    if (authorization == NULL || authorization_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t token_length = strlen(ONDA_API_DEVICE_TOKEN);
    if (token_length == 0U || token_length > ONDA_API_TOKEN_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(authorization, authorization_size, "Bearer %s", ONDA_API_DEVICE_TOKEN);
    return written < 0 || (size_t)written >= authorization_size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t onda_api_response_event_handler(esp_http_client_event_t *event)
{
    if (event == NULL || event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    onda_api_response_capture_t *response = event->user_data;
    if (response == NULL || event->data == NULL || event->data_len < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (response->data == NULL || response->capacity == 0U ||
        (size_t)event->data_len >= response->capacity - response->length) {
        response->overflowed = true;
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(response->data + response->length, event->data, (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static bool onda_api_copy_json_string(const cJSON *object,
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

static bool onda_api_remaining_is_whitespace(const char *text)
{
    if (text == NULL) {
        return false;
    }
    while (*text != '\0') {
        if (!isspace((unsigned char)*text)) {
            return false;
        }
        ++text;
    }
    return true;
}

static void onda_api_set_state(onda_api_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t onda_api_post_json(const char *path,
                                    const char *body,
                                    onda_api_response_capture_t *response,
                                    int *status)
{
    if (path == NULL || body == NULL || response == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = 0;
    char endpoint[ONDA_API_ENDPOINT_MAX_LENGTH];
    char authorization[ONDA_API_AUTHORIZATION_MAX_LENGTH];
    esp_err_t result = onda_api_build_endpoint_path(path, endpoint, sizeof(endpoint));
    if (result != ESP_OK) {
        return result;
    }
    result = onda_api_build_authorization(authorization, sizeof(authorization));
    if (result != ESP_OK) {
        return result;
    }
    const esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = ONDA_API_REQUEST_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size = 512,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = onda_api_response_event_handler,
        .user_data = response,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    result = esp_http_client_set_header(client, "Authorization", authorization);
    if (result == ESP_OK) {
        result = esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    if (result == ESP_OK) {
        result = esp_http_client_set_post_field(client, body, (int)strlen(body));
    }
    if (result == ESP_OK) {
        result = esp_http_client_perform(client);
    }
    *status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return result;
}

static esp_err_t onda_api_parse_initiate(const char *response,
                                         size_t response_length,
                                         const char *recording_id,
                                         onda_api_upload_descriptor_t *upload,
                                         onda_api_sync_result_t *result,
                                         bool *upload_required)
{
    if (response == NULL || response_length == 0U || recording_id == NULL || upload == NULL ||
        result == NULL || upload_required == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *upload_required = false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(response, response_length, &end, false);
    const cJSON *returned_id = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "recordingId");
    const cJSON *upload_json = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "upload");
    const bool base_valid = root != NULL && end != NULL && *end == '\0' && cJSON_IsObject(root) &&
                            cJSON_IsString(returned_id) && strcmp(returned_id->valuestring, recording_id) == 0;
    if (!base_valid) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!cJSON_IsObject(upload_json)) {
        const esp_err_t parsed = onda_api_parse_sync_success(response, response_length, recording_id, result);
        cJSON_Delete(root);
        return parsed;
    }
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(upload_json, "url");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(upload_json, "method");
    const cJSON *headers = cJSON_GetObjectItemCaseSensitive(upload_json, "headers");
    const cJSON *expires_at = cJSON_GetObjectItemCaseSensitive(upload_json, "expiresAt");
    const bool upload_valid = cJSON_IsString(url) && url->valuestring != NULL &&
                              strncmp(url->valuestring, "https://", 8U) == 0 &&
                              cJSON_IsString(method) && strcmp(method->valuestring, "PUT") == 0 &&
                              cJSON_IsObject(headers) &&
                              cJSON_IsString(expires_at) && onda_api_is_rfc3339(expires_at->valuestring) &&
                              onda_api_copy_bounded_string(url, upload->url, sizeof(upload->url));
    if (!upload_valid) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *header = NULL;
    cJSON_ArrayForEach(header, headers)
    {
        if (upload->header_count >= ONDA_API_UPLOAD_HEADER_COUNT_MAX || header->string == NULL ||
            !cJSON_IsString(header) || strchr(header->string, '\r') != NULL ||
            strchr(header->string, '\n') != NULL || strchr(header->valuestring, '\r') != NULL ||
            strchr(header->valuestring, '\n') != NULL) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t name_length = strnlen(header->string, sizeof(upload->headers[0].name));
        const size_t value_length = strnlen(header->valuestring, sizeof(upload->headers[0].value));
        if (name_length == 0U || name_length == sizeof(upload->headers[0].name) ||
            value_length == 0U || value_length == sizeof(upload->headers[0].value)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }
        memcpy(upload->headers[upload->header_count].name, header->string, name_length + 1U);
        memcpy(upload->headers[upload->header_count].value, header->valuestring, value_length + 1U);
        ++upload->header_count;
    }
    cJSON_Delete(root);
    if (upload->header_count == 0U) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *upload_required = true;
    return ESP_OK;
}

esp_err_t onda_api_parse_sync_success(const char *response,
                                      size_t response_length,
                                      const char *recording_id,
                                      onda_api_sync_result_t *result)
{
    if (response == NULL || response_length == 0U || recording_id == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(response, response_length, &end, false);
    const cJSON *returned_id = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "recordingId");
    const cJSON *meeting_id = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "meetingId");
    const cJSON *status = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *created = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "created");
    const cJSON *synced_at = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "syncedAt");
    const bool valid = root != NULL && end != NULL && *end == '\0' && cJSON_IsObject(root) &&
                       cJSON_IsString(returned_id) && strcmp(returned_id->valuestring, recording_id) == 0 &&
                       cJSON_IsString(meeting_id) && onda_api_copy_bounded_string(meeting_id,
                                                                                  result->meeting_id,
                                                                                  sizeof(result->meeting_id)) &&
                       onda_api_is_safe_identifier(result->meeting_id, ONDA_API_MEETING_ID_MAX_LENGTH) &&
                       cJSON_IsString(status) && strcmp(status->valuestring, "uploaded") == 0 &&
                       cJSON_IsBool(created) && cJSON_IsString(synced_at) &&
                       onda_api_copy_bounded_string(synced_at, result->synced_at, sizeof(result->synced_at)) &&
                       onda_api_is_rfc3339(result->synced_at);
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t onda_api_upload_wav(const recording_metadata_record_t *record,
                                     const onda_api_upload_descriptor_t *upload,
                                     int *status)
{
    if (record == NULL || upload == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = -1;
    storage_file_t *file = NULL;
    esp_err_t result = storage_file_open_read(record->wav_path, &file);
    if (result != ESP_OK) {
        return result;
    }
    const esp_http_client_config_t config = {
        .url = upload->url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = ONDA_API_UPLOAD_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .buffer_size = ONDA_API_UPLOAD_BUFFER_SIZE,
        .buffer_size_tx = ONDA_API_UPLOAD_REQUEST_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        (void)storage_file_close(file);
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0U; index < upload->header_count && result == ESP_OK; ++index) {
        result = esp_http_client_set_header(client, upload->headers[index].name, upload->headers[index].value);
    }
    if (result == ESP_OK) {
        result = esp_http_client_open(client, (int)record->size_bytes);
    }
    size_t uploaded = 0U;
    while (result == ESP_OK && uploaded < record->size_bytes) {
        size_t read = 0U;
        result = storage_file_read_next(file, s_upload_buffer, sizeof(s_upload_buffer), &read);
        if (result != ESP_OK || read == 0U || read > record->size_bytes - uploaded) {
            result = result == ESP_OK ? ESP_ERR_INVALID_SIZE : result;
            break;
        }
        size_t offset = 0U;
        while (result == ESP_OK && offset < read) {
            const int written = esp_http_client_write(client, (const char *)s_upload_buffer + offset,
                                                      (int)(read - offset));
            if (written <= 0) {
                result = ESP_FAIL;
                break;
            }
            offset += (size_t)written;
        }
        uploaded += read;
    }
    if (result == ESP_OK && uploaded == record->size_bytes) {
        (void)esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
    }
    const esp_err_t close_result = storage_file_close(file);
    if (result == ESP_OK && close_result != ESP_OK) {
        result = close_result;
        *status = -1;
    }
    esp_http_client_cleanup(client);
    return result;
}

static bool onda_api_copy_bounded_string(const cJSON *value, char *destination, size_t destination_size)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL || destination == NULL || destination_size == 0U) {
        return false;
    }
    const size_t length = strnlen(value->valuestring, destination_size);
    if (length == 0U || length == destination_size) {
        return false;
    }
    memcpy(destination, value->valuestring, length + 1U);
    return true;
}

static bool onda_api_is_rfc3339(const char *value)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strlen(value);
    if (length < 20U || length > ONDA_API_SYNCED_AT_MAX_LENGTH || value[4] != '-' || value[7] != '-' ||
        value[10] != 'T' || value[13] != ':' || value[16] != ':') {
        return false;
    }
    const size_t digits[] = {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U};
    for (size_t index = 0U; index < sizeof(digits) / sizeof(digits[0]); ++index) {
        if (value[digits[index]] < '0' || value[digits[index]] > '9') {
            return false;
        }
    }
    size_t timezone = 19U;
    if (value[timezone] == '.') {
        ++timezone;
        const size_t start = timezone;
        while (timezone < length && value[timezone] >= '0' && value[timezone] <= '9') {
            ++timezone;
        }
        if (timezone == start) {
            return false;
        }
    }
    return timezone + 1U == length && value[timezone] == 'Z';
}

static bool onda_api_is_safe_identifier(const char *value, size_t maximum_length)
{
    if (value == NULL || value[0] == '\0' || strnlen(value, maximum_length + 1U) > maximum_length) {
        return false;
    }
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        const char character = value[index];
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}
