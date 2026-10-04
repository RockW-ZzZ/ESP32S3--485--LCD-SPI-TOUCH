#pragma once
#include "charger_policy.h"
#include "esp_err.h"
typedef struct {
    charger_config_t cfg;
    charger_sample_t sample;
    double ah, wh;
    uint32_t sessions, trips, failures, writes, write_failures;
    bool ready, online, requested, active, stopped, locked, scanning, scan_done, scan_found;
    int scan_address;
    int64_t last_sample_ms, next_write_ms;
    char trip[12];
} charger_state_t;
esp_err_t charger_service_init(unsigned ready_mask);
esp_err_t charger_snapshot(unsigned channel, charger_state_t *out);
esp_err_t charger_configure(unsigned channel, const charger_config_t *cfg);
esp_err_t charger_command(unsigned channel, const char *command);
/* Atomically blocks new starts/configuration for OTA/reboot, only after both OFF acknowledgements. */
bool charger_maintenance_begin(void);
void charger_maintenance_end(void);
esp_err_t charger_web_start(void);
esp_err_t charger_network_start(void);
esp_err_t charger_network_save(const char *ssid, const char *password);
void charger_network_info(char ip[16], bool *connected);
void charger_network_ssid(char ssid[33]);
