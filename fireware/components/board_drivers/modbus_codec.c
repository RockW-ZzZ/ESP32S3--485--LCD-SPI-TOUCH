#include "modbus_codec.h"

uint16_t mb_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    if (!data) return crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
    }
    return crc;
}
static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value; }
static uint16_t get16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static int crc_valid(const uint8_t *p, size_t n)
{
    return n >= 4 && mb_crc16(p, n - 2) == ((uint16_t)p[n - 2] | ((uint16_t)p[n - 1] << 8));
}
mb_codec_result_t mb_build_request(uint8_t slave, uint8_t function, uint16_t start,
    uint16_t count, const uint16_t *values, uint8_t *out, size_t capacity, size_t *length)
{
    if (length) *length = 0;
    if (!out || !length || slave < 1 || slave > 247 || !count ||
        (uint32_t)start + count > 65536U) return MB_CODEC_ARGUMENT;
    if ((function == 3 || function == 4) ? count > 125 :
        function == 6 ? (count != 1 || !values) :
        function == 16 ? (count > 123 || !values) : 1) return MB_CODEC_ARGUMENT;
    size_t n = function == 16 ? 9U + 2U * count : 8U;
    if (capacity < n) return MB_CODEC_ARGUMENT;
    out[0] = slave; out[1] = function; put16(out + 2, start);
    put16(out + 4, function == 6 ? values[0] : count);
    if (function == 16) {
        out[6] = count * 2;
        for (unsigned i = 0; i < count; ++i) put16(out + 7 + i * 2, values[i]);
    }
    uint16_t crc = mb_crc16(out, n - 2);
    out[n - 2] = crc; out[n - 1] = crc >> 8;
    *length = n;
    return MB_CODEC_OK;
}
mb_codec_result_t mb_check_response(const uint8_t *q, size_t qn,
    const uint8_t *r, size_t rn, uint8_t *exception)
{
    if (exception) *exception = 0;
    if (!q || !r || qn < 8 || qn > 256 || q[0] < 1 || q[0] > 247 || !crc_valid(q, qn))
        return MB_CODEC_ARGUMENT;
    uint8_t function = q[1];
    unsigned count = function == 6 ? 1 : get16(q + 4);
    if (!count || (uint32_t)get16(q + 2) + count > 65536U) return MB_CODEC_ARGUMENT;
    if (function == 3 || function == 4) {
        if (qn != 8 || count > 125) return MB_CODEC_ARGUMENT;
    } else if (function == 6) {
        if (qn != 8) return MB_CODEC_ARGUMENT;
    } else if (function == 16) {
        if (count > 123 || qn != 9 + 2 * count || q[6] != 2 * count) return MB_CODEC_ARGUMENT;
    } else return MB_CODEC_ARGUMENT;
    if (rn < 5 || rn > 256) return MB_CODEC_RESPONSE;
    if (!crc_valid(r, rn)) return MB_CODEC_CRC;
    if (r[0] != q[0]) return MB_CODEC_RESPONSE;
    if (r[1] == (function | 0x80)) {
        if (rn != 5 || r[2] == 0) return MB_CODEC_RESPONSE;
        if (exception) *exception = r[2];
        return MB_CODEC_EXCEPTION;
    }
    if (r[1] != function) return MB_CODEC_RESPONSE;
    if (function == 3 || function == 4) {
        if (rn != 5 + count * 2 || r[2] != count * 2) return MB_CODEC_RESPONSE;
    } else {
        if (rn != 8) return MB_CODEC_RESPONSE;
        for (unsigned i = 2; i < 6; ++i) if (r[i] != q[i]) return MB_CODEC_RESPONSE;
    }
    return MB_CODEC_OK;
}
