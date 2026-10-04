#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "charger_service.h"
#include "buzzer.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct { uint32_t version; uint16_t port; char pin[17]; uint8_t theme[25][3]; } web_config_t;
static const uint8_t default_theme[25][3] = {
    {10,12,20},{240,180,10},{10,170,70},{220,45,35},{20,120,190},{200,130,10},{140,60,190},{90,150,30},
    {240,130,20},{0,100,140},{80,60,220},{230,40,100},{220,40,160},{240,180,0},{100,60,160},{255,60,220},
    {230,220,20},{70,130,180},{255,40,40},{0,170,70},{230,25,25},{70,72,80},{0,200,120},{70,72,80},{50,120,255}};
static web_config_t web = {.version = 1, .port = 8899, .pin = "1234"};
static uint16_t listening_port;
static esp_err_t web_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("charger_web", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "config", &web, sizeof(web));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs); return err;
}
static esp_err_t reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status); httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}
static esp_err_t result(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_OK) return reply(req, "200 OK", "OK");
    return reply(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict", esp_err_to_name(err));
}
static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Exact, bounded form parsing. No truncated PINs/numbers, invalid percent escapes or embedded NUL. */
static bool field(const char *form, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    for (const char *p = form; *p;) {
        const char *end = strchr(p, '&'); if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > k && !memcmp(p, key, k) && p[k] == '=') {
            size_t n = 0;
            for (p += k + 1; p < end; ++p) {
                unsigned char v = (unsigned char)*p;
                if (v == '%') {
                    if (p + 2 >= end || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
                    v = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
                } else if (v == '+') v = ' ';
                if (!v || n + 1 >= cap) return false;
                out[n++] = (char)v;
            }
            out[n] = 0; return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}
static bool number(const char *form, const char *key, double *out)
{
    char buf[48], *end;
    if (!field(form, key, buf, sizeof(buf)) || !buf[0]) return false;
    double value = strtod(buf, &end);
    if (*end || !isfinite(value)) return false;
    *out = value; return true;
}
static bool auth(httpd_req_t *req, const char *form)
{
    char pin[32];
    if ((httpd_req_get_hdr_value_str(req, "X-Charger-PIN", pin, sizeof(pin)) == ESP_OK || field(form, "pin", pin, sizeof(pin))) && !strcmp(pin, web.pin)) return true;
    reply(req, "401 Unauthorized", "PIN ERR"); return false;
}
static void json_string(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; ++s) {
        unsigned char v = (unsigned char)*s;
        if (v < 32 || v == '"' || v == '\\') n += (size_t)snprintf(out + n, cap - n, "\\u%04x", v);
        else out[n++] = *s;
    }
    out[n] = 0;
}
static const char *status_name(uint16_t s)
{
    if (s & 0x40) return "OCP";
    if (s & 0x20) return "SHORT";
    if (s & 0x10) return "OTP";
    static const char *names[] = {"IDLE", "BOOT", "CHG", "FULL", "LIM"};
    return (s & 15) < 5 ? names[s & 15] : "UNKNOWN";
}
static esp_err_t state_json(httpd_req_t *req, unsigned ch, const charger_state_t *s)
{
    char buf[1800], ip[16], reason[80]; bool connected;
    charger_network_info(ip, &connected); json_string(reason, sizeof(reason), s->trip);
    long long age = s->last_sample_ms ? esp_timer_get_time() / 1000 - s->last_sample_ms : -1;
    long long next = s->next_write_ms - esp_timer_get_time() / 1000;
    if (next < 0 || !s->active) next = 0;
    snprintf(buf, sizeof(buf),
        "{\"ch\":%u,\"v\":%.1f,\"a\":%.2f,\"w\":%.2f,\"sv\":%.1f,\"sa\":%.2f,\"tr\":%.2f,\"pm\":%.1f,"
        "\"mode\":%u,\"preset\":%u,\"addr\":%u,\"ah\":%.4f,\"wh\":%.4f,\"cnt\":%lu,\"tripN\":%lu,\"trip\":\"%s\","
        "\"st\":\"%s\",\"raw\":%u,\"run\":%s,\"runActive\":%s,\"stopped\":%s,\"rs\":%s,\"ready\":%s,\"lock\":%s,"
        "\"ap\":true,\"wf\":%s,\"ip\":\"%s\",\"estop\":false,\"rst\":%d,\"off\":%lu,\"scanDone\":%s,\"scanFound\":%s,\"scan\":%d,"
        "\"ageMs\":%lld,\"nextWriteMs\":%lld,\"writes\":%lu,\"writeErrors\":%lu,\"buzzer\":%s,\"epoch\":%lld}",
        ch + 1, s->sample.voltage, s->sample.current, s->sample.voltage * s->sample.current,
        s->cfg.voltage, charger_target_current(&s->cfg), s->cfg.trickle, s->cfg.mode ? s->cfg.power : s->cfg.voltage * s->cfg.current,
        s->cfg.mode, s->cfg.preset, s->cfg.address, s->ah, s->wh, (unsigned long)s->sessions, (unsigned long)s->trips, reason,
        s->online ? status_name(s->sample.status) : "OFFLINE", s->sample.status,
        s->requested ? "true" : "false", s->active ? "true" : "false", s->stopped ? "true" : "false", s->online ? "true" : "false",
        s->ready ? "true" : "false", s->locked ? "true" : "false", connected ? "true" : "false", ip, (int)esp_reset_reason(), (unsigned long)s->failures,
        s->scan_done ? "true" : "false", s->scan_found ? "true" : "false", s->scan_address, age, next,
        (unsigned long)s->writes, (unsigned long)s->write_failures, buzzer_is_on() ? "true" : "false", (long long)time(NULL));
    httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, buf);
}
static void reboot_task(void *arg)
{
    (void)arg; vTaskDelay(pdMS_TO_TICKS(1500)); buzzer_set(false); esp_restart();
}
static esp_err_t reboot(httpd_req_t *req)
{
    if (xTaskCreate(reboot_task, "web_reboot", 3072, NULL, 2, NULL) != pdPASS) {
        charger_maintenance_end(); return reply(req, "500 Internal Server Error", "Cannot schedule restart");
    }
    return reply(req, "200 OK", "Restarting");
}
static esp_err_t ota(httpd_req_t *req)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || req->content_len <= 0 || (size_t)req->content_len > partition->size)
        return reply(req, "400 Bad Request", "Upload the application .bin (raw body), within OTA partition size");
    if (!charger_maintenance_begin()) return reply(req, "409 Conflict", "Stop both chargers and wait for OFF acknowledgements first");
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(partition, req->content_len, &handle);
    if (err != ESP_OK) { charger_maintenance_end(); return result(req, err); }
    char buf[2048]; int left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) break;
        left -= n;
    }
    if (err == ESP_OK) err = esp_ota_end(handle);
    else esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) { charger_maintenance_end(); return reply(req, "400 Bad Request", esp_err_to_name(err)); }
    return reboot(req);
}
static esp_err_t handler(httpd_req_t *req)
{
    /* One HTTP task owns web settings. Worker state is accessed only via the service API. */
    char form[1536] = {0}, path[40], tmp[256];
    size_t path_len = strcspn(req->uri, "?");
    if (path_len >= sizeof(path)) return reply(req, "404 Not Found", "Not found");
    memcpy(path, req->uri, path_len); path[path_len] = 0;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len >= sizeof(form)) return reply(req, "400 Bad Request", "Query too long");
    if (query_len) httpd_req_get_url_query_str(req, form, sizeof(form));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (!strcmp(path, "/update")) {
        if (req->method != HTTP_POST) return reply(req, "405 Method Not Allowed", "POST required");
        return auth(req, form) ? ota(req) : ESP_OK;
    }
    if (req->method == HTTP_POST && req->content_len) {
        size_t offset = strlen(form);
        if (offset) form[offset++] = '&';
        if (req->content_len >= (int)(sizeof(form) - offset)) return reply(req, "400 Bad Request", "Body too long");
        int left = req->content_len;
        while (left > 0) {
            int n = httpd_req_recv(req, form + offset, left);
            if (n <= 0) return reply(req, "408 Request Timeout", "Incomplete body");
            offset += n; left -= n;
        }
        form[offset] = 0;
    }
    if (!strcmp(path, "/")) {
        extern const unsigned char start[] asm("_binary_index_html_start");
        extern const unsigned char end[] asm("_binary_index_html_end");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, (const char *)start, end - start);
    }
    double value;
    if (!number(form, "ch", &value) || value < 1 || value > 2 || value != (int)value)
        return reply(req, "400 Bad Request", "Channel must be 1 or 2");
    unsigned ch = (unsigned)value - 1;
    charger_state_t s;
    if (charger_snapshot(ch, &s) != ESP_OK) return reply(req, "503 Service Unavailable", "Service unavailable");
    if (!strcmp(path, "/state")) return state_json(req, ch, &s);
    if (!strcmp(path, "/settings") && req->method == HTTP_GET) {
        snprintf(tmp, sizeof(tmp), "{\"port\":%u,\"maxV\":%.1f,\"maxA\":%.2f,\"maxP\":%.1f,\"rampMs\":%lu,\"rampSt\":%.3f,\"addr\":%u,\"writeSeconds\":15}",
            web.port, s.cfg.max_v, s.cfg.max_a, s.cfg.max_p, (unsigned long)s.cfg.ramp_ms, s.cfg.ramp_step, s.cfg.address);
        httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, tmp);
    }
    if (!strcmp(path, "/presets")) {
        snprintf(tmp, sizeof(tmp), "[[%.1f,%.2f],[%.1f,%.2f],[%.1f,%.2f],[%.1f,%.2f],[%.1f,%.2f]]",
            s.cfg.presets[0][0], s.cfg.presets[0][1], s.cfg.presets[1][0], s.cfg.presets[1][1], s.cfg.presets[2][0], s.cfg.presets[2][1],
            s.cfg.presets[3][0], s.cfg.presets[3][1], s.cfg.presets[4][0], s.cfg.presets[4][1]);
        httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, tmp);
    }
    if (!strcmp(path, "/theme") && req->method == HTTP_GET) {
        char colors[260]; size_t n = 0;
        colors[n++] = '[';
        for (unsigned i = 0; i < 25; ++i) n += (size_t)snprintf(colors + n, sizeof(colors) - n, "%s\"#%02x%02x%02x\"", i ? "," : "", web.theme[i][0], web.theme[i][1], web.theme[i][2]);
        colors[n++] = ']'; colors[n] = 0;
        httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, colors);
    }
    /* All mutations require POST plus the configured PIN. No demo master PIN. */
    if (req->method != HTTP_POST && strcmp(path, "/wifiget")) return reply(req, "405 Method Not Allowed", "POST required");
    if (!auth(req, form)) return ESP_OK;
    charger_config_t cfg = s.cfg;
    if (!strcmp(path, "/setparam")) {
        if (!field(form, "field", tmp, sizeof(tmp)) || !number(form, "val", &value)) return result(req, ESP_ERR_INVALID_ARG);
        if (!strcmp(tmp, "setV")) cfg.voltage = value;
        else if (!strcmp(tmp, "setA") && !cfg.mode) cfg.current = value;
        else if (!strcmp(tmp, "trickle")) cfg.trickle = value;
        else if (!strcmp(tmp, "pwrMax") && cfg.mode) cfg.power = value;
        else return result(req, ESP_ERR_INVALID_ARG);
        if (!cfg.mode) cfg.power = cfg.voltage * cfg.current;
        return result(req, charger_configure(ch, &cfg));
    }
    if (!strcmp(path, "/btn")) {
        if (!field(form, "id", tmp, sizeof(tmp))) return result(req, ESP_ERR_INVALID_ARG);
        if (!strcmp(tmp, "cc")) {
            cfg.current = charger_target_current(&cfg); cfg.power = cfg.voltage * cfg.current; cfg.mode ^= 1;
            return result(req, charger_configure(ch, &cfg));
        }
        if (!strcmp(tmp, "run")) return reply(req, "400 Bad Request", "Use explicit start/stop to avoid stale toggles");
        if (!strcmp(tmp, "ap")) return reply(req, "409 Conflict", "AP stays enabled for configuration without LCD UI");
        return result(req, charger_command(ch, tmp));
    }
    if (!strcmp(path, "/preset") || !strcmp(path, "/preset_set")) {
        if (!number(form, "n", &value) || value < 1 || value > 5 || value != (int)value) return result(req, ESP_ERR_INVALID_ARG);
        unsigned p = (unsigned)value - 1;
        if (!strcmp(path, "/preset_set")) {
            double v, a;
            if (!number(form, "v", &v) || !number(form, "a", &a) || v > cfg.max_v || a > cfg.max_a || v * a > cfg.max_p) return result(req, ESP_ERR_INVALID_ARG);
            cfg.presets[p][0] = v; cfg.presets[p][1] = a;
        } else { cfg.voltage = cfg.presets[p][0]; cfg.current = cfg.presets[p][1]; cfg.power = cfg.voltage * cfg.current; cfg.preset = p + 1; }
        return result(req, charger_configure(ch, &cfg));
    }
    if (!strcmp(path, "/scan")) return result(req, charger_command(ch, "scan"));
    if (!strcmp(path, "/settings")) {
        if (s.requested || s.active) return reply(req, "409 Conflict", "Stop this charger before editing system settings");
        double mv, ma, mp, rm, rs, address, port;
        if (!number(form, "maxV", &mv) || !number(form, "maxA", &ma) || !number(form, "maxP", &mp) ||
            !number(form, "rampMs", &rm) || !number(form, "rampSt", &rs) || !number(form, "addr", &address) || !number(form, "port", &port) ||
            rm < 200 || rm > 10000 || rm != (int)rm || address < 0 || address > 247 || address != (int)address || port < 1024 || port > 65535 || port != (int)port)
            return result(req, ESP_ERR_INVALID_ARG);
        cfg.max_v = mv; cfg.max_a = ma; cfg.max_p = mp; cfg.ramp_ms = (uint32_t)rm; cfg.ramp_step = rs; cfg.address = (uint8_t)address;
        esp_err_t err = charger_configure(ch, &cfg);
        if (err == ESP_OK && web.port != (uint16_t)port) {
            uint16_t old = web.port; web.port = (uint16_t)port; err = web_save(); if (err != ESP_OK) web.port = old;
        }
        return result(req, err);
    }
    if (!strcmp(path, "/theme") || !strcmp(path, "/theme/reset")) {
        uint8_t colors[25][3];
        if (!strcmp(path, "/theme/reset")) memcpy(colors, default_theme, sizeof(colors));
        else {
            if (!field(form, "c", tmp, sizeof(tmp)) || strlen(tmp) != 174) return result(req, ESP_ERR_INVALID_ARG);
            for (unsigned i = 0; i < 25; ++i) {
                if (i && tmp[i * 7 - 1] != ',') return result(req, ESP_ERR_INVALID_ARG);
                for (unsigned j = 0; j < 3; ++j) {
                    int a = hex(tmp[i * 7 + j * 2]), b = hex(tmp[i * 7 + j * 2 + 1]);
                    if (a < 0 || b < 0) return result(req, ESP_ERR_INVALID_ARG);
                    colors[i][j] = (uint8_t)(a * 16 + b);
                }
            }
        }
        uint8_t old[25][3]; memcpy(old, web.theme, sizeof(old)); memcpy(web.theme, colors, sizeof(colors));
        esp_err_t err = web_save(); if (err != ESP_OK) memcpy(web.theme, old, sizeof(old));
        return result(req, err);
    }
    if (!strcmp(path, "/wifiget")) {
        char ssid[33], escaped[200]; charger_network_ssid(ssid); json_string(escaped, sizeof(escaped), ssid);
        snprintf(tmp, sizeof(tmp), "{\"ssid\":\"%s\"}", escaped);
        httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, tmp);
    }
    if (!strcmp(path, "/wifisave")) {
        char ssid[33], password[64];
        if (!field(form, "ssid", ssid, sizeof(ssid)) || !field(form, "pass", password, sizeof(password))) return result(req, ESP_ERR_INVALID_ARG);
        return result(req, charger_network_save(ssid, password));
    }
    if (!strcmp(path, "/pinset")) {
        char pin[17];
        if (!field(form, "new", pin, sizeof(pin)) || strlen(pin) < 4) return result(req, ESP_ERR_INVALID_ARG);
        char old[17]; memcpy(old, web.pin, sizeof(old)); strcpy(web.pin, pin);
        esp_err_t err = web_save(); if (err != ESP_OK) memcpy(web.pin, old, sizeof(old));
        return result(req, err);
    }
    if (!strcmp(path, "/buzzer")) {
        if (!number(form, "on", &value) || (value != 0 && value != 1)) return result(req, ESP_ERR_INVALID_ARG);
        return result(req, buzzer_set(value == 1));
    }
    if (!strcmp(path, "/reboot")) {
        if (!charger_maintenance_begin()) return reply(req, "409 Conflict", "Stop both chargers and wait for OFF acknowledgements first");
        return reboot(req);
    }
    return reply(req, "404 Not Found", "Not found");
}
static esp_err_t redirect(httpd_req_t *req)
{
    char url[80], host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK || strspn(host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:") != strlen(host)) strcpy(host, "192.168.4.1");
    char *colon = strchr(host, ':'); if (colon) *colon = 0;
    snprintf(url, sizeof(url), "http://%s:%u/", host, listening_port);
    httpd_resp_set_status(req, "302 Found"); httpd_resp_set_hdr(req, "Location", url); return httpd_resp_sendstr(req, "Open charger control");
}
esp_err_t charger_web_start(void)
{
    memcpy(web.theme, default_theme, sizeof(web.theme));
    nvs_handle_t nvs;
    if (nvs_open("charger_web", NVS_READONLY, &nvs) == ESP_OK) {
        web_config_t saved; size_t size = sizeof(saved);
        if (nvs_get_blob(nvs, "config", &saved, &size) == ESP_OK && size == sizeof(saved) && saved.version == 1 && saved.port >= 1024 &&
            memchr(saved.pin, 0, sizeof(saved.pin)) && strlen(saved.pin) >= 4) web = saved;
        nvs_close(nvs);
    }
    listening_port = web.port;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = listening_port; cfg.stack_size = 12288; cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 5; cfg.send_wait_timeout = 5; cfg.lru_purge_enable = true;
    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg); if (err != ESP_OK) return err;
    httpd_uri_t uri = {.uri = "/*", .method = HTTP_GET, .handler = handler};
    err = httpd_register_uri_handler(server, &uri); if (err != ESP_OK) return err;
    uri.method = HTTP_POST; err = httpd_register_uri_handler(server, &uri); if (err != ESP_OK) return err;
    cfg.server_port = 80; cfg.ctrl_port++; cfg.stack_size = 4096;
    httpd_handle_t portal;
    err = httpd_start(&portal, &cfg); if (err != ESP_OK) return err;
    uri.method = HTTP_GET; uri.handler = redirect;
    err = httpd_register_uri_handler(portal, &uri);
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("charger");
        mdns_instance_name_set("Dual RS485 charger");
        mdns_service_add(NULL, "_http", "_tcp", listening_port, NULL, 0);
    }
    ESP_LOGI("web", "Web control port %u; two independent chargers", listening_port);
    return err;
}
