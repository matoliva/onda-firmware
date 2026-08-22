#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wifi.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#define WIFI_POP_NAMESPACE "onda"
#define WIFI_POP_KEY "wifi_pop"
#define WIFI_POP_LENGTH 8U
#define WIFI_POP_BUFFER_LENGTH (WIFI_POP_LENGTH + 1U)
#define WIFI_SERVICE_NAME_LENGTH 16U
#define WIFI_MAX_CONNECTION_ATTEMPTS 3U
#define WIFI_OFFLINE_RETRY_DELAY_US (5ULL * 60ULL * 1000ULL * 1000ULL)

static const char *TAG = "ONDA_WIFI";

static const uint64_t s_retry_delays_us[WIFI_MAX_CONNECTION_ATTEMPTS] = {
    1000ULL * 1000ULL,
    2ULL * 1000ULL * 1000ULL,
    4ULL * 1000ULL * 1000ULL,
};

static wifi_state_callback_t s_state_callback;
static void *s_state_context;
static esp_timer_handle_t s_retry_timer;
static wifi_state_t s_state = WIFI_STATE_UNCONFIGURED;
static char s_pop[WIFI_POP_BUFFER_LENGTH];
static bool s_started;
static bool s_station_started;
static bool s_provisioning_active;
static uint8_t s_connection_attempts;

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);
static void wifi_retry_timer_callback(void *argument);
static esp_err_t wifi_load_or_create_pop(void);
static esp_err_t wifi_start_provisioning(void);
static esp_err_t wifi_start_station(void);
static esp_err_t wifi_schedule_connection_attempt(uint64_t delay_us);
static void wifi_enter_offline(void);
static void wifi_notify_state(wifi_state_t state, const char *pop);
static void wifi_report_error(const char *operation, esp_err_t error);
static void wifi_get_service_name(char *service_name, size_t length);

esp_err_t wifi_start(wifi_state_callback_t callback, void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_state_callback = callback;
    s_state_context = context;

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        wifi_report_error("NVS initialisation", result);
        return result;
    }

    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        wifi_report_error("Network interface initialisation", result);
        return result;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        wifi_report_error("Event loop initialisation", result);
        return result;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        wifi_report_error("Station network interface creation", ESP_FAIL);
        return ESP_FAIL;
    }

    /* The ESP-IDF Wi-Fi driver can log the connected SSID at INFO level. */
    esp_log_level_set("wifi", ESP_LOG_WARN);

    const wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi initialisation", result);
        return result;
    }

    result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi persistent storage setup", result);
        return result;
    }

    result = esp_event_handler_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        wifi_event_handler,
                                        NULL);
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT,
                                            IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler,
                                            NULL);
    }
    if (result == ESP_OK) {
        result = esp_event_handler_register(WIFI_PROV_EVENT,
                                            ESP_EVENT_ANY_ID,
                                            wifi_event_handler,
                                            NULL);
    }
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi event registration", result);
        return result;
    }

    const esp_timer_create_args_t retry_timer_config = {
        .callback = wifi_retry_timer_callback,
        .name = "onda_wifi_retry",
    };
    result = esp_timer_create(&retry_timer_config, &s_retry_timer);
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi retry timer creation", result);
        return result;
    }

    s_started = true;
    wifi_notify_state(WIFI_STATE_UNCONFIGURED, NULL);

    wifi_prov_mgr_config_t provisioning_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    result = wifi_prov_mgr_init(provisioning_config);
    if (result != ESP_OK) {
        wifi_report_error("Provisioning manager initialisation", result);
        return result;
    }
    s_provisioning_active = true;

    bool provisioned = false;
    result = wifi_prov_mgr_is_provisioned(&provisioned);
    if (result != ESP_OK) {
        wifi_report_error("Stored Wi-Fi configuration check", result);
        return result;
    }

    if (!provisioned) {
        return wifi_start_provisioning();
    }

    wifi_prov_mgr_deinit();
    s_provisioning_active = false;
    return wifi_start_station();
}

esp_err_t wifi_request_reprovision(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
    }
    s_connection_attempts = 0;

    if (s_provisioning_active) {
        wifi_prov_mgr_deinit();
        s_provisioning_active = false;
    }

    if (s_station_started) {
        const esp_err_t stop_result = esp_wifi_stop();
        if (stop_result != ESP_OK && stop_result != ESP_ERR_WIFI_NOT_STARTED) {
            wifi_report_error("Wi-Fi station stop", stop_result);
            return stop_result;
        }
        s_station_started = false;
    }

    const esp_err_t restore_result = esp_wifi_restore();
    if (restore_result != ESP_OK) {
        wifi_report_error("Wi-Fi credential reset", restore_result);
        return restore_result;
    }

    wifi_prov_mgr_config_t provisioning_config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    const esp_err_t init_result = wifi_prov_mgr_init(provisioning_config);
    if (init_result != ESP_OK) {
        wifi_report_error("Provisioning manager restart", init_result);
        return init_result;
    }
    s_provisioning_active = true;
    return wifi_start_provisioning();
}

static esp_err_t wifi_start_provisioning(void)
{
    esp_err_t result = wifi_load_or_create_pop();
    if (result != ESP_OK) {
        wifi_report_error("Proof-of-possession setup", result);
        return result;
    }

    char service_name[WIFI_SERVICE_NAME_LENGTH];
    wifi_get_service_name(service_name, sizeof(service_name));

    wifi_notify_state(WIFI_STATE_PROVISIONING, s_pop);
    result = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                              s_pop,
                                              service_name,
                                              NULL);
    if (result != ESP_OK) {
        wifi_report_error("BLE provisioning start", result);
    } else {
        ESP_LOGI(TAG, "BLE provisioning started");
    }
    return result;
}

static esp_err_t wifi_start_station(void)
{
    const esp_err_t mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (mode_result != ESP_OK) {
        wifi_report_error("Station mode setup", mode_result);
        return mode_result;
    }

    s_station_started = true;
    const esp_err_t start_result = esp_wifi_start();
    if (start_result != ESP_OK) {
        s_station_started = false;
        wifi_report_error("Wi-Fi station start", start_result);
        return start_result;
    }

    s_connection_attempts = 0;
    wifi_notify_state(WIFI_STATE_CONNECTING, NULL);
    /* WIFI_EVENT_STA_START schedules the first connection attempt. */
    return ESP_OK;
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_data;

    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            wifi_notify_state(WIFI_STATE_PROVISIONING, s_pop);
            break;
        case WIFI_PROV_CRED_RECV:
            ESP_LOGI(TAG, "Wi-Fi credentials received");
            wifi_notify_state(WIFI_STATE_CONNECTING, NULL);
            break;
        case WIFI_PROV_CRED_FAIL:
            ESP_LOGW(TAG, "Provisioned Wi-Fi connection failed; waiting for new credentials");
            (void)wifi_prov_mgr_reset_sm_state_on_failure();
            wifi_notify_state(WIFI_STATE_PROVISIONING, s_pop);
            break;
        case WIFI_PROV_CRED_SUCCESS:
            /* The provisioning manager emits this after IP_EVENT_STA_GOT_IP. */
            ESP_LOGI(TAG, "Wi-Fi provisioning connection succeeded");
            break;
        case WIFI_PROV_END:
            s_provisioning_active = false;
            ESP_LOGI(TAG, "BLE provisioning stopped");
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connection_attempts = 0;
        if (s_retry_timer != NULL) {
            (void)esp_timer_stop(s_retry_timer);
        }
        wifi_notify_state(WIFI_STATE_CONNECTED, NULL);
        ESP_LOGI(TAG, "IP address acquired");
        return;
    }

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_START && !s_provisioning_active) {
        (void)wifi_schedule_connection_attempt(0);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED && !s_provisioning_active) {
        if (s_state == WIFI_STATE_CONNECTED) {
            s_connection_attempts = 0;
        }
        wifi_notify_state(WIFI_STATE_CONNECTING, NULL);
        if (s_connection_attempts < WIFI_MAX_CONNECTION_ATTEMPTS) {
            const uint64_t delay_us = s_retry_delays_us[s_connection_attempts];
            (void)wifi_schedule_connection_attempt(delay_us);
        } else {
            wifi_enter_offline();
        }
    }
}

static void wifi_retry_timer_callback(void *argument)
{
    (void)argument;

    if (!s_started || s_provisioning_active || !s_station_started) {
        return;
    }

    if (s_state == WIFI_STATE_OFFLINE) {
        wifi_notify_state(WIFI_STATE_CONNECTING, NULL);
        s_connection_attempts = 0;
    }

    ++s_connection_attempts;
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi connection attempt", result);
    }
}

static esp_err_t wifi_schedule_connection_attempt(uint64_t delay_us)
{
    if (s_retry_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (esp_timer_is_active(s_retry_timer)) {
        (void)esp_timer_stop(s_retry_timer);
    }

    const esp_err_t result = esp_timer_start_once(s_retry_timer, delay_us);
    if (result != ESP_OK) {
        wifi_report_error("Wi-Fi retry scheduling", result);
    }
    return result;
}

static void wifi_enter_offline(void)
{
    wifi_notify_state(WIFI_STATE_OFFLINE, NULL);
    if (s_retry_timer == NULL) {
        return;
    }

    const esp_err_t result = esp_timer_start_once(s_retry_timer, WIFI_OFFLINE_RETRY_DELAY_US);
    if (result != ESP_OK) {
        wifi_report_error("Offline retry scheduling", result);
    }
}

static esp_err_t wifi_load_or_create_pop(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_POP_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }

    size_t pop_length = sizeof(s_pop);
    result = nvs_get_str(handle, WIFI_POP_KEY, s_pop, &pop_length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        const uint32_t random_value = esp_random();
        const int written = snprintf(s_pop, sizeof(s_pop), "%08" PRIX32, random_value);
        if (written != WIFI_POP_LENGTH) {
            nvs_close(handle);
            return ESP_FAIL;
        }

        result = nvs_set_str(handle, WIFI_POP_KEY, s_pop);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
    }

    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }
    if (pop_length != sizeof(s_pop) || s_pop[WIFI_POP_LENGTH] != '\0') {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void wifi_notify_state(wifi_state_t state, const char *pop)
{
    const bool same_state = s_state == state;
    const bool same_pop = pop == NULL || strcmp(pop, s_pop) == 0;
    if (same_state && same_pop) {
        return;
    }

    s_state = state;
    if (s_state_callback != NULL) {
        s_state_callback(state, pop, s_state_context);
    }
}

static void wifi_report_error(const char *operation, esp_err_t error)
{
    ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
    wifi_notify_state(WIFI_STATE_ERROR, NULL);
}

static void wifi_get_service_name(char *service_name, size_t length)
{
    uint8_t mac[6] = {0};
    const esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) {
        snprintf(service_name, length, "ONDA-SETUP");
        return;
    }

    snprintf(service_name,
             length,
             "ONDA-%02X%02X%02X",
             mac[3],
             mac[4],
             mac[5]);
}
