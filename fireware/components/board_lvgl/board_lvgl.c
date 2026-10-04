#include "board_lvgl.h"
#include "sdkconfig.h"

#ifdef CONFIG_BOARD_LVGL_BENCHMARK
#include <inttypes.h>
#include <stdlib.h>
#include "board_pins.h"
#include "lcd_st7796.h"
#include "touch_gt911.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lv_demos.h"

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP
#error "board_lvgl requires RGB565 with LV_COLOR_16_SWAP disabled; LCD driver swaps bytes"
#endif
#if !LV_USE_DEMO_BENCHMARK || !LV_USE_FONT_COMPRESSED
#error "Enable LVGL benchmark and compressed fonts in menuconfig"
#endif
_Static_assert(sizeof(lv_color_t) == sizeof(uint16_t), "RGB565 color size");

#define TICK_MS 5
static const char *TAG = "board_lvgl";
static bool started;
static lv_disp_draw_buf_t draw_buffer;
static lv_disp_drv_t display_driver;
static lv_indev_drv_t input_driver;
static lv_indev_state_t pointer_state;
static lv_point_t pointer_position;
static uint32_t flush_errors;
static int64_t last_flush_error, last_touch_error, benchmark_start_us;

static void tick(void *arg)
{
    (void)arg;
    lv_tick_inc(TICK_MS);
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colors)
{
    int width = area->x2 - area->x1 + 1, height = area->y2 - area->y1 + 1;
    esp_err_t err = lcd_draw_rgb565(area->x1, area->y1, width, height,
                                   (const uint16_t *)colors, (size_t)width * height);
    if (err != ESP_OK) {
        int64_t now = esp_timer_get_time();
        if (++flush_errors == 1 || now - last_flush_error >= 1000000) {
            ESP_LOGE(TAG, "LCD flush failed: %s; benchmark results are invalid", esp_err_to_name(err));
            last_flush_error = now;
        }
    }
    /* The LCD call blocks until all SPI DMA chunks have completed. */
    lv_disp_flush_ready(drv);
}

static void read_pointer(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    touch_frame_t frame;
    esp_err_t err = touch_read(&frame);
    if (frame.input_reset) {
        /* Runs in LVGL's own task. Cancel a held gesture without synthesizing
         * a release/click on its previous object when GPIO40 locks touch. */
        lv_indev_reset(lv_indev_get_act(), NULL);
        pointer_state = LV_INDEV_STATE_RELEASED;
    }
    if (err != ESP_OK) {
        pointer_state = LV_INDEV_STATE_RELEASED;
        int64_t now = esp_timer_get_time();
        if (now - last_touch_error >= 1000000) {
            ESP_LOGW(TAG, "Touch read: %s", esp_err_to_name(err));
            last_touch_error = now;
        }
    } else if (frame.updated) {
        pointer_state = frame.count ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (frame.count) {
            pointer_position.x = frame.points[0].x;
            pointer_position.y = frame.points[0].y;
        }
    }
    /* No new GT911 frame means retain state, not a fabricated release. */
    data->state = pointer_state;
    data->point = pointer_position;
    data->continue_reading = false;
}

static void benchmark_finished(void)
{
    ESP_LOGI(TAG, "Benchmark complete after %" PRIi64 " ms; flush errors=%" PRIu32,
             (esp_timer_get_time() - benchmark_start_us) / 1000, flush_errors);
    ESP_LOGI(TAG, "Weighted FPS and per-scene results follow on USB and LCD");
}

static void gui_task(void *arg)
{
    (void)arg;
    benchmark_start_us = esp_timer_get_time();
    lv_demo_benchmark_set_finished_cb(benchmark_finished);
    lv_demo_benchmark_set_max_speed(true);
    lv_demo_benchmark();
    for (;;) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 20) delay_ms = 20;
        if (delay_ms < 1) delay_ms = 1;
        TickType_t ticks = pdMS_TO_TICKS(delay_ms);
        vTaskDelay(ticks ? ticks : 1); /* Yield even in maximum-speed scenes. */
    }
}

esp_err_t board_lvgl_start(bool touch_available)
{
    if (started) return ESP_ERR_INVALID_STATE;
    const size_t pixel_count = BOARD_LCD_WIDTH * CONFIG_BOARD_LVGL_BUFFER_LINES;
    lv_color_t *pixels = heap_caps_malloc(pixel_count * sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return ESP_ERR_NO_MEM;
    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t timer_args = {.callback = tick, .name = "lvgl_tick"};
    esp_err_t err = esp_timer_create(&timer_args, &timer);
    if (err != ESP_OK) { free(pixels); return err; }

    lv_init();
    lv_disp_draw_buf_init(&draw_buffer, pixels, NULL, pixel_count);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = BOARD_LCD_WIDTH;
    display_driver.ver_res = BOARD_LCD_HEIGHT;
    display_driver.draw_buf = &draw_buffer;
    display_driver.flush_cb = flush;
    lv_disp_t *display = lv_disp_drv_register(&display_driver);
    lv_indev_t *input = NULL;
    bool timer_running = false;
    if (!display) { err = ESP_ERR_NO_MEM; goto fail; }
    if (touch_available) {
        lv_indev_drv_init(&input_driver);
        input_driver.type = LV_INDEV_TYPE_POINTER;
        input_driver.disp = display;
        input_driver.read_cb = read_pointer;
        input = lv_indev_drv_register(&input_driver);
        if (!input) { err = ESP_ERR_NO_MEM; goto fail; }
    }
    err = esp_timer_start_periodic(timer, TICK_MS * 1000);
    if (err != ESP_OK) goto fail;
    timer_running = true;
    const BaseType_t core = configNUM_CORES > 1 ? 1 : 0;
    if (xTaskCreatePinnedToCore(gui_task, "lvgl", 8192, NULL, 4, NULL, core) != pdPASS) {
        err = ESP_ERR_NO_MEM; goto fail;
    }
    started = true;
    ESP_LOGI(TAG, "LVGL %d.%d.%d benchmark: %dx%d, SPI %d MHz, PSRAM buffer %u bytes, touch=%s",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, CONFIG_BOARD_LCD_SPI_MHZ,
             (unsigned)(pixel_count * sizeof(*pixels)), touch_available ? "on" : "off");
    return ESP_OK;
fail:
    if (timer_running) esp_timer_stop(timer);
    esp_timer_delete(timer);
    if (input) lv_indev_delete(input);
    if (display) lv_disp_remove(display);
    free(pixels);
    return err;
}
#else
esp_err_t board_lvgl_start(bool touch_available)
{
    (void)touch_available;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
