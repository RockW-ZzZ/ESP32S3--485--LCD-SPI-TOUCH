#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BUTTON_FILTER_PRESSED  1U
#define BUTTON_FILTER_RELEASED 2U
#define BUTTON_FILTER_LONG     4U
#define BUTTON_KEY1_FILTER_MS  200
#define BUTTON_KEY2_FILTER_MS  30
#define BUTTON_KEY2_HOLD_MS    2000

typedef struct {
    bool raw, stable, long_sent;
    int64_t changed_ms, pressed_ms;
} button_filter_t;

/* Both edges must remain stable for filter_ms. long_ms=0 disables long events.
 * Times use a monotonic clock. Start from an all-zero state. */
static inline unsigned button_filter_step(button_filter_t *s, bool sample,
    int64_t now, unsigned filter_ms, unsigned long_ms)
{
    unsigned result = 0;
    if (sample != s->raw) { s->raw = sample; s->changed_ms = now; }
    if (s->raw != s->stable && now - s->changed_ms >= filter_ms) {
        s->stable = s->raw;
        if (s->stable) { s->pressed_ms = now; s->long_sent = false; }
        result |= s->stable ? BUTTON_FILTER_PRESSED : BUTTON_FILTER_RELEASED;
    }
    if (long_ms && s->stable && s->raw && !s->long_sent && now - s->pressed_ms >= long_ms) {
        s->long_sent = true;
        result |= BUTTON_FILTER_LONG;
    }
    return result;
}
