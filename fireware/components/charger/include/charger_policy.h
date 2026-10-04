#pragma once
#include "charger_codec.h"
#define CHARGER_WRITE_MS 15000
#define CHARGER_COUNT 2
typedef struct {
    float voltage, current, trickle, power, max_v, max_a, max_p, ramp_step;
    uint32_t ramp_ms;
    uint8_t address, mode, preset, reserved;
    float presets[5][2];
} charger_config_t;
typedef struct {
    int64_t started_ms, ramp_ms, sample_ms;
    float ramp_current;
    double ah, wh;
    unsigned over_count;
    bool seen_current;
} charger_policy_t;
void charger_defaults(charger_config_t *cfg);
bool charger_config_valid(const charger_config_t *cfg);
float charger_target_current(const charger_config_t *cfg);
float charger_ramp(charger_policy_t *state, const charger_config_t *cfg, int64_t now);
/* NULL means continue; otherwise reason to latch and request immediate OFF. */
const char *charger_protection(charger_policy_t *state, const charger_config_t *cfg,
                               const charger_sample_t *sample, int64_t now);
