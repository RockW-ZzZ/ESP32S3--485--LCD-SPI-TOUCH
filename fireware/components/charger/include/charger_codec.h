#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Vendor RTU dialect from the demo, NOT standard function 0x0F coils.
 * Address 0 is a unicast address here. Standard Modbus APIs remain unchanged. */
typedef struct { float voltage, current; uint16_t status; } charger_sample_t;
size_t charger_read_frame(uint8_t address, uint8_t out[8]);
size_t charger_power_frame(uint8_t address, bool on, uint8_t out[8]);
size_t charger_vi_frame(uint8_t address, float voltage, float current, uint8_t out[12]);
size_t charger_query_frame(uint8_t out[6]);
bool charger_parse_sample(uint8_t address, const uint8_t *frame, size_t size, charger_sample_t *sample);
bool charger_parse_ack(const uint8_t *request, const uint8_t *frame, size_t size);
bool charger_parse_address(const uint8_t *frame, size_t size, uint8_t *address);
