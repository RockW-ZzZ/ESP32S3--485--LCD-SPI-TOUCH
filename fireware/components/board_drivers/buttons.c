#include "buttons.h"
#include "board_pins.h"
#include "button_filter.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t events;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pressed[2];
static const int pins[] = {BOARD_KEY1, BOARD_KEY2};

static void emit(unsigned id, button_event_type_t type, int64_t now)
{
    button_event_t event = {.id = id, .type = type, .timestamp_ms = now};
    if (xQueueSend(events, &event, 0) != pdTRUE) {
        button_event_t discarded;
        (void)xQueueReceive(events, &discarded, 0);
        (void)xQueueSend(events, &event, 0);
        ESP_LOGW("buttons", "event queue overflow; oldest event dropped");
    }
}
static void scan_task(void *arg)
{
    (void)arg;
    button_filter_t filters[2] = {0};
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        for (unsigned i = 0; i < 2; ++i) {
            bool sample = gpio_get_level(pins[i]) == 0;
            unsigned changes = button_filter_step(&filters[i], sample, now,
                i == BUTTON_KEY1 ? BUTTON_KEY1_FILTER_MS : BUTTON_KEY2_FILTER_MS,
                i == BUTTON_KEY1 ? 0 : BUTTON_KEY2_HOLD_MS);
            if (changes & (BUTTON_FILTER_PRESSED | BUTTON_FILTER_RELEASED)) {
                portENTER_CRITICAL(&state_lock);
                pressed[i] = filters[i].stable;
                portEXIT_CRITICAL(&state_lock);
            }
            /* KEY1 is reserved: expose filtered state, no action/event binding. */
            if (i == BUTTON_KEY1) continue;
            if (changes & BUTTON_FILTER_PRESSED) emit(i, BUTTON_PRESSED, now);
            if (changes & BUTTON_FILTER_RELEASED) emit(i, BUTTON_RELEASED, now);
            if (changes & BUTTON_FILTER_LONG) emit(i, BUTTON_LONG_PRESS, now);
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(5));
    }
}
esp_err_t buttons_init(void)
{
    if (events) return ESP_OK;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_KEY1) | (1ULL << BOARD_KEY2),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    events = xQueueCreate(16, sizeof(button_event_t));
    if (!events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(scan_task, "buttons", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(events); events = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
bool buttons_get_event(button_event_t *event, TickType_t wait_ticks)
{
    return events && event && xQueueReceive(events, event, wait_ticks) == pdTRUE;
}
bool buttons_is_pressed(button_id_t id)
{
    if (id != BUTTON_KEY1 && id != BUTTON_KEY2) return false;
    portENTER_CRITICAL(&state_lock);
    bool value = pressed[id];
    portEXIT_CRITICAL(&state_lock);
    return value;
}
