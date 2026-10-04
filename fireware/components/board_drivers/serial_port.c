#include "serial_port.h"
#include "board_pins.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM) || defined(CONFIG_ESP_CONSOLE_SECONDARY_UART)
#error "All UARTs are used by CN2/RS485; keep the application console on USB Serial/JTAG"
#endif
#define SERIAL_UART UART_NUM_0
static SemaphoreHandle_t tx_lock;
static bool ready;

esp_err_t serial_port_init(int baud, uart_parity_t parity, uart_stop_bits_t stop_bits)
{
    if (ready) return ESP_ERR_INVALID_STATE;
    if (baud < 1200 || baud > 115200 ||
        (parity != UART_PARITY_DISABLE && parity != UART_PARITY_EVEN && parity != UART_PARITY_ODD) ||
        (stop_bits != UART_STOP_BITS_1 && stop_bits != UART_STOP_BITS_2)) return ESP_ERR_INVALID_ARG;
    tx_lock = xSemaphoreCreateMutex();
    if (!tx_lock) return ESP_ERR_NO_MEM;
    uart_config_t cfg = {.baud_rate = baud, .data_bits = UART_DATA_8_BITS,
        .parity = parity, .stop_bits = stop_bits, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};
    esp_err_t err = uart_param_config(SERIAL_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(SERIAL_UART, BOARD_SERIAL_TX, BOARD_SERIAL_RX, -1, -1);
    if (err == ESP_OK) err = uart_driver_install(SERIAL_UART, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK) { vSemaphoreDelete(tx_lock); tx_lock = NULL; return err; }
    ready = true;
    return ESP_OK;
}
int serial_port_read(void *buffer, size_t capacity, TickType_t wait_ticks)
{
    if (!ready || !buffer || !capacity || capacity > 4096) return -1;
    return uart_read_bytes(SERIAL_UART, buffer, capacity, wait_ticks);
}
esp_err_t serial_port_write(const void *data, size_t length)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!data || !length || length > 4096) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    int n = uart_write_bytes(SERIAL_UART, data, length);
    esp_err_t err = n == (int)length ? uart_wait_tx_done(SERIAL_UART, portMAX_DELAY) : ESP_FAIL;
    xSemaphoreGive(tx_lock);
    return err;
}
