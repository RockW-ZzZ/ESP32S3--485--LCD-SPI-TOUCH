#include "buzzer.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool ready, enabled;
esp_err_t buzzer_init(void)
{
    /* Preload LOW before enabling the output; R30 holds LOW during reset. */
    esp_err_t err = gpio_set_level(BOARD_BUZZER, 0);
    if (err != ESP_OK) return err;
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << BOARD_BUZZER, .mode = GPIO_MODE_OUTPUT};
    err = gpio_config(&cfg);
    portENTER_CRITICAL(&mux);
    ready = err == ESP_OK; enabled = false;
    portEXIT_CRITICAL(&mux);
    return err;
}
esp_err_t buzzer_set(bool on)
{
    portENTER_CRITICAL(&mux);
    esp_err_t err = ready ? gpio_set_level(BOARD_BUZZER, on) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) enabled = on;
    portEXIT_CRITICAL(&mux);
    return err;
}
bool buzzer_is_on(void)
{
    portENTER_CRITICAL(&mux);
    bool on = enabled;
    portEXIT_CRITICAL(&mux);
    return on;
}
