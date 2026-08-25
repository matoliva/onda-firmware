#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "onda_api.h"
#include "onda_api_config.h"

#define ONDA_API_COMMAND_QUEUE_LENGTH 1U
#define ONDA_API_TASK_STACK_SIZE 4096U
#define ONDA_API_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define ONDA_API_REQUEST_TIMEOUT_MS 5000U
#define ONDA_API_ENDPOINT_MAX_LENGTH 192U
#define ONDA_API_TOKEN_MAX_LENGTH 128U
#define ONDA_API_AUTHORIZATION_MAX_LENGTH (sizeof("Bearer ") + ONDA_API_TOKEN_MAX_LENGTH)
#define ONDA_API_RESPONSE_MAX_LENGTH 512U

static const char *TAG = "ONDA_API";

typedef enum {
    ONDA_API_COMMAND_GET_DEVICE_IDENTITY,
} onda_api_command_t;

typedef struct {
    char data[ONDA_API_RESPONSE_MAX_LENGTH + 1U];
    size_t length;
    bool overflowed;
} onda_api_response_capture_t;

static QueueHandle_t s_command_queue;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static onda_api_state_t s_state = ONDA_API_NOT_CHECKED;
static bool s_initialized;
static bool s_identity_check_requested;

static void onda_api_task(void *context);
static esp_err_t onda_api_get_device_identity(void);
static esp_err_t onda_api_build_endpoint(char *endpoint, size_t endpoint_size);
static esp_err_t onda_api_build_authorization(char *authorization, size_t authorization_size);
static esp_err_t onda_api_response_event_handler(esp_http_client_event_t *event);
static bool onda_api_copy_json_string(const cJSON *object,
                                      const char *key,
                                      char *destination,
                                      size_t destination_size);
static bool onda_api_remaining_is_whitespace(const char *text);
static void onda_api_set_state(onda_api_state_t state);

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

    onda_api_response_capture_t response = {0};
    const esp_http_client_config_t config = {
        .url = endpoint,
        .method = HTTP_METHOD_GET,
        .timeout_ms = ONDA_API_REQUEST_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .max_authorization_retries = -1,
        .buffer_size = 256,
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
    if (endpoint == NULL || endpoint_size == 0U) {
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
    const int written = snprintf(endpoint, endpoint_size, "%.*s/api/device/me",
                                 (int)trimmed_length, ONDA_API_BASE_URL);
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
    if ((size_t)event->data_len > ONDA_API_RESPONSE_MAX_LENGTH - response->length) {
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
