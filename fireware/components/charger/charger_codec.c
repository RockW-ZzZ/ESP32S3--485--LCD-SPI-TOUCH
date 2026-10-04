#include "charger_codec.h"
#include "modbus_codec.h"
static size_t finish(uint8_t *out, size_t n)
{
    uint16_t crc = mb_crc16(out, n);
    out[n] = (uint8_t)crc; out[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}
static bool valid(const uint8_t *p, size_t n)
{
    return p && n >= 4 && mb_crc16(p, n - 2) == (uint16_t)(p[n - 2] | (p[n - 1] << 8));
}
size_t charger_read_frame(uint8_t a, uint8_t out[8])
{
    if (a > 247 || !out) return 0;
    out[0] = a; out[1] = 3; out[2] = 0; out[3] = 1; out[4] = 0; out[5] = 3;
    return finish(out, 6);
}
size_t charger_power_frame(uint8_t a, bool on, uint8_t out[8])
{
    if (a > 247 || !out) return 0;
    out[0] = a; out[1] = 6; out[2] = 0; out[3] = 0; out[4] = 0; out[5] = on ? 0xFF : 0;
    return finish(out, 6);
}
size_t charger_vi_frame(uint8_t a, float v, float i, uint8_t out[12])
{
    /* Ordered comparisons reject NaN and infinity before integer conversion. */
    if (a > 247 || !out || !(v >= 0 && v <= 6553.5f && i >= 0 && i <= 655.35f)) return 0;
    uint16_t vr = (uint16_t)(v * 10 + 0.5f), ir = (uint16_t)(i * 100 + 0.5f);
    out[0] = a; out[1] = 0x0F; out[2] = 0; out[3] = 1; out[4] = 0; out[5] = 2;
    out[6] = (uint8_t)(vr >> 8); out[7] = (uint8_t)vr;
    out[8] = (uint8_t)(ir >> 8); out[9] = (uint8_t)ir;
    return finish(out, 10);
}
size_t charger_query_frame(uint8_t out[6])
{
    if (!out) return 0;
    out[0] = 0xDD; out[1] = 0xDD; out[2] = 0; out[3] = 1;
    return finish(out, 4);
}
bool charger_parse_sample(uint8_t a, const uint8_t *p, size_t n, charger_sample_t *s)
{
    if (!s || n != 11 || !valid(p, n) || p[0] != a || p[1] != 3 || p[2] != 6) return false;
    s->voltage = ((p[3] << 8) | p[4]) / 10.0f;
    /* Read current is little endian; voltage, status and all writes are big endian. */
    s->current = (p[5] | (p[6] << 8)) / 100.0f;
    s->status = (uint16_t)((p[7] << 8) | p[8]);
    return true;
}
bool charger_parse_ack(const uint8_t *q, const uint8_t *p, size_t n)
{
    if (!q || n != 8 || !valid(p, n) || (q[1] != 6 && q[1] != 0x0F)) return false;
    for (unsigned i = 0; i < 6; ++i) if (q[i] != p[i]) return false;
    return true;
}
bool charger_parse_address(const uint8_t *p, size_t n, uint8_t *a)
{
    if (!a || n != 8 || !valid(p, n) || p[0] != 0xDD || p[1] != 0xDD || p[2] || p[3] != 1 || p[4] || p[5] > 247) return false;
    *a = p[5]; return true;
}
