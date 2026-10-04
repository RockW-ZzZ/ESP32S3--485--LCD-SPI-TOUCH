#include "board_display.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t board_display_prepare(void)
{
    static bool prepared;
    if (prepared) return ESP_OK;
    gpio_config_t output = {
        .pin_bit_mask = (1ULL << BOARD_DISPLAY_RST) | (1ULL << BOARD_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output), "display", "reset GPIO");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_LCD_BL, 0), "display", "backlight off");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_DISPLAY_RST, 0), "display", "reset low");
    /* Assert reset before driving INT, to avoid fighting an active GT911. */
    vTaskDelay(pdMS_TO_TICKS(20));
    output.pin_bit_mask = 1ULL << BOARD_TOUCH_INT;
    ESP_RETURN_ON_ERROR(gpio_config(&output), "display", "INT output");
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_TOUCH_INT, 0), "display", "select 0x5D");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(BOARD_DISPLAY_RST, 1), "display", "release reset");
    vTaskDelay(pdMS_TO_TICKS(10));
    /* Keep INT low for synchronization, then let the controller own it. */
    vTaskDelay(pdMS_TO_TICKS(50));
    output.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&output), "display", "INT input");
    vTaskDelay(pdMS_TO_TICKS(140));
    prepared = true;
    return ESP_OK;
}
