#include <string.h>
#include <stdio.h>
#include "charger_service.h"
#include "rs485.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    charger_state_t s;
    charger_policy_t policy;
    charger_config_t applied;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    unsigned index, bad_reads;
    bool power_uncertain;
    uint32_t epoch;
    int scan_cursor;
    int64_t stop_retry_ms, saved_ms;
} channel_t;
typedef struct {
    uint32_t version;
    charger_config_t cfg;
    double ah, wh;
    uint32_t sessions, trips;
    char trip[12];
} saved_t;
static channel_t channels[CHARGER_COUNT];
static SemaphoreHandle_t control_lock;
static bool maintenance;
static const char *TAG = "charger";
static int64_t ms(void) { return esp_timer_get_time() / 1000; }
static void take(channel_t *c) { xSemaphoreTake(c->lock, portMAX_DELAY); }
static void give(channel_t *c) { xSemaphoreGive(c->lock); }

/* Caller holds this channel's lock; separate NVS handles serialize through IDF. */
static esp_err_t save(channel_t *c)
{
    saved_t data = {.version = 1, .cfg = c->s.cfg, .ah = c->policy.ah, .wh = c->policy.wh,
        .sessions = c->s.sessions, .trips = c->s.trips};
    memcpy(data.trip, c->s.trip, sizeof(data.trip));
    char key[8]; snprintf(key, sizeof(key), "port%u", c->index + 1);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("charger", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, key, &data, sizeof(data));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    c->saved_ms = ms();
    if (err != ESP_OK) ESP_LOGE(TAG, "Port %u save: %s", c->index + 1, esp_err_to_name(err));
    return err;
}
static void load(channel_t *c)
{
    charger_defaults(&c->s.cfg);
    nvs_handle_t nvs;
    if (nvs_open("charger", NVS_READONLY, &nvs) != ESP_OK) return;
    char key[8]; snprintf(key, sizeof(key), "port%u", c->index + 1);
    saved_t data; size_t size = sizeof(data);
    esp_err_t err = nvs_get_blob(nvs, key, &data, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size != sizeof(data) || data.version != 1 || !charger_config_valid(&data.cfg)) return;
    c->s.cfg = data.cfg;
    if (data.ah >= 0 && data.ah < 1e12 && data.wh >= 0 && data.wh < 1e12) {
        c->policy.ah = data.ah; c->policy.wh = data.wh;
    }
    c->s.sessions = data.sessions; c->s.trips = data.trips;
    memcpy(c->s.trip, data.trip, sizeof(c->s.trip)); c->s.trip[sizeof(c->s.trip) - 1] = 0;
}
static bool exchange(channel_t *c, uint8_t *tx, size_t n, uint8_t *rx, size_t *rn, unsigned timeout)
{
    return n && rs485_exchange((rs485_port_t)c->index, tx, n, rx, 32, rn, timeout) == ESP_OK;
}
static bool power(channel_t *c, uint8_t address, bool on)
{
    uint8_t tx[8], rx[32]; size_t rn = 0;
    size_t n = charger_power_frame(address, on, tx);
    return exchange(c, tx, n, rx, &rn, 250) && charger_parse_ack(tx, rx, rn);
}
static bool write_vi(channel_t *c, uint8_t a, float voltage, float current)
{
    uint8_t tx[12], rx[32]; size_t rn = 0;
    size_t n = charger_vi_frame(a, voltage, current, tx);
    bool ok = exchange(c, tx, n, rx, &rn, 250) && charger_parse_ack(tx, rx, rn);
    take(c);
    c->s.writes++; if (!ok) c->s.write_failures++;
    /* Failed writes also wait 15 s; immediate OFF is a separate operation. */
    c->s.next_write_ms = ms() + CHARGER_WRITE_MS;
    give(c);
    return ok;
}
static bool read_sample(channel_t *c, uint8_t a, charger_sample_t *s, unsigned timeout)
{
    uint8_t tx[8], rx[32]; size_t rn = 0;
    size_t n = charger_read_frame(a, tx);
    return exchange(c, tx, n, rx, &rn, timeout) && charger_parse_sample(a, rx, rn, s);
}
static void trip(channel_t *c, const char *reason)
{
    c->s.requested = false; c->s.stopped = false; c->epoch++;
    c->stop_retry_ms = 0; c->s.trips++;
    snprintf(c->s.trip, sizeof(c->s.trip), "%s", reason);
    c->policy.sample_ms = 0;
    ESP_LOGW(TAG, "Port %u protection: %s; requesting OFF", c->index + 1, reason);
}
static void service_step(channel_t *c, int64_t *next_read)
{
    take(c);
    charger_state_t s = c->s;
    uint32_t epoch = c->epoch;
    bool stopping = !s.requested && !s.stopped && ms() >= c->stop_retry_ms;
    int scan = c->scan_cursor;
    give(c);
    if (stopping) {
        bool ok = power(c, s.cfg.address, false);
        take(c);
        if (epoch == c->epoch) {
            c->s.stopped = ok;
            if (ok) { c->s.active = false; c->power_uncertain = false; c->policy.sample_ms = 0; save(c); }
            c->stop_retry_ms = ms() + 1000;
        }
        give(c);
        return;
    }
    if (s.scanning) {
        uint8_t found = 0;
        bool ok;
        if (scan < 0) {
            uint8_t tx[6], rx[32]; size_t rn = 0;
            size_t n = charger_query_frame(tx);
            ok = exchange(c, tx, n, rx, &rn, 250) && charger_parse_address(rx, rn, &found);
        } else {
            charger_sample_t sample;
            found = (uint8_t)scan;
            ok = read_sample(c, found, &sample, 80);
        }
        take(c);
        if (c->s.scanning) {
            if (ok || scan >= 247) {
                c->s.scanning = false; c->s.scan_done = true; c->s.scan_found = ok;
                c->s.scan_address = ok ? found : -1;
                /* Discovery never silently redirects a live controller. User saves address. */
            } else c->scan_cursor++;
        }
        give(c);
        return;
    }
    if (s.requested && !s.active) {
        /* One initial preload before ON prevents enabling a charger's old
         * retained settings. Thereafter all setpoint writes are 15 s apart. */
        float target = charger_target_current(&s.cfg);
        float initial = target < 1 ? target : 1;
        bool ok = write_vi(c, s.cfg.address, s.cfg.voltage, initial);
        take(c);
        bool current = c->epoch == epoch && c->s.requested;
        if (ok && current) c->power_uncertain = true;
        give(c);
        if (ok && current) ok = power(c, s.cfg.address, true);
        take(c);
        if (current && c->epoch == epoch && c->s.requested) {
            if (ok) {
                c->s.active = true; c->s.sessions++;
                c->applied = s.cfg;
                c->policy = (charger_policy_t){.started_ms = ms(), .ramp_ms = ms(), .ramp_current = initial};
                c->s.trip[0] = 0; c->bad_reads = 0;
                save(c);
            } else trip(c, "START");
        }
        give(c);
        return;
    }
    if (ms() >= *next_read) {
        charger_sample_t sample;
        bool ok = read_sample(c, s.cfg.address, &sample, 250);
        *next_read = ms() + 500;
        take(c);
        c->s.online = ok;
        if (ok) {
            c->bad_reads = 0; c->s.sample = sample; c->s.last_sample_ms = ms();
            if (c->s.active && c->s.requested) {
                /* Pending lower setpoints have not reached the charger yet.
                 * Use the acknowledged VI target for power protection, while
                 * trickle and the absolute power ceiling remain current. */
                charger_config_t protection = c->applied;
                protection.trickle = c->s.cfg.trickle;
                protection.max_p = c->s.cfg.max_p;
                const char *reason = charger_protection(&c->policy, &protection, &sample, ms());
                if (reason) trip(c, reason);
            }
        } else {
            c->s.failures++; c->bad_reads++; c->policy.sample_ms = 0;
            if (c->s.requested && c->bad_reads >= 3) trip(c, "COMM");
        }
        give(c);
        /* Re-snapshot immediately so a protection stop precedes any write. */
        return;
    }
    take(c);
    bool due = c->s.active && c->s.requested && ms() >= c->s.next_write_ms;
    charger_config_t cfg = c->s.cfg;
    float current = due ? charger_ramp(&c->policy, &cfg, ms()) : 0;
    epoch = c->epoch;
    if (ms() - c->saved_ms >= 60000 && c->s.active) save(c);
    give(c);
    if (due) {
        bool ok = write_vi(c, cfg.address, cfg.voltage, current);
        take(c);
        if (ok) c->applied = cfg;
        if (!ok && c->epoch == epoch && c->s.requested) trip(c, "WRITE");
        give(c);
        return;
    }
}

static void worker(void *arg)
{
    channel_t *c = arg;
    int64_t next_read = 0;
    for (;;) {
        service_step(c, &next_read);
        /* A trip skips this wait so OFF follows the detected fault immediately. */
        take(c);
        bool stop_now = !c->s.requested && !c->s.stopped && ms() >= c->stop_retry_ms;
        give(c);
        if (stop_now) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(25));
    }
}
esp_err_t charger_service_init(unsigned ready_mask)
{
    control_lock = xSemaphoreCreateMutex();
    if (!control_lock) return ESP_ERR_NO_MEM;
    for (unsigned i = 0; i < CHARGER_COUNT; ++i) {
        channel_t *c = &channels[i]; c->index = i;
        c->lock = xSemaphoreCreateMutex();
        if (!c->lock) return ESP_ERR_NO_MEM;
        load(c); c->s.ready = (ready_mask & (1U << i)) != 0;
        c->s.scan_address = -1;
        if (c->s.ready && xTaskCreate(worker, i ? "charger2" : "charger1", 6144, c, 3, &c->task) != pdPASS)
            c->s.ready = false;
    }
    return ESP_OK;
}
esp_err_t charger_snapshot(unsigned channel, charger_state_t *out)
{
    if (channel >= CHARGER_COUNT || !out || !channels[channel].lock) return ESP_ERR_INVALID_ARG;
    channel_t *c = &channels[channel]; take(c);
    *out = c->s; out->ah = c->policy.ah; out->wh = c->policy.wh;
    give(c); return ESP_OK;
}
esp_err_t charger_configure(unsigned channel, const charger_config_t *cfg)
{
    if (channel >= CHARGER_COUNT || !charger_config_valid(cfg)) return ESP_ERR_INVALID_ARG;
    channel_t *c = &channels[channel];
    xSemaphoreTake(control_lock, portMAX_DELAY); take(c);
    bool address_change = cfg->address != c->s.cfg.address;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!maintenance && !c->s.locked && !c->s.scanning &&
        (!address_change || (!c->s.requested && !c->s.active && !c->power_uncertain)) &&
        (!c->s.requested || c->s.active)) {
        charger_config_t old = c->s.cfg; c->s.cfg = *cfg;
        err = save(c);
        if (err != ESP_OK) c->s.cfg = old;
        else if (address_change) { c->s.stopped = false; c->s.online = false; c->epoch++; c->stop_retry_ms = 0; }
    }
    give(c); xSemaphoreGive(control_lock);
    if (err == ESP_OK && c->task) xTaskNotifyGive(c->task);
    return err;
}
esp_err_t charger_command(unsigned channel, const char *command)
{
    if (channel >= CHARGER_COUNT || !command) return ESP_ERR_INVALID_ARG;
    channel_t *c = &channels[channel];
    xSemaphoreTake(control_lock, portMAX_DELAY); take(c);
    esp_err_t err = ESP_OK;
    if (!strcmp(command, "stop")) {
        c->s.requested = false; c->s.stopped = false; c->epoch++; c->stop_retry_ms = 0;
        c->s.scanning = false;
    } else if (maintenance || !c->s.ready) err = ESP_ERR_INVALID_STATE;
    else if (!strcmp(command, "lock")) c->s.locked = !c->s.locked;
    else if (c->s.locked || c->s.scanning) err = ESP_ERR_INVALID_STATE;
    else if (!strcmp(command, "start")) {
        if (!c->s.online || !c->s.stopped || c->s.requested || (c->s.sample.status & 0x70)) err = ESP_ERR_INVALID_STATE;
        else { c->s.requested = true; c->s.stopped = false; c->epoch++; }
    } else if (!strcmp(command, "scan")) {
        if (c->s.requested || c->s.active) err = ESP_ERR_INVALID_STATE;
        else { c->s.scanning = true; c->s.scan_done = c->s.scan_found = false; c->scan_cursor = -1; }
    } else if (!strcmp(command, "clr") || !strcmp(command, "clrall")) {
        if (c->s.requested || !c->s.stopped) err = ESP_ERR_INVALID_STATE;
        else {
            c->policy.ah = c->policy.wh = 0;
            if (!strcmp(command, "clrall")) { c->s.sessions = c->s.trips = 0; c->s.trip[0] = 0; }
            err = save(c);
        }
    } else err = ESP_ERR_INVALID_ARG;
    give(c); xSemaphoreGive(control_lock);
    if (err == ESP_OK && c->task) xTaskNotifyGive(c->task);
    return err;
}
bool charger_maintenance_begin(void)
{
    xSemaphoreTake(control_lock, portMAX_DELAY);
    bool ok = !maintenance;
    for (unsigned i = 0; i < CHARGER_COUNT; ++i) take(&channels[i]);
    for (unsigned i = 0; i < CHARGER_COUNT; ++i) {
        charger_state_t *s = &channels[i].s;
        if (s->requested || s->active || s->scanning || channels[i].power_uncertain || (s->ready && !s->stopped)) ok = false;
    }
    if (ok) maintenance = true;
    for (unsigned i = 0; i < CHARGER_COUNT; ++i) give(&channels[i]);
    xSemaphoreGive(control_lock); return ok;
}
void charger_maintenance_end(void)
{
    xSemaphoreTake(control_lock, portMAX_DELAY); maintenance = false; xSemaphoreGive(control_lock);
}
