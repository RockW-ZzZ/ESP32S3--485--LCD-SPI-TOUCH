#pragma once
/* Host test ABI only. Firmware always uses the real installed ESP-IDF headers. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
typedef unsigned TickType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
typedef enum { UART_NUM_0, UART_NUM_1, UART_NUM_2 } uart_port_t;
typedef enum { UART_PARITY_DISABLE, UART_PARITY_EVEN = 2, UART_PARITY_ODD } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1, UART_STOP_BITS_2 = 3 } uart_stop_bits_t;
enum { UART_DATA_8_BITS = 3, UART_HW_FLOWCTRL_DISABLE = 0, UART_SCLK_DEFAULT = 0,
       UART_MODE_RS485_HALF_DUPLEX = 1 };
enum { UART_DATA, UART_BREAK, UART_BUFFER_FULL, UART_FIFO_OVF, UART_FRAME_ERR, UART_PARITY_ERR };
typedef struct { int baud_rate, data_bits; uart_parity_t parity; uart_stop_bits_t stop_bits;
                 int flow_ctrl, source_clk; } uart_config_t;
typedef struct { int type; size_t size; bool timeout_flag; } uart_event_t;
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
#define GPIO_MODE_OUTPUT 2
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
int xQueueReset(QueueHandle_t);
int64_t esp_timer_get_time(void);
esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_driver_install(uart_port_t, int, int, int, QueueHandle_t *, int);
esp_err_t uart_driver_delete(uart_port_t);
esp_err_t uart_set_pin(uart_port_t, int, int, int, int);
esp_err_t uart_set_mode(uart_port_t, int);
esp_err_t uart_set_rts(uart_port_t, int);
esp_err_t uart_set_rx_timeout(uart_port_t, uint8_t);
esp_err_t uart_set_rx_full_threshold(uart_port_t, int);
void uart_set_always_rx_timeout(uart_port_t, bool);
esp_err_t uart_flush_input(uart_port_t);
esp_err_t uart_get_buffered_data_len(uart_port_t, size_t *);
int uart_write_bytes(uart_port_t, const void *, size_t);
esp_err_t uart_wait_tx_done(uart_port_t, TickType_t);
int uart_read_bytes(uart_port_t, void *, uint32_t, TickType_t);
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, uint32_t);
esp_err_t gpio_reset_pin(int);
esp_err_t gpio_set_direction(int, int);
