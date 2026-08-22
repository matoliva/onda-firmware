#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * These board assignments and the 16 kHz standard-I2S baseline are from
 * Waveshare's ESP-IDF 5.5.1 07_Audio_Test for ESP32-S3-ePaper-1.54G.
 */
#define AUDIO_POWER_GPIO GPIO_NUM_42
#define AUDIO_POWER_ON_LEVEL 0
#define AUDIO_POWER_OFF_LEVEL 1
#define AUDIO_I2C_PORT I2C_NUM_0
#define AUDIO_I2C_SDA_GPIO GPIO_NUM_47
#define AUDIO_I2C_SCL_GPIO GPIO_NUM_48
#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_I2S_BCLK_GPIO GPIO_NUM_15
#define AUDIO_I2S_WS_GPIO GPIO_NUM_38
#define AUDIO_I2S_DOUT_GPIO GPIO_NUM_45
#define AUDIO_I2S_DIN_GPIO GPIO_NUM_16
#define AUDIO_I2S_MCLK_GPIO GPIO_NUM_14
#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_CHANNEL_COUNT 2
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_MIC_GAIN_DB 45.0f
#define AUDIO_POWER_SETTLE_MS 10U

static const char *TAG = "ONDA_AUDIO";

static bool s_initialized;
static bool s_capturing;
static bool s_i2s_channels_enabled;
static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static i2s_chan_handle_t s_i2s_rx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_record_device;

static void audio_release_resources(void)
{
    if (s_record_device != NULL) {
        esp_codec_dev_delete(s_record_device);
        s_record_device = NULL;
    }
    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_data_if != NULL) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_i2s_rx != NULL) {
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
    if (s_i2s_tx != NULL) {
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }

    gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
    s_i2s_channels_enabled = false;
}

esp_err_t audio_init(void)
{
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t power_config = {
        .pin_bit_mask = 1ULL << AUDIO_POWER_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&power_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure audio power: %s", esp_err_to_name(result));
        return result;
    }

    result = gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_ON_LEVEL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable audio power: %s", esp_err_to_name(result));
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_SETTLE_MS));

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = AUDIO_I2C_PORT,
        .sda_io_num = AUDIO_I2C_SDA_GPIO,
        .scl_io_num = AUDIO_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    result = i2c_new_master_bus(&i2c_config, &s_i2c_bus);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise I2C: %s", esp_err_to_name(result));
        goto fail;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    result = i2s_new_channel(&channel_config, &s_i2s_tx, &s_i2s_rx);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channels: %s", esp_err_to_name(result));
        goto fail;
    }

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCLK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DOUT_GPIO,
            .din = AUDIO_I2S_DIN_GPIO,
        },
    };
    result = i2s_channel_init_std_mode(s_i2s_tx, &i2s_config);
    if (result == ESP_OK) {
        result = i2s_channel_init_std_mode(s_i2s_rx, &i2s_config);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise I2S: %s", esp_err_to_name(result));
        goto fail;
    }

    result = i2s_channel_enable(s_i2s_tx);
    if (result == ESP_OK) {
        result = i2s_channel_enable(s_i2s_rx);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S clocks: %s", esp_err_to_name(result));
        goto fail;
    }
    s_i2s_channels_enabled = true;

    audio_codec_i2s_cfg_t codec_i2s_config = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&codec_i2s_config);
    if (s_data_if == NULL) {
        result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create I2S codec interface");
        goto fail;
    }

    audio_codec_i2c_cfg_t codec_i2c_config = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = s_i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c_config);
    if (s_ctrl_if == NULL) {
        result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create I2C codec interface");
        goto fail;
    }

    s_gpio_if = audio_codec_new_gpio();
    if (s_gpio_if == NULL) {
        result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create codec GPIO interface");
        goto fail;
    }

    es8311_codec_cfg_t es8311_config = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .pa_pin = GPIO_NUM_NC,
        .use_mclk = true,
        /* The vendor board configuration keeps the ES8311 DAC reference on. */
        .no_dac_ref = false,
    };
    s_codec_if = es8311_codec_new(&es8311_config);
    if (s_codec_if == NULL) {
        result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create ES8311 codec interface");
        goto fail;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_record_device = esp_codec_dev_new(&device_config);
    if (s_record_device == NULL) {
        result = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create microphone device");
        goto fail;
    }

    gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
    s_initialized = true;
    ESP_LOGI(TAG, "Microphone ready (16 kHz, stereo, 16-bit PCM)");
    return ESP_OK;

fail:
    audio_release_resources();
    return result;
}

esp_err_t audio_start(void)
{
    if (!s_initialized || s_record_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_capturing) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_ON_LEVEL);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable audio power: %s", esp_err_to_name(result));
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(AUDIO_POWER_SETTLE_MS));

    if (!s_i2s_channels_enabled) {
        result = i2s_channel_enable(s_i2s_tx);
        if (result == ESP_OK) {
            result = i2s_channel_enable(s_i2s_rx);
        }
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable I2S clocks: %s", esp_err_to_name(result));
            gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
            return result;
        }
        s_i2s_channels_enabled = true;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = AUDIO_SAMPLE_RATE_HZ,
        .channel = AUDIO_CHANNEL_COUNT,
        .bits_per_sample = AUDIO_BITS_PER_SAMPLE,
    };
    result = esp_codec_dev_open(s_record_device, &sample_info);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open microphone capture: %s", esp_err_to_name(result));
        esp_codec_dev_close(s_record_device);
        s_i2s_channels_enabled = false;
        gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
        return result;
    }

    result = esp_codec_dev_set_in_gain(s_record_device, AUDIO_MIC_GAIN_DB);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set microphone gain: %s", esp_err_to_name(result));
        esp_codec_dev_close(s_record_device);
        i2s_channel_disable(s_i2s_tx);
        s_i2s_channels_enabled = false;
        gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
        return result;
    }

    s_capturing = true;
    ESP_LOGI(TAG, "Capture started");
    return ESP_OK;
}

esp_err_t audio_read(void *buffer, size_t buffer_size, size_t *bytes_read, TickType_t timeout)
{
    if (buffer == NULL || bytes_read == NULL || buffer_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || !s_capturing || s_i2s_rx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *bytes_read = 0;
    const esp_err_t result = i2s_channel_read(s_i2s_rx, buffer, buffer_size, bytes_read, timeout);
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "Microphone read failed: %s", esp_err_to_name(result));
    }
    return result;
}

esp_err_t audio_stop(void)
{
    if (!s_initialized || !s_capturing || s_record_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_codec_dev_close(s_record_device);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop microphone capture: %s", esp_err_to_name(result));
        return result;
    }

    s_capturing = false;
    s_i2s_channels_enabled = false;

    result = i2s_channel_disable(s_i2s_tx);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to stop I2S clock: %s", esp_err_to_name(result));
    }

    const esp_err_t power_result = gpio_set_level(AUDIO_POWER_GPIO, AUDIO_POWER_OFF_LEVEL);
    if (power_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable audio power: %s", esp_err_to_name(power_result));
    }

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (power_result != ESP_OK) {
        return power_result;
    }

    ESP_LOGI(TAG, "Capture stopped");
    return ESP_OK;
}
