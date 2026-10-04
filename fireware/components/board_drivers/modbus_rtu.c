#include "modbus_rtu.h"
#include "modbus_codec.h"

static esp_err_t transact(rs485_port_t port, uint8_t slave, uint8_t function, uint16_t start, uint16_t count,
    const uint16_t *values, uint16_t *out, uint32_t timeout_ms, uint8_t *exception)
{
    if (exception) *exception = 0;
    uint8_t tx[256], rx[256];
    size_t tx_length = 0, rx_length = 0;
    if (mb_build_request(slave, function, start, count, values, tx, sizeof(tx), &tx_length) != MB_CODEC_OK)
        return ESP_ERR_INVALID_ARG;
    esp_err_t err = rs485_exchange(port, tx, tx_length, rx, sizeof(rx), &rx_length, timeout_ms);
    if (err != ESP_OK) return err;
    switch (mb_check_response(tx, tx_length, rx, rx_length, exception)) {
    case MB_CODEC_OK: break;
    case MB_CODEC_CRC: return ESP_ERR_INVALID_CRC;
    case MB_CODEC_EXCEPTION: return ESP_ERR_MODBUS_EXCEPTION;
    default: return ESP_ERR_INVALID_RESPONSE;
    }
    if (out) for (unsigned i = 0; i < count; ++i) out[i] = ((uint16_t)rx[3 + 2*i] << 8) | rx[4 + 2*i];
    return ESP_OK;
}
esp_err_t modbus_read_registers(rs485_port_t port, uint8_t slave, uint8_t function, uint16_t start,
    uint16_t count, uint16_t *registers, uint32_t timeout_ms, uint8_t *exception)
{
    if (exception) *exception = 0;
    if (!registers || (function != 3 && function != 4)) return ESP_ERR_INVALID_ARG;
    return transact(port, slave, function, start, count, NULL, registers, timeout_ms, exception);
}
esp_err_t modbus_write_single(rs485_port_t port, uint8_t slave, uint16_t address, uint16_t value,
    uint32_t timeout_ms, uint8_t *exception)
{
    return transact(port, slave, 6, address, 1, &value, NULL, timeout_ms, exception);
}
esp_err_t modbus_write_multiple(rs485_port_t port, uint8_t slave, uint16_t start, uint16_t count,
    const uint16_t *registers, uint32_t timeout_ms, uint8_t *exception)
{
    return transact(port, slave, 16, start, count, registers, NULL, timeout_ms, exception);
}
