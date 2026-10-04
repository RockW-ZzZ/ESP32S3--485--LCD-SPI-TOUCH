#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Call once after lcd_init() and, optionally, touch_init(). Starts benchmark.
 * The GUI task owns all subsequent LVGL calls and consumes touch_read().
 * Drivers and LVGL objects are retained for the lifetime of the application. */
esp_err_t board_lvgl_start(bool touch_available);
#ifdef __cplusplus
}
#endif
