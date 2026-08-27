#include "power.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"

/* Verified against Waveshare's 08_BATT_PWR_Test example for SKU 34586. */
#define POWER_BATTERY_LATCH_GPIO GPIO_NUM_17
#define POWER_BUTTON_GPIO GPIO_NUM_18

static const char *TAG = "ONDA_POWER";
static bool s_initialised;

static esp_err_t power_configure_latch(void)
{
    const gpio_config_t latch_config = {
        .pin_bit_mask = 1ULL << POWER_BATTERY_LATCH_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&latch_config);
}

static esp_err_t power_enter_sleep_with_latch(bool retain_latch)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = gpio_set_level(POWER_BATTERY_LATCH_GPIO, retain_latch ? 1 : 0);
    if (result != ESP_OK) {
        return result;
    }
    result = gpio_hold_en(POWER_BATTERY_LATCH_GPIO);
    if (result != ESP_OK) {
        return result;
    }
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif

    result = esp_sleep_enable_ext1_wakeup_io(1ULL << POWER_BUTTON_GPIO,
                                              ESP_EXT1_WAKEUP_ANY_LOW);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PWR wake-up: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "Entering deep sleep; PWR wakes the device");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

esp_err_t power_init(void)
{
    esp_err_t result = power_configure_latch();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure battery latch: %s", esp_err_to_name(result));
        return result;
    }

    /* Set the output register before releasing a retained deep-sleep level. */
    result = gpio_set_level(POWER_BATTERY_LATCH_GPIO, 1);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to retain battery power: %s", esp_err_to_name(result));
        return result;
    }

#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    result = gpio_hold_dis(POWER_BATTERY_LATCH_GPIO);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to release battery latch hold: %s", esp_err_to_name(result));
        return result;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Battery power latch retained");
    return ESP_OK;
}

esp_err_t power_enter_sleep(void)
{
    return power_enter_sleep_with_latch(true);
}

esp_err_t power_enter_off_sleep(void)
{
    return power_enter_sleep_with_latch(false);
}

esp_err_t power_release_battery_latch(void)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    esp_err_t result = gpio_hold_dis(POWER_BATTERY_LATCH_GPIO);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = gpio_set_level(POWER_BATTERY_LATCH_GPIO, 0);
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Battery power latch released");
    }
    return result;
}
