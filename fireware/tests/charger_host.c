/* Deterministic scheduler around the ACTUAL service, codec and policy.
 * Hardware UART concurrency is covered separately by test_runtime.py. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../components/charger/charger_service.c"
#include "modbus_codec.h"
static int64_t clock_ms, deadlines[2];
static unsigned faults[2], requests[2][4], groups;
static float measured_v[2], measured_a[2];
static uint16_t measured_status[2];
static float last_v[2], last_a[2];
static int64_t vi_times[2][100];
static bool cancel_vi, cancel_on, nvs_fail;
static void finish(uint8_t *p, size_t n, size_t *len)
{
    uint16_t crc = mb_crc16(p, n); p[n] = (uint8_t)crc; p[n + 1] = (uint8_t)(crc >> 8); *len = n + 2;
}
esp_err_t rs485_exchange(rs485_port_t ch, const uint8_t *q, size_t n, uint8_t *r, size_t cap, size_t *rn, uint32_t timeout)
{
    (void)n; (void)cap; (void)timeout;
    unsigned kind = q[1] == 3 ? 0 : q[1] == 0x0F ? 1 : q[1] == 6 && q[5] ? 2 : 3;
    if (kind == 1) {
        vi_times[ch][requests[ch][1]] = clock_ms;
        last_v[ch] = ((q[6] << 8) | q[7]) / 10.f;
        last_a[ch] = ((q[8] << 8) | q[9]) / 100.f;
        if (cancel_vi) { cancel_vi = false; assert(charger_command(ch, "stop") == ESP_OK); }
    }
    if (kind == 2 && cancel_on) { cancel_on = false; assert(charger_command(ch, "stop") == ESP_OK); }
    requests[ch][kind]++;
    if (faults[ch] & (1U << kind)) return ESP_ERR_TIMEOUT;
    if (kind == 0) {
        unsigned v = (unsigned)(measured_v[ch] * 10 + .5f), a = (unsigned)(measured_a[ch] * 100 + .5f);
        r[0] = q[0]; r[1] = 3; r[2] = 6; r[3] = v >> 8; r[4] = v; r[5] = a; r[6] = a >> 8;
        r[7] = measured_status[ch] >> 8; r[8] = measured_status[ch]; finish(r, 9, rn);
    } else if (q[1] == 0xDD) {
        memcpy(r, q, 4); r[4] = 0; r[5] = 7; finish(r, 6, rn);
    } else {
        memcpy(r, q, 6); if (faults[ch] & 16) r[5] ^= 1;
        finish(r, 6, rn);
    }
    if (faults[ch] & 32) r[*rn - 1] ^= 1;
    return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t l, TickType_t t) { (void)l; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t l) { (void)l; return 1; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *task)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; *task = (void *)1; return pdPASS; }
void xTaskNotifyGive(TaskHandle_t t) { (void)t; }
unsigned ulTaskNotifyTake(int b, unsigned t) { (void)b; (void)t; return 0; }
const char *esp_err_to_name(int err) { (void)err; return "mock"; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{ (void)h; (void)key; (void)data; (void)size; return nvs_fail ? ESP_FAIL : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{ (void)h; (void)key; (void)data; (void)size; return ESP_FAIL; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static void step(unsigned ch) { service_step(&channels[ch], &deadlines[ch]); }
static void reset(void)
{
    memset(channels, 0, sizeof(channels)); memset(deadlines, 0, sizeof(deadlines));
    memset(requests, 0, sizeof(requests)); memset(faults, 0, sizeof(faults));
    clock_ms = 100; maintenance = cancel_vi = cancel_on = nvs_fail = false;
    for (int i = 0; i < 2; i++) { measured_v[i] = 54.6f; measured_a[i] = 2; measured_status[i] = 0x2082; }
    assert(charger_service_init(3) == ESP_OK);
    for (unsigned i = 0; i < 2; i++) { step(i); step(i); assert(channels[i].s.stopped && channels[i].s.online); }
}
static void start(unsigned ch) { assert(charger_command(ch, "start") == ESP_OK); step(ch); assert(channels[ch].s.active); }
static void advance(unsigned ms)
{
    int64_t until = clock_ms + ms;
    while (clock_ms < until) { clock_ms += 25; step(0); step(1); }
}
static void done(const char *name) { printf("PASS: %s\n", name); groups++; }
int main(void)
{
    uint8_t q[12], r[32]; size_t n;
    assert(charger_power_frame(0, true, q) == 8 && q[4] == 0 && q[5] == 255);
    assert(charger_power_frame(0, false, q) == 8 && q[4] == 0 && q[5] == 0);
    assert(charger_vi_frame(0, 54.6f, 15, q) == 12);
    assert(!memcmp(q, (uint8_t[]){0,15,0,1,0,2,2,34,5,220}, 10));
    memcpy(r, q, 6); finish(r, 6, &n); assert(charger_parse_ack(q, r, n));
    r[5]++; finish(r, 6, &n); assert(!charger_parse_ack(q, r, n));
    assert(!charger_vi_frame(0, NAN, 1, q) && !charger_vi_frame(248, 1, 1, q));
    assert(!charger_vi_frame(0, 1, INFINITY, q));
    done("vendor 0x00 / 0x0F frames, power values, echo and numeric validation");
    memcpy(r, (uint8_t[]){0,3,6,2,34,0xDC,5,0x20,0x82}, 9); finish(r, 9, &n);
    charger_sample_t sample;
    assert(charger_parse_sample(0, r, n, &sample));
    assert(fabsf(sample.voltage - 54.6f) < .01f && sample.current == 15 && sample.status == 0x2082);
    for (unsigned i = 0; i < n; i++) { r[i] ^= 1; assert(!charger_parse_sample(0, r, n, &sample)); r[i] ^= 1; }
    assert(!charger_parse_sample(1, r, n, &sample) && !charger_parse_sample(0, r, n - 1, &sample));
    done("mixed endianness, all-byte CRC corruption, address and size rejection");
    reset(); start(0); start(1);
    assert(requests[0][1] == 1 && last_a[0] == 1);
    advance(14900); assert(requests[0][1] == 1 && requests[1][1] == 1);
    advance(400); assert(requests[0][1] == 2 && requests[1][1] == 2);
    assert(vi_times[0][1] - vi_times[0][0] >= 15000);
    charger_config_t cfg = channels[1].s.cfg; cfg.voltage = 48; cfg.current = .5f;
    assert(charger_configure(1, &cfg) == ESP_OK); advance(14000);
    assert(channels[1].s.active); /* A pending lower target must not falsely trip. */
    measured_v[1] = 48; measured_a[1] = .5f; advance(1500);
    assert(last_v[0] == 54.6f && last_v[1] == 48 && last_a[1] == .5f);
    done("two independent 15-second schedules, elapsed ramp and reduced current target");
    assert(charger_command(0, "stop") == ESP_OK); step(0);
    assert(channels[0].s.stopped && !channels[0].s.active && channels[1].s.active);
    done("immediate OFF bypasses write timer and affects only selected channel");
    reset(); faults[0] = 2; assert(charger_command(0, "start") == ESP_OK); step(0);
    assert(!channels[0].s.active && !channels[0].s.requested && requests[0][2] == 0);
    done("failed initial setpoint never enables power");
    reset(); faults[0] = 4 | 8; assert(charger_command(0, "start") == ESP_OK); step(0);
    assert(!channels[0].s.active && channels[0].power_uncertain);
    cfg = channels[0].s.cfg; cfg.address = 7; assert(charger_configure(0, &cfg) != ESP_OK);
    assert(!charger_maintenance_begin()); step(0); assert(!channels[0].s.stopped);
    faults[0] = 0; advance(1100); assert(channels[0].s.stopped && !channels[0].power_uncertain);
    done("lost ON/OFF acknowledgements retain uncertainty, retry OFF, block address/OTA");
    reset(); cancel_vi = true; assert(charger_command(0, "start") == ESP_OK); step(0); step(0);
    assert(requests[0][2] == 0 && channels[0].s.stopped);
    reset(); cancel_on = true; assert(charger_command(0, "start") == ESP_OK); step(0); step(0);
    assert(!channels[0].s.active && channels[0].s.stopped);
    done("stop during preload or ON acknowledgement cannot resurrect run state");
    reset(); start(0); start(1); faults[0] = 1; advance(2000);
    assert(!strcmp(channels[0].s.trip, "COMM") && !channels[0].s.requested && channels[1].s.active);
    done("three failed reads trip only affected charger");
    reset(); start(0); measured_a[0] = 30; advance(1100);
    assert(!strcmp(channels[0].s.trip, "PWR") && channels[0].s.stopped);
    done("two consecutive power violations produce acknowledged stop");
    reset(); start(0); advance(11000); measured_a[0] = .1f; advance(600);
    assert(!strcmp(channels[0].s.trip, "TRKL") && channels[0].s.stopped);
    reset(); measured_a[0] = .1f; start(0); advance(12000); assert(channels[0].s.active);
    done("trickle requires 10-second guard and previously crossed current threshold");
    for (unsigned mask = 0x10; mask <= 0x40; mask <<= 1) {
        reset(); start(0); measured_status[0] = (uint16_t)mask; advance(600); assert(channels[0].s.stopped);
    }
    reset(); start(0); advance(2000); assert(channels[0].s.active);
    done("OTP, SHORT and OCP protect; demo normal status 0x2082 does not false-trip");
    reset(); start(0); assert(charger_command(0, "lock") == ESP_OK);
    cfg = channels[0].s.cfg; cfg.voltage = 50; assert(charger_configure(0, &cfg) != ESP_OK);
    assert(charger_command(0, "stop") == ESP_OK); step(0); assert(channels[0].s.stopped);
    assert(charger_maintenance_begin()); assert(charger_command(1, "start") != ESP_OK); charger_maintenance_end();
    assert(charger_command(0, "lock") == ESP_OK);
    nvs_fail = true; assert(charger_configure(0, &cfg) != ESP_OK); assert(channels[0].s.cfg.voltage == 54.6f);
    done("lock permits stop; maintenance prevents starts; failed persistence rolls back config");
    reset(); faults[0] = 1; assert(charger_command(0, "scan") == ESP_OK); step(0);
    assert(channels[0].s.scan_done && channels[0].s.scan_found && channels[0].s.scan_address == 7);
    assert(channels[0].s.cfg.address == 0);
    done("idle address discovery does not silently change control destination");
    charger_policy_t policy = {.started_ms = 100, .ramp_ms = 100, .ramp_current = 1};
    charger_defaults(&cfg); cfg.mode = 1; cfg.power = 546; assert(charger_target_current(&cfg) == 10);
    assert(fabsf(charger_ramp(&policy, &cfg, 15100) - 1.75f) < .001f);
    sample = (charger_sample_t){.voltage = 50, .current = 2, .status = 2};
    assert(!charger_protection(&policy, &cfg, &sample, 1000));
    assert(!charger_protection(&policy, &cfg, &sample, 2000));
    assert(fabs(policy.ah - 2.0 / 3600) < 1e-8 && fabs(policy.wh - 100.0 / 3600) < 1e-8);
    double wh = policy.wh; charger_protection(&policy, &cfg, &sample, 10000); assert(policy.wh == wh);
    cfg.voltage = NAN; assert(!charger_config_valid(&cfg));
    done("CP calculation, ramp timing, true Ah/Wh integration and communication-gap exclusion");
    printf("%u charger test groups passed\n", groups); return 0;
}
