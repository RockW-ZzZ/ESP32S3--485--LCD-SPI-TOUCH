#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "board_pins.h"

/* Calibrate in the sensor's native portrait space, then rotate to match
 * ST7796 MADCTL=0x28 (vendor USE_HORIZONTAL=2). No GT911 configuration writes. */
static inline bool touch_map_landscape(uint16_t raw_x, uint16_t raw_y,
    uint16_t resolution_x, uint16_t resolution_y, bool swap_xy,
    bool mirror_x, bool mirror_y, uint16_t *out_x, uint16_t *out_y)
{
    if (!out_x || !out_y || resolution_x < 2 || resolution_y < 2 ||
        raw_x >= resolution_x || raw_y >= resolution_y) return false;
    uint32_t x = raw_x, y = raw_y, xmax = resolution_x, ymax = resolution_y;
    if (swap_xy) {
        uint32_t tmp = x; x = y; y = tmp;
        tmp = xmax; xmax = ymax; ymax = tmp;
    }
    x = x * (BOARD_LCD_NATIVE_WIDTH - 1) / (xmax - 1);
    y = y * (BOARD_LCD_NATIVE_HEIGHT - 1) / (ymax - 1);
    if (mirror_x) x = BOARD_LCD_NATIVE_WIDTH - 1 - x;
    if (mirror_y) y = BOARD_LCD_NATIVE_HEIGHT - 1 - y;
    *out_x = (uint16_t)y;
    *out_y = (uint16_t)(BOARD_LCD_NATIVE_WIDTH - 1 - x);
    return true;
}
