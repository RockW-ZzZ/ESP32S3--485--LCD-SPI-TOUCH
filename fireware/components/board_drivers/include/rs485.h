#pragma once
#include <stddef.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { RS485_PORT_1 = 0, RS485_PORT_2, RS485_PORT_COUNT } rs485_port_t;
/* Initialize each port once, sequentially at startup. Independent UARTs/queues/locks.
 * Port 1: UART1 TX17/RX18/RTS21. Port 2: UART2 TX11/RX12/RTS14. */
esp_err_t rs485_init(rs485_port_t port, int baud, uart_parity_t parity, uart_stop_bits_t stop_bits);
/* Blocking request/reply transaction, serialized ONLY within the selected port.
 * Different ports can run concurrently from different tasks, even at different
 * baud rates. RTS controls that port's ISO3082 DE+/RE automatically.
 * tx/rx include CRC. timeout_ms bounds lock/idle wait separately, then the entire
 * response after TX completes (1..60000 ms). Always expects a reply; no retries.
 * Raw transport does not interpret addresses (vendor address 0 may reply).
 * RX UART parity/framing/overflow errors reject the whole transaction. */
esp_err_t rs485_exchange(rs485_port_t port, const uint8_t *tx, size_t tx_length,
                         uint8_t *rx, size_t capacity, size_t *rx_length,
                         uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
