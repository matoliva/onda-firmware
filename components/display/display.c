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
#define EPD_COLOR_YELLOW 2U
#define EPD_COLOR_RED 3U

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
static esp_err_t display_initialise_panel(void);
static void display_release_transport(void);
static void canvas_clear(uint8_t color);
static void canvas_set_pixel(uint16_t x, uint16_t y, uint8_t color);
static void canvas_draw_char(uint16_t x,
                             uint16_t y,
                             char character,
                             uint8_t scale,
                             uint8_t color);
static void canvas_draw_centered_text(const char *text,
                                      uint16_t y,
                                      uint8_t scale,
                                      uint8_t color);
static void canvas_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color);
static void canvas_draw_line(uint16_t start_x,
                             uint16_t start_y,
                             uint16_t end_x,
                             uint16_t end_y,
                             uint8_t color);
static void canvas_draw_wifi_arc(uint16_t center_x,
                                 uint16_t top_y,
                                 uint16_t half_width,
                                 uint8_t color);
static void canvas_draw_wifi_icon(display_wifi_status_t status);
static void canvas_draw_storage_icon(display_storage_status_t status);
static void canvas_draw_battery_icon(display_battery_status_t status);
static uint8_t display_color_to_epd(display_color_t color);

static esp_err_t display_initialise_panel(void)
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

esp_err_t display_show(const display_screen_t *screen)
{
    if (screen == NULL || screen->title == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = display_initialise_panel();
    if (result != ESP_OK) {
        return result;
    }

    const uint8_t title_color = display_color_to_epd(screen->title_color);
    const uint8_t accent_color = display_color_to_epd(screen->accent_color);

    ESP_LOGI(TAG, "Rendering %s screen", screen->title);
    canvas_clear(EPD_COLOR_WHITE);
    canvas_draw_char(10, 10, 'O', 1, EPD_COLOR_BLACK);
    canvas_draw_char(17, 10, 'N', 1, EPD_COLOR_BLACK);
    canvas_draw_char(24, 10, 'D', 1, EPD_COLOR_BLACK);
    canvas_draw_char(31, 10, 'A', 1, EPD_COLOR_BLACK);
    canvas_draw_storage_icon(screen->storage_status);
    canvas_draw_battery_icon(screen->battery_status);
    canvas_draw_wifi_icon(screen->wifi_status);

    if (screen->show_recording_indicator) {
        canvas_fill_rect(20, 79, 8, 8, EPD_COLOR_RED);
    }
    canvas_draw_centered_text(screen->title, 72, 2, title_color);
    if (screen->detail_first_line != NULL) {
        canvas_draw_centered_text(screen->detail_first_line, 112, 1, accent_color);
    }
    if (screen->detail_second_line != NULL) {
        canvas_draw_centered_text(screen->detail_second_line, 128, 1, EPD_COLOR_BLACK);
    }
    if (screen->proof_of_possession != NULL) {
        canvas_draw_centered_text("PoP", 150, 1, EPD_COLOR_BLACK);
        canvas_draw_centered_text(screen->proof_of_possession, 166, 1, accent_color);
    }

    result = display_send_command(0x10);
    if (result == ESP_OK) {
        result = display_send_data(s_framebuffer, sizeof(s_framebuffer));
    }
    if (result == ESP_OK) {
        result = display_refresh();
    }

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to refresh %s screen: %s", screen->title, esp_err_to_name(result));
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

    ESP_LOGI(TAG, "Display %s", screen->title);
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

static void canvas_draw_char(uint16_t x,
                             uint16_t y,
                             char character,
                             uint8_t scale,
                             uint8_t color)
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
                                     color);
                }
            }
        }
    }
}

static void canvas_draw_centered_text(const char *text,
                                      uint16_t y,
                                      uint8_t scale,
                                      uint8_t color)
{
    if (text == NULL) {
        return;
    }

    const size_t text_length = strlen(text);
    const uint16_t text_width = (uint16_t)(text_length * Font12.Width * scale);
    if (text_width > DISPLAY_WIDTH) {
        return;
    }
    const uint16_t x = (DISPLAY_WIDTH - text_width) / 2U;

    for (size_t index = 0; index < text_length; ++index) {
        canvas_draw_char(x + (index * Font12.Width * scale), y, text[index], scale, color);
    }
}

static void canvas_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t color)
{
    for (uint16_t row = 0; row < height; ++row) {
        for (uint16_t column = 0; column < width; ++column) {
            canvas_set_pixel(x + column, y + row, color);
        }
    }
}

static void canvas_draw_line(uint16_t start_x,
                             uint16_t start_y,
                             uint16_t end_x,
                             uint16_t end_y,
                             uint8_t color)
{
    int16_t x = start_x;
    int16_t y = start_y;
    const int16_t delta_x = end_x > start_x ? (int16_t)(end_x - start_x) :
                                             (int16_t)(start_x - end_x);
    const int16_t step_x = start_x < end_x ? 1 : -1;
    const int16_t delta_y = end_y > start_y ? (int16_t)(start_y - end_y) :
                                             (int16_t)(end_y - start_y);
    const int16_t step_y = start_y < end_y ? 1 : -1;
    int16_t error = delta_x + delta_y;

    for (;;) {
        canvas_set_pixel((uint16_t)x, (uint16_t)y, color);
        if (x == (int16_t)end_x && y == (int16_t)end_y) {
            return;
        }

        const int16_t doubled_error = 2 * error;
        if (doubled_error >= delta_y) {
            error += delta_y;
            x += step_x;
        }
        if (doubled_error <= delta_x) {
            error += delta_x;
            y += step_y;
        }
    }
}

static void canvas_draw_wifi_arc(uint16_t center_x,
                                 uint16_t top_y,
                                 uint16_t half_width,
                                 uint8_t color)
{
    for (uint16_t offset = 0; offset <= half_width; ++offset) {
        const uint16_t vertical_offset = (offset * offset) / (half_width + 3U);
        const uint16_t left_x = center_x - offset;
        const uint16_t right_x = center_x + offset;
        const uint16_t y = top_y + vertical_offset;

        canvas_set_pixel(left_x, y, color);
        canvas_set_pixel(left_x, y + 1U, color);
        canvas_set_pixel(right_x, y, color);
        canvas_set_pixel(right_x, y + 1U, color);
    }
}

static void canvas_draw_wifi_icon(display_wifi_status_t status)
{
    const uint16_t center_x = 190U;

    canvas_draw_wifi_arc(center_x, 9U, 7U, EPD_COLOR_BLACK);
    canvas_draw_wifi_arc(center_x, 14U, 4U, EPD_COLOR_BLACK);
    canvas_fill_rect(center_x - 1U, 19U, 3U, 3U, EPD_COLOR_BLACK);

    if (status == DISPLAY_WIFI_OFFLINE) {
        canvas_draw_line(182U, 9U, 198U, 22U, EPD_COLOR_BLACK);
    }
}

static void canvas_draw_storage_icon(display_storage_status_t status)
{
    const uint16_t x = 132U;
    const uint16_t y = 10U;
    canvas_draw_char(x, y, 'S', 1U, EPD_COLOR_BLACK);
    canvas_draw_char(x + 7U, y, 'D', 1U, EPD_COLOR_BLACK);

    switch (status) {
    case DISPLAY_STORAGE_AVAILABLE:
        break;
    case DISPLAY_STORAGE_UNAVAILABLE:
    case DISPLAY_STORAGE_ERROR:
    default:
        canvas_draw_line(x - 1U, y - 1U, x + 15U, y + 12U, EPD_COLOR_BLACK);
        break;
    }
}

static void canvas_draw_battery_icon(display_battery_status_t status)
{
    const uint16_t x = 153U;
    const uint16_t y = 10U;
    const uint8_t color = status == DISPLAY_BATTERY_CRITICAL ? EPD_COLOR_RED : EPD_COLOR_BLACK;
    uint16_t fill_width = 0U;

    switch (status) {
    case DISPLAY_BATTERY_HIGH:
        fill_width = 12U;
        break;
    case DISPLAY_BATTERY_MEDIUM:
        fill_width = 8U;
        break;
    case DISPLAY_BATTERY_LOW:
        fill_width = 4U;
        break;
    case DISPLAY_BATTERY_CRITICAL:
        fill_width = 2U;
        break;
    case DISPLAY_BATTERY_UNKNOWN:
    default:
        break;
    }

    canvas_draw_line(x, y, x + 15U, y, color);
    canvas_draw_line(x, y, x, y + 9U, color);
    canvas_draw_line(x + 15U, y, x + 15U, y + 9U, color);
    canvas_draw_line(x, y + 9U, x + 15U, y + 9U, color);
    canvas_fill_rect(x + 16U, y + 3U, 2U, 4U, color);
    if (fill_width > 0U) {
        canvas_fill_rect(x + 2U, y + 2U, fill_width, 6U, color);
    } else {
        canvas_draw_char(x + 5U, y - 1U, '?', 1U, color);
    }
}

static uint8_t display_color_to_epd(display_color_t color)
{
    switch (color) {
    case DISPLAY_COLOR_BLACK:
        return EPD_COLOR_BLACK;
    case DISPLAY_COLOR_WHITE:
        return EPD_COLOR_WHITE;
    case DISPLAY_COLOR_YELLOW:
        return EPD_COLOR_YELLOW;
    case DISPLAY_COLOR_RED:
        return EPD_COLOR_RED;
    default:
        return EPD_COLOR_BLACK;
    }
}
