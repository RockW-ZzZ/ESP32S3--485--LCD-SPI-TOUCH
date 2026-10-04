#include "charger_policy.h"
void charger_defaults(charger_config_t *c)
{
    *c = (charger_config_t){.voltage = 54.6f, .current = 15, .trickle = .5f,
        .power = 819, .max_v = 100, .max_a = 50, .max_p = 3000,
        .ramp_step = .05f, .ramp_ms = 1000, .preset = 1,
        .presets = {{48.2f, 10}, {54.6f, 12.5f}, {60, 15}, {67.2f, 8}, {71.4f, 5}}};
}
float charger_target_current(const charger_config_t *c)
{
    return c->mode ? c->power / c->voltage : c->current;
}
bool charger_config_valid(const charger_config_t *c)
{
    if (!c || !(c->max_v >= 1 && c->max_v <= 1000 && c->max_a >= .1f && c->max_a <= 655.35f &&
        c->max_p >= 1 && c->max_p <= 100000 && c->voltage >= .1f && c->voltage <= c->max_v &&
        c->current >= 0 && c->current <= c->max_a && c->trickle >= 0 && c->trickle <= c->max_a &&
        c->power >= 0 && c->power <= c->max_p && c->ramp_step >= .005f && c->ramp_step <= 5) ||
        c->ramp_ms < 200 || c->ramp_ms > 10000 || c->address > 247 || c->mode > 1 || c->preset < 1 || c->preset > 5) return false;
    float target = charger_target_current(c);
    if (!(target >= 0 && target <= c->max_a && c->voltage * target <= c->max_p)) return false;
    /* Presets may exceed user limits after limits are lowered; validate them
     * against wire limits here and against user limits when actually applied. */
    for (unsigned i = 0; i < 5; ++i)
        if (!(c->presets[i][0] >= .1f && c->presets[i][0] <= 1000 && c->presets[i][1] >= 0 && c->presets[i][1] <= 655.35f)) return false;
    return true;
}
float charger_ramp(charger_policy_t *s, const charger_config_t *c, int64_t now)
{
    float target = charger_target_current(c);
    int64_t steps = (now - s->ramp_ms) / c->ramp_ms;
    if (steps > 0) { s->ramp_current += (float)steps * c->ramp_step; s->ramp_ms += steps * c->ramp_ms; }
    if (s->ramp_current > target) s->ramp_current = target;
    return s->ramp_current;
}
const char *charger_protection(charger_policy_t *s, const charger_config_t *c, const charger_sample_t *v, int64_t now)
{
    float watts = v->voltage * v->current;
    int64_t dt = now - s->sample_ms;
    /* Never integrate a communication gap or the time before starting. */
    if (s->sample_ms && dt > 0 && dt <= 2000) {
        s->ah += v->current * (double)dt / 3600000;
        s->wh += watts * (double)dt / 3600000;
    }
    s->sample_ms = now;
    if (v->status & 0x40) return "OCP";
    if (v->status & 0x20) return "SHORT";
    if (v->status & 0x10) return "OTP";
    float limit = c->mode ? c->power : c->voltage * c->current;
    if (limit > c->max_p) limit = c->max_p;
    s->over_count = watts > limit * 1.10f ? s->over_count + 1 : 0;
    if (s->over_count >= 2) return "PWR";
    if (v->current >= c->trickle) s->seen_current = true;
    if (c->trickle > .01f && s->seen_current && now - s->started_ms >= 10000 && v->current < c->trickle) return "TRKL";
    return 0;
}
