// Dynamic resolution controller (dynres.h).
#include "dynres.h"
#include <math.h>

void dynres_defaults(DynresParams *p, float target_fps, float min_ratio, float max_ratio) {
    p->target_ms = target_fps > 1.0f ? 1000.0f / target_fps : 0.0f;
    p->min_ratio = min_ratio;
    p->max_ratio = max_ratio;
    p->step = 0.05f;
    p->over = 0.95f;
    p->under = 0.75f;
    p->over_hold = 0.4f;
    p->under_hold = 2.0f;
    p->cooldown = 1.5f;
}

static float quantize(float r, float step) { return step > 0.0f ? roundf(r / step) * step : r; }

float dynres_update(DynresState *s, const DynresParams *p, float gpu_ms, float dt) {
    float lo = fminf(p->min_ratio, p->max_ratio), hi = fminf(p->max_ratio, 1.0f);
    if (s->ratio <= 0.0f || s->ratio > hi + 1e-4f) {   // first frame, or the player lowered the maximum
        s->ratio = hi;
        s->gpu_ms = 0.0f;
        s->over_t = s->under_t = 0.0f;
    }
    if (gpu_ms > 0.0f) s->gpu_ms = s->gpu_ms > 0.0f ? s->gpu_ms + (gpu_ms - s->gpu_ms) * fminf(1.0f, dt / 0.25f) : gpu_ms;   // ~0.25 s
    if (s->cool_t > 0.0f) {
        s->cool_t -= dt;
        return s->ratio;
    }
    if (s->gpu_ms <= 0.0f || p->target_ms <= 0.0f) return s->ratio;
    float load = s->gpu_ms / p->target_ms;
    s->over_t = load > p->over ? s->over_t + dt : 0.0f;
    s->under_t = load < p->under ? s->under_t + dt : 0.0f;
    float want = s->ratio;
    // The pixel count goes with the square of the ratio: aim the load at 85 % of the budget.
    float fit = s->ratio * sqrtf(0.85f / fmaxf(load, 1e-3f));
    if (s->over_t >= p->over_hold && s->ratio > lo + 1e-4f) {
        want = quantize(fminf(fit, s->ratio - p->step), p->step);
        if (want >= s->ratio) want = s->ratio - p->step;
    } else if (s->under_t >= p->under_hold && s->ratio < hi - 1e-4f) {
        want = quantize(fminf(fmaxf(fit, s->ratio + p->step), s->ratio + 2.0f * p->step), p->step);   // back up slowly
        if (want <= s->ratio) want = s->ratio + p->step;
    }
    want = fmaxf(lo, fminf(hi, want));
    if (fabsf(want - s->ratio) > 1e-4f) {
        s->ratio = want;
        s->cool_t = p->cooldown;
        s->over_t = s->under_t = 0.0f;
        s->gpu_ms = 0.0f;   // re-measured at the new size
        s->changes++;
    }
    return s->ratio;
}
