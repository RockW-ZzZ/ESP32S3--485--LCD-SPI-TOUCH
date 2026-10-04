#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LCD_RGB565(r,g,b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
esp_err_t lcd_init(void);
/* Serialized, synchronous calls; buffer can be reused immediately on return.
 * RGB565 input is native uint16_t (red=0xF800), row-major, with no byte swapping
 * needed by the caller. Coordinates are landscape 480x320, top-left origin. */
esp_err_t lcd_draw_rgb565(int x, int y, int width, int height,
                         const uint16_t *pixels, size_t pixel_count);
esp_err_t lcd_fill_rect(int x, int y, int width, int height, uint16_t color);
esp_err_t lcd_fill(uint16_t color);
esp_err_t lcd_set_backlight(unsigned percent);
#ifdef __cplusplus
}
#endif
