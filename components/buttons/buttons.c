#include <stdbool.h>
#include <stdint.h>

#include "buttons.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * GPIO assignments and active-low behaviour are taken from Waveshare's
 * ESP-IDF 5.5.1 examples for the ESP32-S3-ePaper-1.54G. GPIO0 remains an
 * input only, preserving its BOOT/download strap role.
 */
#define BUTTON_BOOT_GPIO GPIO_NUM_0
#define BUTTON_PWR_GPIO GPIO_NUM_18
#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_DEBOUNCE_MS 15U
#define BUTTON_LONG_PRESS_US (1000LL * 1000LL)
#define BUTTON_TASK_STACK_SIZE 4096U
#define BUTTON_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)

#define BUTTON_NOTIFICATION_BOOT BIT0
#define BUTTON_NOTIFICATION_PWR BIT1

static const char *TAG = "ONDA_BUTTON";

typedef struct {
    button_id_t id;
    gpio_num_t gpio;
    uint32_t notification_bit;
    bool pressed;
    bool long_press_reported;
    int64_t pressed_at_us;
} button_state_t;

static button_state_t s_buttons[] = {
    {.id = BUTTON_ID_BOOT,
     .gpio = BUTTON_BOOT_GPIO,
     .notification_bit = BUTTON_NOTIFICATION_BOOT},
    {.id = BUTTON_ID_PWR, .gpio = BUTTON_PWR_GPIO, .notification_bit = BUTTON_NOTIFICATION_PWR},
};

static TaskHandle_t s_button_task;
static buttons_event_handler_t s_event_handler;
static void *s_event_context;
static bool s_initialised;
static bool s_owns_isr_service;

static void buttons_task(void *context);
static void buttons_isr(void *argument);
static void buttons_process_level(button_state_t *button, int level);
static void buttons_report_due_long_presses(void);
static TickType_t buttons_next_wait(void);
static void buttons_emit(button_state_t *button, button_event_type_t type);
static esp_err_t buttons_add_isr_handlers(void);
static void buttons_remove_isr_handlers(void);

esp_err_t buttons_init(buttons_event_handler_t handler, void *context)
{
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }

    const gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << BUTTON_BOOT_GPIO) | (1ULL << BUTTON_PWR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    esp_err_t result = gpio_config(&input_config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIOs: %s", esp_err_to_name(result));
        return result;
    }

    result = gpio_install_isr_service(0);
    if (result == ESP_OK) {
        s_owns_isr_service = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(result));
        return result;
    }

    s_event_handler = handler;
    s_event_context = context;

    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
        s_buttons[index].pressed = gpio_get_level(s_buttons[index].gpio) == BUTTON_ACTIVE_LEVEL;
        s_buttons[index].long_press_reported = false;
        s_buttons[index].pressed_at_us = esp_timer_get_time();
    }

    const BaseType_t task_result = xTaskCreate(buttons_task,
                                                "onda_buttons",
                                                BUTTON_TASK_STACK_SIZE,
                                                NULL,
                                                BUTTON_TASK_PRIORITY,
                                                &s_button_task);
    if (task_result != pdPASS) {
        s_button_task = NULL;
        s_event_handler = NULL;
        s_event_context = NULL;
        if (s_owns_isr_service) {
            gpio_uninstall_isr_service();
            s_owns_isr_service = false;
        }
        return ESP_ERR_NO_MEM;
    }

    result = buttons_add_isr_handlers();
    if (result != ESP_OK) {
        buttons_remove_isr_handlers();
        vTaskDelete(s_button_task);
        s_button_task = NULL;
        s_event_handler = NULL;
        s_event_context = NULL;
        if (s_owns_isr_service) {
            gpio_uninstall_isr_service();
            s_owns_isr_service = false;
        }
        ESP_LOGE(TAG, "Failed to add button GPIO ISR handlers: %s", esp_err_to_name(result));
        return result;
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Button input initialised");
    return ESP_OK;
}

static void buttons_task(void *context)
{
    (void)context;

    for (;;) {
        uint32_t changed_buttons = 0;
        const TickType_t wait_time = buttons_next_wait();
        const BaseType_t notified = xTaskNotifyWait(0, UINT32_MAX, &changed_buttons, wait_time);

        if (notified == pdTRUE) {
            /* The vendor baseline uses 15 ms of stable input for debounce. */
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS) + 1U);

            for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
                button_state_t *button = &s_buttons[index];
                if ((changed_buttons & button->notification_bit) != 0U) {
                    buttons_process_level(button, gpio_get_level(button->gpio));
                }
            }
        }

        buttons_report_due_long_presses();
    }
}

static void buttons_isr(void *argument)
{
    const uint32_t notification_bit = (uint32_t)(uintptr_t)argument;
    BaseType_t task_woken = pdFALSE;

    xTaskNotifyFromISR(s_button_task, notification_bit, eSetBits, &task_woken);
    if (task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void buttons_process_level(button_state_t *button, int level)
{
    const bool pressed = level == BUTTON_ACTIVE_LEVEL;
    if (pressed == button->pressed) {
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    button->pressed = pressed;

    if (pressed) {
        button->pressed_at_us = now_us;
        button->long_press_reported = false;
        return;
    }

    if (!button->long_press_reported) {
        const int64_t duration_us = now_us - button->pressed_at_us;
        const button_event_type_t type = duration_us >= BUTTON_LONG_PRESS_US
                                             ? BUTTON_EVENT_LONG_PRESS
                                             : BUTTON_EVENT_SHORT_PRESS;
        buttons_emit(button, type);
    }

    button->long_press_reported = false;
}

static void buttons_report_due_long_presses(void)
{
    const int64_t now_us = esp_timer_get_time();

    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
        button_state_t *button = &s_buttons[index];
        if (!button->pressed || button->long_press_reported ||
            now_us - button->pressed_at_us < BUTTON_LONG_PRESS_US) {
            continue;
        }

        if (gpio_get_level(button->gpio) != BUTTON_ACTIVE_LEVEL) {
            buttons_process_level(button, gpio_get_level(button->gpio));
            continue;
        }

        button->long_press_reported = true;
        buttons_emit(button, BUTTON_EVENT_LONG_PRESS);
    }
}

static TickType_t buttons_next_wait(void)
{
    const int64_t now_us = esp_timer_get_time();
    int64_t smallest_remaining_us = INT64_MAX;

    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
        const button_state_t *button = &s_buttons[index];
        if (!button->pressed || button->long_press_reported) {
            continue;
        }

        const int64_t remaining_us = BUTTON_LONG_PRESS_US - (now_us - button->pressed_at_us);
        if (remaining_us <= 0) {
            return 0;
        }
        if (remaining_us < smallest_remaining_us) {
            smallest_remaining_us = remaining_us;
        }
    }

    if (smallest_remaining_us == INT64_MAX) {
        return portMAX_DELAY;
    }

    const TickType_t wait_ticks = pdMS_TO_TICKS((smallest_remaining_us + 999LL) / 1000LL);
    return wait_ticks == 0 ? 1 : wait_ticks;
}

static void buttons_emit(button_state_t *button, button_event_type_t type)
{
    const button_event_t event = {
        .id = button->id,
        .type = type,
    };
    s_event_handler(event, s_event_context);
}

static esp_err_t buttons_add_isr_handlers(void)
{
    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
        const button_state_t *button = &s_buttons[index];
        const esp_err_t result = gpio_isr_handler_add(button->gpio,
                                                       buttons_isr,
                                                       (void *)(uintptr_t)button->notification_bit);
        if (result != ESP_OK) {
            return result;
        }
    }

    return ESP_OK;
}

static void buttons_remove_isr_handlers(void)
{
    for (size_t index = 0; index < sizeof(s_buttons) / sizeof(s_buttons[0]); ++index) {
        gpio_isr_handler_remove(s_buttons[index].gpio);
    }
}
