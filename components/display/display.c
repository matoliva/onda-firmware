#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Board connection and panel command sequence adapted from Waveshare's
 * ESP32-S3-ePaper-1.54G 09_E_Paper_Test example. The transport is kept here
 * so application code does not depend on panel or SPI details.
 */

#define DISPLAY_WIDTH 200U
#define DISPLAY_HEIGHT 200U
#define DISPLAY_PIXELS_PER_BYTE 4U
#define DISPLAY_FRAMEBUFFER_SIZE \
    ((DISPLAY_WIDTH * DISPLAY_HEIGHT) / DISPLAY_PIXELS_PER_BYTE)

#define DISPLAY_POWER_PIN GPIO_NUM_6
#define DISPLAY_RESET_PIN GPIO_NUM_9
#define DISPLAY_DC_PIN GPIO_NUM_10
#define DISPLAY_CS_PIN GPIO_NUM_11
#define DISPLAY_SCK_PIN GPIO_NUM_12
#define DISPLAY_MOSI_PIN GPIO_NUM_13
#define DISPLAY_BUSY_PIN GPIO_NUM_8

#define DISPLAY_SPI_HOST SPI3_HOST
#define DISPLAY_SPI_CLOCK_HZ (20 * 1000 * 1000)
#define DISPLAY_POWER_SETTLE_MS 10U
#define DISPLAY_RESET_HIGH_MS 200U
#define DISPLAY_RESET_LOW_MS 20U
#define DISPLAY_BUSY_POLL_MS 100U
#define DISPLAY_BUSY_TIMEOUT_MS 30000U

#define EPD_COLOR_BLACK 0U
#define EPD_COLOR_WHITE 1U

#define FONT12_ASCII_FIRST ' '
#define FONT12_ASCII_LAST '~'

static const char *TAG = "ONDA_DISPLAY";

static spi_device_handle_t s_spi;
static bool s_transport_ready;
static bool s_panel_awake;
static uint8_t s_framebuffer[DISPLAY_FRAMEBUFFER_SIZE];

static esp_err_t display_configure_power(void);
static esp_err_t display_configure_transport(void);
static esp_err_t display_wake_panel(void);
static esp_err_t display_sleep_panel(void);
static esp_err_t display_wait_until_idle(void);
static esp_err_t display_send_command(uint8_t command);
static esp_err_t display_send_data(const uint8_t *data, size_t length);
static esp_err_t display_transfer(const uint8_t *data, size_t length);
static esp_err_t display_refresh(void);
static esp_err_t display_show_screen(const char *state_label,
                                      const char *detail,
                                      const char *proof_of_possession);
static void display_release_transport(void);
static void canvas_clear(uint8_t color);
static void canvas_set_pixel(uint16_t x, uint16_t y, uint8_t color);
static void canvas_draw_char(uint16_t x, uint16_t y, char character, uint8_t scale);
static void canvas_draw_centered_text(const char *text, uint16_t y, uint8_t scale);

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initialising display");

    esp_err_t result = display_configure_power();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure display power: %s", esp_err_to_name(result));
        return result;
    }

    if (!s_transport_ready) {
        result = display_configure_transport();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure display transport: %s", esp_err_to_name(result));
            display_release_transport();
            gpio_set_level(DISPLAY_POWER_PIN, 1);
            return result;
        }
    }

    result = display_wake_panel();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialise display panel: %s", esp_err_to_name(result));
        gpio_set_level(DISPLAY_POWER_PIN, 1);
        return result;
    }

    s_panel_awake = true;
    ESP_LOGI(TAG, "Display initialised");
    return ESP_OK;
}

esp_err_t display_show_ready(bool wifi_connected)
{
    return display_show_screen("Ready", wifi_connected ? "Wi-Fi connected" : "Recording available", NULL);
}

esp_err_t display_show_recording(void)
{
    return display_show_screen("Recording", NULL, NULL);
}

esp_err_t display_show_error(void)
{
    return display_show_screen("Error", NULL, NULL);
}

esp_err_t display_show_wifi_setup(const char *proof_of_possession)
{
    return display_show_screen("Wi-Fi setup", "Phone provisioning", proof_of_possession);
}

esp_err_t display_show_wifi_connecting(void)
{
    return display_show_screen("Connecting", "Wi-Fi", NULL);
}

esp_err_t display_show_offline(void)
{
    return display_show_screen("Offline", "Recording available", NULL);
}

esp_err_t display_show_wifi_error(void)
{
    return display_show_screen("Wi-Fi error", "Recording available", NULL);
}

static esp_err_t display_show_screen(const char *state_label,
                                     const char *detail,
                                     const char *proof_of_possession)
{
    if (state_label == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_panel_awake) {
        ESP_LOGE(TAG, "Display is not initialised");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Rendering %s screen", state_label);
    canvas_clear(EPD_COLOR_WHITE);
    canvas_draw_centered_text("ONDA", 44, 3);
    canvas_draw_centered_text(state_label, 88, 2);
    if (detail != NULL) {
        canvas_draw_centered_text(detail, 122, 1);
    }
    if (proof_of_possession != NULL) {
        canvas_draw_centered_text("PoP", 148, 1);
        canvas_draw_centered_text(proof_of_possession, 174, 2);
    }

    esp_err_t result = display_send_command(0x10);
    if (result == ESP_OK) {
        result = display_send_data(s_framebuffer, sizeof(s_framebuffer));
    }
    if (result == ESP_OK) {
        result = display_refresh();
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh %s screen: %s", state_label, esp_err_to_name(result));
        gpio_set_level(DISPLAY_POWER_PIN, 1);
        s_panel_awake = false;
        return result;
    }

    result = display_sleep_panel();
    gpio_set_level(DISPLAY_POWER_PIN, 1);
    s_panel_awake = false;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sleep display panel: %s", esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "Display %s", state_label);
    return ESP_OK;
}

static esp_err_t display_configure_power(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << DISPLAY_POWER_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    const esp_err_t result = gpio_config(&config);
    if (result == ESP_OK) {
        gpio_set_level(DISPLAY_POWER_PIN, 1);
    }
    return result;
}

static esp_err_t display_configure_transport(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = DISPLAY_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = DISPLAY_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_FRAMEBUFFER_SIZE,
    };
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = DISPLAY_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << DISPLAY_RESET_PIN) | (1ULL << DISPLAY_DC_PIN) |
                        (1ULL << DISPLAY_CS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << DISPLAY_BUSY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t result = gpio_config(&output_config);
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_config(&busy_config);
    if (result != ESP_OK) {
        return result;
    }

    gpio_set_level(DISPLAY_RESET_PIN, 1);
    gpio_set_level(DISPLAY_CS_PIN, 1);
    gpio_set_level(DISPLAY_DC_PIN, 1);

    result = spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        return result;
    }

    result = spi_bus_add_device(DISPLAY_SPI_HOST, &device_config, &s_spi);
    if (result != ESP_OK) {
        spi_bus_free(DISPLAY_SPI_HOST);
        return result;
    }

    s_transport_ready = true;
    return ESP_OK;
}

static esp_err_t display_wake_panel(void)
{
    gpio_set_level(DISPLAY_POWER_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_POWER_SETTLE_MS));

    gpio_set_level(DISPLAY_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_RESET_HIGH_MS));
    gpio_set_level(DISPLAY_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_RESET_LOW_MS));
    gpio_set_level(DISPLAY_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_RESET_HIGH_MS));

    static const uint8_t booster_soft_start[] = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};
    static const uint8_t panel_setting[] = {0x0F, 0x29};
    static const uint8_t resolution[] = {DISPLAY_WIDTH / 256U, DISPLAY_WIDTH % 256U,
                                         DISPLAY_HEIGHT / 256U, DISPLAY_HEIGHT % 256U};

    esp_err_t result = display_send_command(0x4D);
    const uint8_t command_4d_data = 0x78;
    if (result == ESP_OK) {
        result = display_send_data(&command_4d_data, 1);
    }
    if (result == ESP_OK) {
        result = display_send_command(0x00);
    }
    if (result == ESP_OK) {
        result = display_send_data(panel_setting, sizeof(panel_setting));
    }
    if (result == ESP_OK) {
        result = display_send_command(0x06);
    }
    if (result == ESP_OK) {
        result = display_send_data(booster_soft_start, sizeof(booster_soft_start));
    }
    if (result == ESP_OK) {
        result = display_send_command(0x50);
    }
    const uint8_t command_50_data = 0x37;
    if (result == ESP_OK) {
        result = display_send_data(&command_50_data, 1);
    }
    if (result == ESP_OK) {
        result = display_send_command(0x61);
    }
    if (result == ESP_OK) {
        result = display_send_data(resolution, sizeof(resolution));
    }
    if (result == ESP_OK) {
        result = display_send_command(0xE9);
    }
    const uint8_t command_e9_data = 0x01;
    if (result == ESP_OK) {
        result = display_send_data(&command_e9_data, 1);
    }
    if (result == ESP_OK) {
        result = display_send_command(0x30);
    }
    const uint8_t command_30_data = 0x08;
    if (result == ESP_OK) {
        result = display_send_data(&command_30_data, 1);
    }
    if (result == ESP_OK) {
        result = display_send_command(0x04);
    }
    if (result == ESP_OK) {
        result = display_wait_until_idle();
    }
    return result;
}

static esp_err_t display_sleep_panel(void)
{
    esp_err_t result = display_send_command(0x02);
    const uint8_t command_02_data = 0x00;
    if (result == ESP_OK) {
        result = display_send_data(&command_02_data, 1);
    }
    if (result == ESP_OK) {
        result = display_wait_until_idle();
    }
    if (result == ESP_OK) {
        result = display_send_command(0x07);
    }
    const uint8_t command_07_data = 0xA5;
    if (result == ESP_OK) {
        result = display_send_data(&command_07_data, 1);
    }
    return result;
}

static esp_err_t display_wait_until_idle(void)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(DISPLAY_BUSY_TIMEOUT_MS);

    while (gpio_get_level(DISPLAY_BUSY_PIN) == 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "Timed out waiting for display BUSY release");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_BUSY_POLL_MS));
    }

    return ESP_OK;
}

static esp_err_t display_send_command(uint8_t command)
{
    gpio_set_level(DISPLAY_DC_PIN, 0);
    return display_transfer(&command, 1);
}

static esp_err_t display_send_data(const uint8_t *data, size_t length)
{
    gpio_set_level(DISPLAY_DC_PIN, 1);
    return display_transfer(data, length);
}

static esp_err_t display_transfer(const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0 || s_spi == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = data,
    };

    gpio_set_level(DISPLAY_CS_PIN, 0);
    const esp_err_t result = spi_device_polling_transmit(s_spi, &transaction);
    gpio_set_level(DISPLAY_CS_PIN, 1);
    return result;
}

static esp_err_t display_refresh(void)
{
    const uint8_t refresh_data = 0x00;
    esp_err_t result = display_send_command(0x12);
    if (result == ESP_OK) {
        result = display_send_data(&refresh_data, 1);
    }
    if (result == ESP_OK) {
        result = display_wait_until_idle();
    }
    return result;
}

static void display_release_transport(void)
{
    if (s_spi != NULL) {
        spi_bus_remove_device(s_spi);
        s_spi = NULL;
    }
    if (s_transport_ready) {
        spi_bus_free(DISPLAY_SPI_HOST);
        s_transport_ready = false;
    }
}

static void canvas_clear(uint8_t color)
{
    const uint8_t packed_color = (color << 6U) | (color << 4U) | (color << 2U) | color;
    memset(s_framebuffer, packed_color, sizeof(s_framebuffer));
}

static void canvas_set_pixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) {
        return;
    }

    const size_t index = ((size_t)y * (DISPLAY_WIDTH / DISPLAY_PIXELS_PER_BYTE)) +
                         (x / DISPLAY_PIXELS_PER_BYTE);
    const uint8_t shift = 6U - ((x % DISPLAY_PIXELS_PER_BYTE) * 2U);
    s_framebuffer[index] = (s_framebuffer[index] & ~(0x03U << shift)) | ((color & 0x03U) << shift);
}

static void canvas_draw_char(uint16_t x, uint16_t y, char character, uint8_t scale)
{
    if (character < FONT12_ASCII_FIRST || character > FONT12_ASCII_LAST) {
        character = '?';
    }

    const size_t glyph_offset = ((size_t)(character - FONT12_ASCII_FIRST)) * Font12.Height;
    for (uint16_t row = 0; row < Font12.Height; ++row) {
        const uint8_t row_data = Font12.table[glyph_offset + row];
        for (uint16_t column = 0; column < Font12.Width; ++column) {
            if ((row_data & (0x80U >> column)) == 0) {
                continue;
            }
            for (uint8_t y_scale = 0; y_scale < scale; ++y_scale) {
                for (uint8_t x_scale = 0; x_scale < scale; ++x_scale) {
                    canvas_set_pixel(x + (column * scale) + x_scale,
                                     y + (row * scale) + y_scale,
                                     EPD_COLOR_BLACK);
                }
            }
        }
    }
}

static void canvas_draw_centered_text(const char *text, uint16_t y, uint8_t scale)
{
    const size_t text_length = strlen(text);
    const uint16_t text_width = (uint16_t)(text_length * Font12.Width * scale);
    const uint16_t x = (DISPLAY_WIDTH - text_width) / 2U;

    for (size_t index = 0; index < text_length; ++index) {
        canvas_draw_char(x + (index * Font12.Width * scale), y, text[index], scale);
    }
}
