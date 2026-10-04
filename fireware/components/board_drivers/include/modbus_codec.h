#pragma once
/* Hardware-independent protocol core, also compiled in host regression tests. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    MB_CODEC_OK = 0, MB_CODEC_ARGUMENT, MB_CODEC_CRC, MB_CODEC_RESPONSE, MB_CODEC_EXCEPTION
} mb_codec_result_t;
uint16_t mb_crc16(const uint8_t *data, size_t length);
mb_codec_result_t mb_build_request(uint8_t slave, uint8_t function, uint16_t start,
    uint16_t count, const uint16_t *values, uint8_t *out, size_t capacity, size_t *length);
mb_codec_result_t mb_check_response(const uint8_t *request, size_t request_length,
    const uint8_t *response, size_t response_length, uint8_t *exception);
#ifdef __cplusplus
}
#endif
