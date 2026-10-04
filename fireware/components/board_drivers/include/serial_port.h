#pragma once
#include <stddef.h>
#include "driver/uart.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Isolated CN2 serial port on UART0, TX43/RX44. USB carries logs. */
esp_err_t serial_port_init(int baud, uart_parity_t parity, uart_stop_bits_t stop_bits);
/* Single reader, task context. Returns bytes read (0=timeout) or -1 on error. */
int serial_port_read(void *buffer, size_t capacity, TickType_t wait_ticks);
/* Serialized writers. Blocks until bytes have left the wire; no flow control. */
esp_err_t serial_port_write(const void *data, size_t length);
#ifdef __cplusplus
}
#endif
