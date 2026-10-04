#include "lcd_st7796.h"
#include "board_display.h"
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define LCD_HOST SPI2_HOST
#define CHUNK_PIXELS (BOARD_LCD_WIDTH * 8)
static spi_device_handle_t device;
static SemaphoreHandle_t lock;
static uint8_t *dma_buffer;
static bool ready;

static esp_err_t send_bytes(bool data, const void *bytes, size_t length)
{
    if (!length) return ESP_OK;
    esp_err_t err = gpio_set_level(BOARD_LCD_DC, data);
    if (err != ESP_OK) return err;
    spi_transaction_t t = {.length = length * 8, .tx_buffer = bytes};
    if (length <= sizeof(t.tx_data)) {
        t.flags = SPI_TRANS_USE_TXDATA;
        memcpy(t.tx_data, bytes, length);
    }
    return spi_device_polling_transmit(device, &t);
}

static esp_err_t command(uint8_t cmd, const void *data, size_t length)
{
    esp_err_t err = send_bytes(false, &cmd, 1);
    return err == ESP_OK ? send_bytes(true, data, length) : err;
}

static esp_err_t window(int x, int y, int w, int h)
{
    uint8_t columns[] = {x >> 8, x, (x + w - 1) >> 8, x + w - 1};
    uint8_t rows[] = {y >> 8, y, (y + h - 1) >> 8, y + h - 1};
    esp_err_t err = command(0x2A, columns, sizeof(columns));
    if (err == ESP_OK) err = command(0x2B, rows, sizeof(rows));
    if (err == ESP_OK) err = command(0x2C, NULL, 0);
    return err;
}

esp_err_t lcd_init(void)
{
    if (ready) return ESP_OK;
    esp_err_t err = board_display_prepare();
    if (err != ESP_OK) return err;
    bool bus_installed = false;
    lock = xSemaphoreCreateMutex();
    dma_buffer = heap_caps_malloc(CHUNK_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lock || !dma_buffer) { err = ESP_ERR_NO_MEM; goto fail; }
    gpio_config_t dc = {.pin_bit_mask = 1ULL << BOARD_LCD_DC, .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&dc);
    if (err != ESP_OK) goto fail;
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_LCD_MOSI, .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = CHUNK_PIXELS * 2,
    };
    err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto fail;
    bus_installed = true;
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = CONFIG_BOARD_LCD_SPI_MHZ * 1000000,
        .mode = 0, .spics_io_num = BOARD_LCD_CS, .queue_size = 1,
    };
    err = spi_bus_add_device(LCD_HOST, &cfg, &device);
    if (err != ESP_OK) goto fail;

    /* Register values from the supplied ZJY350S11CTG21 STM32 SPI example.
     * The reference board has different gamma/VCOM settings; do not copy them. */
    static const struct { uint8_t cmd, len, data[14]; uint16_t delay_ms; } init[] = {
        {0x01, 0, {0}, 150}, /* LCD-only software reset; shared RES stays high. */
        {0x11, 0, {0}, 120},
        {0xF0, 1, {0xC3}, 0}, {0xF0, 1, {0x96}, 0},
        {0x36, 1, {0x28}, 0}, /* Landscape 480x320, vendor USE_HORIZONTAL=2, BGR. */
        {0x3A, 1, {0x05}, 0},
        {0xE8, 8, {0x40,0x82,0x07,0x18,0x27,0x0A,0xB6,0x33}, 0},
        {0xC5, 1, {0x27}, 0}, {0xC2, 1, {0xA7}, 0},
        {0xE0,14, {0xF0,0x01,0x06,0x0F,0x12,0x1D,0x36,0x54,0x44,0x0C,0x18,0x16,0x13,0x15}, 0},
        {0xE1,14, {0xF0,0x01,0x05,0x0A,0x0B,0x07,0x32,0x44,0x44,0x0C,0x18,0x17,0x13,0x16}, 0},
        {0xF0, 1, {0x3C}, 0}, {0xF0, 1, {0x69}, 0}, {0x29, 0, {0}, 20},
    };
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); ++i) {
        err = command(init[i].cmd, init[i].data, init[i].len);
        if (err != ESP_OK) goto fail;
        if (init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init[i].delay_ms));
    }
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_10_BIT, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer);
    if (err != ESP_OK) goto fail;
    ledc_channel_config_t channel = {
        .gpio_num = BOARD_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) goto fail;
    ready = true;
    /* Backlight remains off until the caller finishes drawing. */
    return ESP_OK;
fail:
    if (device) { spi_bus_remove_device(device); device = NULL; }
    if (bus_installed) spi_bus_free(LCD_HOST);
    if (lock) { vSemaphoreDelete(lock); lock = NULL; }
    free(dma_buffer); dma_buffer = NULL;
    return err;
}

static esp_err_t draw(int x, int y, int w, int h, const uint16_t *pixels, uint16_t color)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || w > BOARD_LCD_WIDTH ||
        h > BOARD_LCD_HEIGHT || x > BOARD_LCD_WIDTH - w || y > BOARD_LCD_HEIGHT - h)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = window(x, y, w, h);
    size_t remaining = (size_t)w * h;
    while (remaining && err == ESP_OK) {
        size_t n = remaining < CHUNK_PIXELS ? remaining : CHUNK_PIXELS;
        for (size_t i = 0; i < n; ++i) {
            uint16_t value = pixels ? *pixels++ : color;
            dma_buffer[2 * i] = value >> 8;
            dma_buffer[2 * i + 1] = value;
        }
        err = send_bytes(true, dma_buffer, n * 2);
        remaining -= n;
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t lcd_draw_rgb565(int x, int y, int w, int h, const uint16_t *p, size_t count)
{
    if (!p || w <= 0 || h <= 0 || w > BOARD_LCD_WIDTH || h > BOARD_LCD_HEIGHT ||
        count < (size_t)w * h) return ESP_ERR_INVALID_ARG;
    return draw(x, y, w, h, p, 0);
}
esp_err_t lcd_fill_rect(int x, int y, int w, int h, uint16_t c) { return draw(x,y,w,h,NULL,c); }
esp_err_t lcd_fill(uint16_t c) { return lcd_fill_rect(0,0,BOARD_LCD_WIDTH,BOARD_LCD_HEIGHT,c); }
esp_err_t lcd_set_backlight(unsigned percent)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                           percent * 1023 / 100, 0);
    xSemaphoreGive(lock);
    return err;
}
