// Tracer rounds (docs/BALLISTICS.md "Tracers").
#include "gameplay/ballistics/bal_tracer.h"
#include <math.h>

#define BURNOUT_S 0.15f

bool bal_tracer_round(const BalTracerDef *t, u32 mag_seed, int shot_in_mag, int mag_size, bool all_tracer, bool forced) {
    if (forced || all_tracer) return true;
    if (!t->every) return false;
    if (t->last_n && mag_size > 0 && shot_in_mag >= mag_size - t->last_n) return true;
    // The belt / magazine is loaded with a tracer every Nth round; where the cycle starts is a
    // property of the magazine (its seed), so it is replicated and the same on every machine.
    u32 phase = dg_hash_u32(mag_seed ^ 0x7AC3E5u) % t->every;
    return ((u32)shot_in_mag + phase) % t->every == 0;
}

BalTracerLook bal_tracer_look(const BalTracerDef *t, float dist, float time, bool arcade) {
    BalTracerLook l = { BAL_TRS_BURNING, 1.0f };
    float burn_t = t->burn_s > 0.0f ? t->burn_s : 1e9f;
    // Burn ends at the time limit or the distance limit, whichever the bullet reaches first; the
    // distance limit is turned into a time through the current pace.
    if (t->burn_m > 0.0f && dist > t->burn_m && time > 0.0f) burn_t = fminf(burn_t, time * (t->burn_m / dist));
    if (time >= burn_t + BURNOUT_S) { l.state = BAL_TRS_DARK; l.intensity = 0.0f; return l; }
    if (time >= burn_t) { l.state = BAL_TRS_BURNOUT; l.intensity = 1.0f - (time - burn_t) / BURNOUT_S; return l; }
    if (!arcade && dist < t->ignition_m) {
        // The igniter glows dull red; the main compound catches over the last 20 % of the distance.
        float f = dist / fmaxf(t->ignition_m, 1e-3f);
        l.state = BAL_TRS_IGNITER;
        l.intensity = 0.15f + 0.85f * smoothstepf(0.8f, 1.0f, f);
    }
    return l;
}

float bal_tracer_streak(float speed, float shutter_s) { return speed * shutter_s; }

vec3 bal_tracer_rgb(int color) {
    switch (color) {
    case BAL_TRC_GREEN: return v3(0.25f, 1.0f, 0.3f);
    case BAL_TRC_WHITE: return v3(1.0f, 0.95f, 0.85f);
    case BAL_TRC_ORANGE: return v3(1.0f, 0.55f, 0.15f);
    default: return v3(1.0f, 0.22f, 0.08f);
    }
}
