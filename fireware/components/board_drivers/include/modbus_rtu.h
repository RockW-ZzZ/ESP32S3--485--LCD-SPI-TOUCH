#pragma once
#include <stdint.h>
#include "rs485.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ESP_ERR_MODBUS_EXCEPTION ((esp_err_t)0x7201)
/* Call rs485_init for each port, then these blocking master APIs from tasks.
 * Each port is an independent Modbus master; same slave IDs can exist on both.
 * Addresses 1..247 only; register offsets are zero-based, not 40001 notation.
 * No broadcast, automatic retries or slave service. Output registers remain
 * untouched on errors. exception may be NULL; otherwise reset to 0 on every call. */
esp_err_t modbus_read_registers(rs485_port_t port, uint8_t slave, uint8_t function, uint16_t start,
    uint16_t count, uint16_t *registers, uint32_t timeout_ms, uint8_t *exception);
esp_err_t modbus_write_single(rs485_port_t port, uint8_t slave, uint16_t address, uint16_t value,
    uint32_t timeout_ms, uint8_t *exception);
esp_err_t modbus_write_multiple(rs485_port_t port, uint8_t slave, uint16_t start, uint16_t count,
    const uint16_t *registers, uint32_t timeout_ms, uint8_t *exception);
#ifdef __cplusplus
}
#endif
