#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define TOUCH_MAX_POINTS 5
typedef struct {
    uint8_t id;
    uint16_t x, y, size;
} touch_point_t;
typedef struct {
    bool updated; /* false: no new frame, not a release. */
    bool input_reset; /* Mode changed: cancel the previous UI gesture, not a click. */
    uint8_t count; /* updated=true,count=0: all contacts released. */
    touch_point_t points[TOUCH_MAX_POINTS];
} touch_frame_t;
esp_err_t touch_init(void);
/* Thread-safe task APIs. Boot default is enabled, not persisted to flash.
 * Disabling masks input; sensor is still polled and acknowledged. Enabling
 * discards pending sensor data; a held finger must lift before a new gesture. */
esp_err_t touch_set_enabled(bool enabled);
bool touch_is_enabled(void);
/* Poll from one consumer task, typically every 10-20 ms. Serialized by the driver.
 * Preserves factory sensor configuration; reports 480x320 landscape coordinates.
 * While LVGL runs, its input callback is the sole consumer. */
esp_err_t touch_read(touch_frame_t *frame);
#ifdef __cplusplus
}
#endif
