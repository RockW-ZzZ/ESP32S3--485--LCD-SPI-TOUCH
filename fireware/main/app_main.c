#include <inttypes.h>
#include "board_pins.h"
#include "board_lvgl.h"
#include "buttons.h"
#include "lcd_st7796.h"
#include "touch_gt911.h"
#include "serial_port.h"
#include "modbus_rtu.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "app";
static bool report(const char *name, esp_err_t err)
{
    if (err != ESP_OK) { ESP_LOGE(TAG, "%s: %s", name, esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "%s ready", name);
    return true;
}
static void serial_task(void *arg)
{
    (void)arg;
    uint8_t data[128];
    for (;;) {
        int n = serial_port_read(data, sizeof(data), pdMS_TO_TICKS(100));
        if (n > 0) {
            ESP_LOGI(TAG, "isolated UART RX: %d bytes", n);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, n, ESP_LOG_INFO);
#ifdef CONFIG_BOARD_SERIAL_ECHO
            report("UART echo", serial_port_write(data, n));
#endif
        } else if (n < 0) {
            ESP_LOGE(TAG, "UART read failed"); vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#ifdef CONFIG_BOARD_MODBUS_DEMO
typedef struct {
    rs485_port_t port;
    uint8_t slave;
    uint16_t start_register;
} modbus_demo_config_t;
static const modbus_demo_config_t modbus_demo_configs[RS485_PORT_COUNT] = {
    {RS485_PORT_1, CONFIG_BOARD_MODBUS1_SLAVE, CONFIG_BOARD_MODBUS1_START_REGISTER},
    {RS485_PORT_2, CONFIG_BOARD_MODBUS2_SLAVE, CONFIG_BOARD_MODBUS2_START_REGISTER},
};
static void modbus_task(void *arg)
{
    const modbus_demo_config_t *cfg = arg;
    for (;;) {
        uint16_t registers[2];
        uint8_t exception = 0;
        esp_err_t err = modbus_read_registers(cfg->port, cfg->slave, 3,
            cfg->start_register, 2, registers, 2000, &exception);
        if (err == ESP_OK) ESP_LOGI(TAG, "RS485-%d Modbus registers: %u, %u", cfg->port + 1, registers[0], registers[1]);
        else ESP_LOGW(TAG, "RS485-%d Modbus: %s (0x%x), exception=0x%02x", cfg->port + 1, esp_err_to_name(err), err, exception);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif
static void init_modbus_ports(void)
{
    uart_parity_t parity[RS485_PORT_COUNT] = {UART_PARITY_DISABLE, UART_PARITY_DISABLE};
    uart_stop_bits_t stop[RS485_PORT_COUNT] = {UART_STOP_BITS_1, UART_STOP_BITS_1};
#ifdef CONFIG_BOARD_RS4851_PARITY_EVEN
    parity[0] = UART_PARITY_EVEN;
#elif defined(CONFIG_BOARD_RS4851_PARITY_ODD)
    parity[0] = UART_PARITY_ODD;
#endif
#ifdef CONFIG_BOARD_RS4852_PARITY_EVEN
    parity[1] = UART_PARITY_EVEN;
#elif defined(CONFIG_BOARD_RS4852_PARITY_ODD)
    parity[1] = UART_PARITY_ODD;
#endif
#ifdef CONFIG_BOARD_RS4851_TWO_STOP_BITS
    stop[0] = UART_STOP_BITS_2;
#endif
#ifdef CONFIG_BOARD_RS4852_TWO_STOP_BITS
    stop[1] = UART_STOP_BITS_2;
#endif
    const int baud[RS485_PORT_COUNT] = {CONFIG_BOARD_RS4851_BAUD, CONFIG_BOARD_RS4852_BAUD};
    const char *names[RS485_PORT_COUNT] = {"RS485-1 UART1 TX17/RX18/DE21", "RS485-2 UART2 TX11/RX12/DE14"};
    for (unsigned i = 0; i < RS485_PORT_COUNT; ++i) {
        bool ok = report(names[i], rs485_init((rs485_port_t)i, baud[i], parity[i], stop[i]));
#ifdef CONFIG_BOARD_MODBUS_DEMO
        if (ok && xTaskCreate(modbus_task, i == 0 ? "modbus1" : "modbus2", 4096,
                              (void *)&modbus_demo_configs[i], 3, NULL) != pdPASS)
            ESP_LOGE(TAG, "Cannot create Modbus task for port %u", i + 1);
#else
        if (ok) ESP_LOGI(TAG, "RS485-%u Modbus master ready; automatic queries disabled", i + 1);
#endif
    }
}
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3-WROOM-1U-N16R8 basic drivers, IDF %s", esp_get_idf_version());
    ESP_LOGI(TAG, "PSRAM detected: %u bytes", (unsigned)esp_psram_get_size());
    bool lcd_ok = report("ST7796 480x320 landscape", lcd_init());
    if (lcd_ok) {
#ifdef CONFIG_BOARD_LCD_COLOR_TEST
        report("LCD red", lcd_fill_rect(0, 0, BOARD_LCD_WIDTH / 3, BOARD_LCD_HEIGHT, 0xF800));
        report("LCD green", lcd_fill_rect(BOARD_LCD_WIDTH / 3, 0, BOARD_LCD_WIDTH / 3, BOARD_LCD_HEIGHT, 0x07E0));
        report("LCD blue", lcd_fill_rect(2 * BOARD_LCD_WIDTH / 3, 0, BOARD_LCD_WIDTH / 3, BOARD_LCD_HEIGHT, 0x001F));
#else
        report("LCD clear", lcd_fill(0x0000));
#endif
        report("LCD backlight", lcd_set_backlight(80));
    }
    bool touch_ok = report("GT911", touch_init());
    report("GPIO41 reserved (200 ms filter), GPIO40 touch toggle (2 s hold)", buttons_init());
    ESP_LOGI(TAG, "Touch input enabled at boot; hold GPIO40 for 2 seconds to toggle");
    if (report("CN2 isolated UART0", serial_port_init(CONFIG_BOARD_SERIAL_BAUD, UART_PARITY_DISABLE, UART_STOP_BITS_1))) {
        if (xTaskCreate(serial_task, "serial_demo", 4096, NULL, 3, NULL) != pdPASS)
            ESP_LOGE(TAG, "Cannot create serial task");
    }
    init_modbus_ports();
    bool gui_active = false;
#ifdef CONFIG_BOARD_LVGL_BENCHMARK
    if (lcd_ok) gui_active = report("LVGL benchmark", board_lvgl_start(touch_ok));
#endif
    uint8_t previous_count = 0;
    int64_t last_touch_log = 0, last_touch_error = 0, last_health = 0;
    const char *event_names[] = {"pressed", "released", "long press"};
    bool reserved_pressed = false;
    for (;;) {
        button_event_t event;
        while (buttons_get_event(&event, 0)) {
            ESP_LOGI(TAG, "KEY%d %s", event.id + 1, event_names[event.type]);
            if (event.id == BUTTON_KEY2 && event.type == BUTTON_LONG_PRESS && touch_ok) {
                bool enabled = !touch_is_enabled();
                esp_err_t err = touch_set_enabled(enabled);
                if (err == ESP_OK) ESP_LOGI(TAG, "Touch input %s by GPIO40", enabled ? "enabled" : "disabled");
                else ESP_LOGW(TAG, "Touch toggle failed: %s", esp_err_to_name(err));
            }
        }
        bool key1 = buttons_is_pressed(BUTTON_KEY1);
        if (key1 != reserved_pressed) {
            ESP_LOGI(TAG, "GPIO41 reserved state=%s (200 ms filtered)", key1 ? "pressed" : "released");
            reserved_pressed = key1;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (touch_ok && !gui_active) {
            touch_frame_t frame;
            esp_err_t err = touch_read(&frame);
            if (err != ESP_OK) {
                if (now - last_touch_error >= 1000) {
                    ESP_LOGW(TAG, "Touch read: %s", esp_err_to_name(err)); last_touch_error = now;
                }
            } else if (frame.updated) {
                if (frame.count != previous_count || (frame.count && now - last_touch_log >= 100)) {
                    if (!frame.count) ESP_LOGI(TAG, "Touch released");
                    for (unsigned i = 0; i < frame.count; ++i)
                        ESP_LOGI(TAG, "Touch id=%u x=%u y=%u size=%u", frame.points[i].id,
                            frame.points[i].x, frame.points[i].y, frame.points[i].size);
                    last_touch_log = now;
                }
                previous_count = frame.count;
            }
        }
        if (now - last_health >= 30000) {
            ESP_LOGI(TAG, "free heap=%" PRIu32 ", minimum=%" PRIu32,
                     esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
            last_health = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
