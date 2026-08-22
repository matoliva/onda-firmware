#ifndef ONDA_BUTTONS_H
#define ONDA_BUTTONS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUTTON_ID_BOOT,
    BUTTON_ID_PWR,
} button_id_t;

typedef enum {
    BUTTON_EVENT_SHORT_PRESS,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_VERY_LONG_PRESS,
} button_event_type_t;

typedef struct {
    button_id_t id;
    button_event_type_t type;
} button_event_t;

typedef void (*buttons_event_handler_t)(button_event_t event, void *context);

/**
 * Initialise the board's BOOT and PWR inputs.
 *
 * Events are delivered from the component's worker task, never from a GPIO
 * interrupt handler. The function may be called once after boot.
 */
esp_err_t buttons_init(buttons_event_handler_t handler, void *context);

#ifdef __cplusplus
}
#endif

#endif
