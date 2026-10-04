#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { BUTTON_KEY1 = 0, BUTTON_KEY2 = 1 } button_id_t;
typedef enum { BUTTON_PRESSED, BUTTON_RELEASED, BUTTON_LONG_PRESS } button_event_type_t;
typedef struct {
    button_id_t id;
    button_event_type_t type;
    int64_t timestamp_ms;
} button_event_t;
esp_err_t buttons_init(void);
/* KEY2/GPIO40 only: 30 ms debounce, one LONG_PRESS per 2000 ms hold (after
 * debounced press). One consumer, 16 events; oldest dropped on overflow. */
bool buttons_get_event(button_event_t *event, TickType_t wait_ticks);
/* KEY1/GPIO41 is state-only, with 200 ms stable filtering on both edges.
 * No KEY1 events or long-press action. Both keys are active low. */
bool buttons_is_pressed(button_id_t id);
#ifdef __cplusplus
}
#endif
