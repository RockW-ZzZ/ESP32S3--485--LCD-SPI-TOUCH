#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool enabled, physical_down, wait_release, reset_pending;
} touch_gate_t;

/* Initialize with .enabled=true. Caller serializes this state with I2C reads. */
static inline void touch_gate_set(touch_gate_t *g, bool enabled)
{
    if (g->enabled == enabled) return;
    g->enabled = enabled;
    g->wait_release = enabled && g->physical_down;
    g->reset_pending = true;
}

/* Keep observing/acknowledging sensor frames while disabled, but suppress them.
 * A finger already held on re-enable must lift before a new gesture is accepted. */
static inline bool touch_gate_filter(touch_gate_t *g, bool updated, uint8_t count,
                                     bool *reset_input)
{
    if (updated) {
        g->physical_down = count != 0;
        if (!count) g->wait_release = false;
    }
    *reset_input = g->reset_pending;
    g->reset_pending = false;
    return !g->enabled || g->wait_release;
}
