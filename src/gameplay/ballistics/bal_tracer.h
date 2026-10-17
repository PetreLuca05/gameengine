// Tracers are ammunition (pure): which rounds of a magazine are tracers, and how a tracer looks
// over its flight: dim igniter, full burn, burnout, then dark while the bullet flies on.
#ifndef DG_BAL_TRACER_H
#define DG_BAL_TRACER_H

#include "gameplay/ballistics/bal_data.h"

typedef enum BalTracerState { BAL_TRS_IGNITER, BAL_TRS_BURNING, BAL_TRS_BURNOUT, BAL_TRS_DARK } BalTracerState;

// shot_in_mag: 0-based round index since the magazine was loaded; mag_seed from RecoilState (both
// live in the replicated PlayerSim). all_tracer: an all-tracer magazine (ammo item variant).
// forced: the fire_tracers_only debug cvar.
bool bal_tracer_round(const BalTracerDef *t, u32 mag_seed, int shot_in_mag, int mag_size, bool all_tracer, bool forced);

typedef struct BalTracerLook {
    u8 state;          // BalTracerState
    float intensity;   // 0..1 head brightness (before flicker)
} BalTracerLook;

// dist / time of flight; arcade: no ignition delay.
BalTracerLook bal_tracer_look(const BalTracerDef *t, float dist, float time, bool arcade);
// Motion-blur streak of the head: speed × shutter (≈ v/60 at 60 FPS).
float bal_tracer_streak(float speed, float shutter_s);
// Linear RGB of the burning compound (strontium red, barium green, magnesium white, orange).
vec3 bal_tracer_rgb(int color);

#endif
