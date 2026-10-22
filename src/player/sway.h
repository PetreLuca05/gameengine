// Global weapon sway (docs/FIRE_ANIM.md "Global sway", docs/RECOIL.md "Weapon sway"): ONE profile
// for every held item, assets/weapons/sway_global.json. Breathing, look lag, movement bob and the
// landing dip; the hold point applies the result (hip: about the pivot, ADS: about the sight).
//
// Nothing weapon-specific reaches sway_eval: no mass, no class, no attachment except the optic's
// global steadiness (`optic`). Switching weapons therefore never changes sway; weight and
// character come only from the fire animation (src/anim/fire_anim.c). Pure and deterministic.
#ifndef DG_SWAY_H
#define DG_SWAY_H

#include "core/base.h"
#include "core/dgmath.h"

struct cJSON;

enum { SWAY_BREATHING, SWAY_LOOK_LAG, SWAY_BOB, SWAY_LANDING, SWAY_COMPONENTS };

typedef struct SwayProfile {
    // breathing / idle figure-eight
    float sway_rot, sway_freq, breath_pos, breath_freq;
    // look lag (deg / m per rad/s, clamp, spring)
    float lag_rot, lag_pos, lag_max, lag_k, lag_d;
    // movement bob
    float bob_pos, bob_rot, bob_freq;
    // landing dip
    float land_k, land_d, land_gain, land_max, land_pitch;
    // component multipliers at the hip and in ADS (blended with the ADS progress by the hold point)
    float hip[SWAY_COMPONENTS], ads[SWAY_COMPONENTS];
    float hip_scale, ads_scale;       // the whole hip / ADS sway (the pre-fire-animation sway dimmed: 1/2, 1/3)
    float hold_breath_mult;           // ADS sway while holding the breath
    float crouch, prone;              // stance multipliers
    float lever;                      // ADS: translations become the rotation of this lever (m)
    float aim_sway;                   // deg: the reticle-moving aim sway (wpn_ads_sway, off by default) x the optic
} SwayProfile;

typedef struct SwayState {
    float prev_yaw, prev_pitch;
    vec2 lag, lag_v;
    float t, bob_phase, bob_amt;
    float land, land_v, air_vy;
    bool was_grounded;
    float breath_s, shake;
} SwayState;

typedef struct SwayInput {
    float dt;
    bool phase_valid;                 // deterministic phases: clock (s) and the movement stride
    double clock;
    float stride;
    float aim_yaw, aim_pitch;         // view angles (radians)
    vec3 vel_cam;                     // camera space velocity (m/s)
    float vel_world_y;                // world vertical speed (landing)
    bool grounded, sprint, static_pose;
    float sway_scale, bob_scale, look_lag_scale;
    float optic, breath_k, stance, wobble;   // optic steadiness, hold-breath factor, stance, extra wobble 0..1
    float ads_mult;                   // x the ADS multipliers (wpn_sway_ads_scale; 0 = 1)
} SwayInput;

typedef struct SwayOut {
    vec3 hip_rot, hip_pos;            // deg / m (camera space), about the hip sway anchor
    vec3 ads_rot;                     // deg, about the sight (rotation only)
    vec3 lb_hip_rot, lb_hip_pos, lb_ads_rot;   // the look lag + bob + breathing + landing share of the above (the viewmodel sway supersedes it)
} SwayOut;

void sway_profile_defaults(SwayProfile *p);
void sway_profile_parse(SwayProfile *p, const struct cJSON *j);
bool sway_profile_load(SwayProfile *p, const char *path);   // false = defaults kept
// The process-wide profile (loaded once by the game; defaults until then).
const SwayProfile *sway_global(void);
void sway_set_global(const SwayProfile *p);

void sway_reset(SwayState *s);
void sway_eval(SwayState *s, const SwayProfile *p, const SwayInput *in, SwayOut *out);

#endif
