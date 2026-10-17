// Timed-action ring (docs/UI_WHEELS_HUD.md "Timed ring"): a white radial ring, thicker than the
// normal bars (theme layout token ring_thickness_wheel), draining from full to empty over an
// action's duration (or filling: charge mode) with a countdown in the middle (max -> 0, one decimal
// under 10 s, whole seconds above) and the action's name under it. Drawn in the radial wheel's centre
// and, with no wheel open, at the HUD centre (healing, bandaging, cooking a grenade, hold-to-use).
// A cancelled action fades the ring out. The math is pure (inline, tests/test_hud.c).
#ifndef DG_TIMED_RING_H
#define DG_TIMED_RING_H

#include "core/base.h"
#include <math.h>
#include <stdio.h>

typedef enum TimedRingMode { TR_DRAIN, TR_CHARGE } TimedRingMode;

// Ring fill 0..1: drain = remaining / duration, charge = elapsed / duration.
DG_INLINE float timed_ring_fill(float duration, float remaining, int mode) {
    if (duration <= 0) return 0;
    float f = remaining / duration;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    return mode == TR_CHARGE ? 1.0f - f : f;
}

// Countdown text: "7.4" under 10 s, "12" (whole seconds, rounded up) above, "0.0" at the end.
DG_INLINE void timed_ring_text(float remaining, char *out, int size) {
    if (remaining < 0) remaining = 0;
    if (remaining < 10.0f) {
        // Round up to the tenth so it never shows 0.0 before the end.
        float t = ceilf(remaining * 10.0f - 1e-4f) / 10.0f;
        if (t <= 0.0f) t = 0.0f;   // no "-0.0"
        snprintf(out, (size_t)size, "%.1f", (double)t);
    } else {
        snprintf(out, (size_t)size, "%d", (int)ceilf(remaining - 1e-4f));
    }
}

// A ring that keeps its last value while fading out after a cancel / finish.
typedef struct TimedRingState {
    bool active;
    float duration, remaining;
    int mode;
    char label[48];
    float alpha;              // presentation (fades in on start, out on stop)
    bool cancelled;           // stopped before the end: fades out faster, label "Cancelled"
} TimedRingState;

// Per frame: feed the action (active = running now). Returns the alpha to draw with.
float timed_ring_update(TimedRingState *s, bool active, float duration, float remaining, int mode, const char *label, float dt);

// Draws at (cx, cy) with the theme's ring radius / thickness (radius_scale 1 = HUD size).
void ui_timed_ring_at(float cx, float cy, float radius_scale, float duration, float remaining, int mode, const char *label, float alpha);
// HUD centre (below the crosshair).
void ui_timed_ring(float duration, float remaining, const char *label);
void ui_timed_ring_state(const TimedRingState *s, float cx, float cy, float radius_scale);

#endif
