#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/timers.h"

#include "onda_time.h"

#define ONDA_TIME_NTP_SERVER "pool.ntp.org"
#define ONDA_TIME_TIMEZONE "NZST-12NZDT,M9.5.0,M4.1.0/3"
#define ONDA_TIME_SYNC_TIMEOUT_MS 30000U
#define ONDA_TIME_EARLIEST_UTC 1704067200LL /* 2024-01-01T00:00:00Z */
#define ONDA_TIME_LATEST_UTC 4102444800LL   /* 2100-01-01T00:00:00Z */

static const char *TAG = "ONDA_TIME";

static StaticTimer_t s_sync_timer_buffer;
static TimerHandle_t s_sync_timer;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static onda_time_state_t s_state = ONDA_TIME_UNSYNCED;
static bool s_initialized;
static bool s_sntp_initialized;

static void onda_time_sync_notification(struct timeval *time_value);
static void onda_time_sync_timeout(TimerHandle_t timer);
static bool onda_time_value_is_plausible(time_t value);
static esp_err_t onda_time_init_sntp(void);
static void onda_time_set_state(onda_time_state_t state);
static void onda_time_log_synchronized_time(time_t now);

esp_err_t onda_time_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (setenv("TZ", ONDA_TIME_TIMEZONE, 1) != 0) {
        ESP_LOGE(TAG, "Failed to configure Pacific/Auckland timezone");
        onda_time_set_state(ONDA_TIME_FAILED);
        return ESP_ERR_NO_MEM;
    }
    tzset();

    s_sync_timer = xTimerCreateStatic("onda_time", pdMS_TO_TICKS(ONDA_TIME_SYNC_TIMEOUT_MS),
                                      pdFALSE, NULL, onda_time_sync_timeout, &s_sync_timer_buffer);
    if (s_sync_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization timeout timer");
        onda_time_set_state(ONDA_TIME_FAILED);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Time service initialized for Pacific/Auckland");
    return ESP_OK;
}

esp_err_t onda_time_start_sync(void)
{
    if (!s_initialized || s_sync_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t init_result = onda_time_init_sntp();
    if (init_result != ESP_OK) {
        onda_time_set_state(ONDA_TIME_FAILED);
        ESP_LOGE(TAG, "SNTP initialization failed: %s", esp_err_to_name(init_result));
        return init_result;
    }

    const bool was_synced = onda_time_is_synced();
    if (!was_synced) {
        onda_time_set_state(ONDA_TIME_SYNCING);
        if (xTimerReset(s_sync_timer, 0) != pdPASS) {
            onda_time_set_state(ONDA_TIME_FAILED);
            ESP_LOGE(TAG, "Failed to start synchronization timeout");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Starting NTP synchronization");
    } else {
        ESP_LOGI(TAG, "Refreshing synchronized network time");
    }

    const esp_err_t start_result = esp_netif_sntp_start();
    if (start_result != ESP_OK) {
        if (!was_synced) {
            (void)xTimerStop(s_sync_timer, 0);
            onda_time_set_state(ONDA_TIME_FAILED);
        }
        ESP_LOGE(TAG, "NTP synchronization start failed: %s", esp_err_to_name(start_result));
    }
    return start_result;
}

onda_time_state_t onda_time_get_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const onda_time_state_t state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

bool onda_time_is_synced(void)
{
    return onda_time_get_state() == ONDA_TIME_SYNCED;
}

static void onda_time_sync_notification(struct timeval *time_value)
{
    (void)time_value;

    const time_t now = time(NULL);
    if (!onda_time_value_is_plausible(now)) {
        ESP_LOGE(TAG, "Received implausible NTP timestamp");
        return;
    }

    onda_time_set_state(ONDA_TIME_SYNCED);
    if (s_sync_timer != NULL && xTimerStop(s_sync_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to stop synchronization timeout");
    }
    ESP_LOGI(TAG, "Time synchronized");
    onda_time_log_synchronized_time(now);
}

static void onda_time_sync_timeout(TimerHandle_t timer)
{
    (void)timer;
    if (onda_time_get_state() != ONDA_TIME_SYNCING) {
        return;
    }

    onda_time_set_state(ONDA_TIME_FAILED);
    esp_sntp_stop();
    ESP_LOGW(TAG, "NTP synchronization failed after %u seconds; waiting for Wi-Fi reconnect",
             ONDA_TIME_SYNC_TIMEOUT_MS / 1000U);
}

static bool onda_time_value_is_plausible(time_t value)
{
    return (int64_t)value >= ONDA_TIME_EARLIEST_UTC && (int64_t)value < ONDA_TIME_LATEST_UTC;
}

static esp_err_t onda_time_init_sntp(void)
{
    if (s_sntp_initialized) {
        return ESP_OK;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ONDA_TIME_NTP_SERVER);
    config.start = false;
    config.wait_for_sync = false;
    config.smooth_sync = false;
    config.sync_cb = onda_time_sync_notification;
    const esp_err_t result = esp_netif_sntp_init(&config);
    if (result == ESP_OK) {
        s_sntp_initialized = true;
    }
    return result;
}

static void onda_time_set_state(onda_time_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

static void onda_time_log_synchronized_time(time_t now)
{
    struct tm utc_time;
    struct tm local_time;
    char utc[24U];
    char local[32U];
    if (gmtime_r(&now, &utc_time) == NULL || localtime_r(&now, &local_time) == NULL ||
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0U ||
        strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S %Z", &local_time) == 0U) {
        ESP_LOGW(TAG, "Failed to format synchronized time");
        return;
    }
    ESP_LOGI(TAG, "UTC %s", utc);
    ESP_LOGI(TAG, "Local %s", local);
}
