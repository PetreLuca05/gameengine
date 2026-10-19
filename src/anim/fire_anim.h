// Gun-driven fire animation (docs/FIRE_ANIM.md). Pure and deterministic: no engine state, the
// same shots and the same fixed ticks give bit-identical output (tests/test_fire_anim.c).
//
// Weight and impact come from timing asymmetry and mass, not from bigger amplitudes:
//   - the kick lands in ONE tick (displacement injected on the shot tick, no ease-in), the
//     return takes 10-30x longer on an under-damped spring that overshoots past rest;
//   - every channel integrates F / m: a heavier gun (or inertia, for the rotations) peaks lower
//     and settles slower, a light one snaps and settles fast; attachments add mass at their socket;
//   - a chain with lag: gun -> hands (1-2 ticks, soft springs) -> shoulders (push back) ->
//     camera (2-3 ticks after the shoulders, its own overshoot). The camera never leads the gun.
//
// Layers evaluated here (the caller applies them after the hold point, global sway and the ADS
// alignment, before the hand IK):
//   1. fire pose delta   per-class keyed curves (first shot / sustained / release settle),
//                        auto fire crops each curve at the next shot so the motion compounds
//   2. mass springs      pitch / yaw / roll / kickback / vertical, randomized per shot
//   3. hands + body      hand error (the hands trail the gun), shoulder push-back
//   4. mechanical        slide / bolt travel (locks back on empty), attachment wobble, eject cue
//   5. camera            delayed camera spring, FOV punch, roll twitch, screen kick, blur spike
// Sway is NOT here: it is one global profile for every weapon (src/player/sway.c).
//
// Conventions match the hold point kick input: position camera space metres (x right, y up,
// z back), rotation degrees (pitch + = muzzle up, yaw + = muzzle RIGHT, roll + = cant left).
#ifndef DG_FIRE_ANIM_H
#define DG_FIRE_ANIM_H

#include "core/base.h"
#include "core/dgmath.h"

struct cJSON;

enum { FA_PITCH, FA_YAW, FA_ROLL, FA_BACK, FA_UP, FA_CH };
#define FA_MAX_KEYS 12
#define FA_CAM_DELAY_MAX 6
#define FA_MAX_ATT_MASS 6

typedef enum FireClassId { FC_RIFLE, FC_PISTOL, FC_SMG, FC_LMG, FC_SHOTGUN, FC_DMR, FC_SNIPER, FC_CROSSBOW, FC_COUNT } FireClassId;
const char *fire_class_name(int c);
int fire_class_from_name(const char *s);   // -1 if unknown

// Keyed pose delta: t in seconds, v per channel in units of the channel's peak (1 = the class peak).
typedef struct FaCurve {
    int n;
    float t[FA_MAX_KEYS];
    float v[FA_MAX_KEYS][FA_CH];
} FaCurve;

// One spring channel, authored at the class reference mass: the kick peak, the per-shot random
// range (multiplier), the natural frequency and damping ratio, the ADS scale and how much of the
// kick carries on as velocity (momentum: sustained fire rides up instead of resetting).
typedef struct FaChannel {
    float peak;          // deg (rotations) or m (translations) at the reference mass
    float rnd_min, rnd_max;
    float omega;         // rad/s at the reference mass
    float zeta;          // damping ratio (constant with mass: every class keeps its overshoot)
    float ads;           // ADS impulse scale
    float follow;        // fraction of the kick that continues as velocity (0..1)
    float sign_rnd;      // 0..1: chance-weighted random sign (yaw jitter, roll flip)
    float ride;          // sustained fire: each shot moves the spring's rest by ride x the kick
    float ride_max;      // ... up to ride_max x the kick (the hands fight it, then lose ground)
} FaChannel;

// An attachment's contribution (assets/attachments/*.json "fire" block).
typedef struct FaAttMod {
    bool set;
    float mass;                // kg
    float dist;                // m in front of the grip (along the barrel, + = forward)
    float height;              // m above the bore (optics: yaw lag)
    float impulse[FA_CH];      // multipliers on the channel peaks
    float omega_mult, zeta_add;
    float yaw_lag;             // extra yaw inertia multiplier
    float settle_mult;         // stretches the curves (suppressor: longer settle)
    float droop;               // deg of muzzle droop per unit heat (front-heavy devices)
    float fov_punch, screen_kick;   // perceptual multipliers
} FaAttMod;

typedef struct FireAnimDef {
    char cls_name[16];
    int cls;
    float mass, moment, length;           // kg, 0..1 front-heaviness, m (grip to muzzle)
    float ref_mass, ref_moment;           // the class values the channel peaks were authored at
    FaChannel ch[FA_CH];
    float first_mult;                     // first shot of a string: x the impulse
    float hip_scale, ads_scale;           // overall hip / ADS
    FaCurve first, sustained, release;
    float curve_amp[FA_CH];               // curve units -> deg / m
    float settle_time;                    // s without a shot before the next one is a "first" shot
    float ride_release;                   // s: the ride returns when the string ends (the spring overshoots it)
    // layer 3
    float hand_omega, hand_zeta, hand_max_err;   // rad/s, ratio, m
    float shoulder_push, shoulder_omega, shoulder_zeta;   // m of push-back per m of gun kickback
    float trigger_pull;                   // 0..1 trigger finger curl on the shot
    // layer 4
    float slide_travel, slide_time;       // m, s (0 travel = no slide)
    bool slide_lock_empty;
    float bolt_delay;                     // bolt action: manual cycle start after the shot (s, 0 = none)
    float wobble_omega, wobble_zeta, wobble_gain;   // mag / optic / underbarrel secondary spring
    float droop;                          // deg of muzzle droop per unit heat (sum of attachments)
    float heat_per_shot, heat_decay;      // sustained-fire heat 0..1
    // layer 5
    float cam_gain[FA_CH];                // camera response per gun channel (from the shoulder chain)
    int cam_delay;                        // ticks (2-3)
    float cam_omega, cam_zeta;
    float cam_hip;                        // the camera's share at the hip (its kick, roll twitch, FOV punch; 1 aimed)
    float fov_punch, fov_release;         // deg (positive = narrows), s
    float roll_twitch;                    // deg
    float screen_kick, screen_release;    // m toward the camera, s
    float blur_ticks;                     // motion blur spike length (ticks)
    float chroma;                         // chromatic / vignette tick (heavy weapons), 0..1
    // derived by fire_anim_resolve (do not author)
    float k[FA_CH], c[FA_CH], m[FA_CH], J[FA_CH];
    float time_stretch, droop_total, fov_mult, screen_mult;
    FaAttMod att[FA_MAX_ATT_MASS];
    int att_count;
} FireAnimDef;

// Per-shot context.
typedef struct FireShot {
    float ads_t;           // 0 hip .. 1 aimed
    float sub_tick;        // 0..1: fraction of the tick that already elapsed when the shot fired
    u32 seed;              // deterministic per shot
    int mag_left;          // after the shot
    float interval;        // s to the next shot at the current fire rate (auto crop), 0 = semi
    float scale;           // wpn_fire_anim_scale x third-person reduction
} FireShot;

typedef struct FaSpring { float x, v; } FaSpring;

// What the caller applies this tick (sample with fire_anim_sample for render alpha).
typedef struct FireAnimOut {
    vec3 gun_pos, gun_rot;     // layers 1 + 2: the item kick (camera space m / deg)
    vec3 hand_pos, hand_rot;   // layer 3: hand target error relative to the gun (m / deg, |pos| <= hand_max_err)
    float shoulder;            // m of shoulder push-back (clavicles / spine)
    float trigger;             // 0..1 trigger finger pull
    float slide;               // m of slide / bolt travel (+ = back)
    vec3 wobble_rot;           // deg: magazine / optic / underbarrel lag
    vec3 cam_rot;              // deg: camera reaction (pitch up, yaw right, roll)
    float cam_back;            // m
    float fov_punch;           // deg to subtract from the FOV
    float roll_twitch;         // deg
    float screen_kick;         // m toward the camera (whole gun + hands rig)
    float blur;                // 0..1 viewmodel motion blur spike
    float chroma;              // 0..1
} FireAnimOut;

typedef struct FireAnimState {
    // layer 1: the playing curve instance
    float curve_t, curve_len, curve_amp, crop;
    u8 curve_kind;         // 0 none, 1 first, 2 sustained, 3 release
    float since_shot;
    bool kicked;           // a shot landed since the last step (the hands get no feed-forward)
    int string_shots;      // shots in the current string (0 after settling)
    bool firing;           // a string is in progress (release settle plays when it ends)
    float curve_ch[FA_CH]; // per-channel random x sign of the playing curve
    // layer 2
    FaSpring gun[FA_CH];
    float ride[FA_CH];
    float heat;
    // layer 3
    FaSpring hand[FA_CH];  // hand error behind the gun (only the kick displaces it)
    FaSpring shoulder;
    float trigger;
    // layer 4
    float slide, slide_t;
    bool slide_active, slide_locked, eject_pending, eject_done;
    FaSpring wobble[2];
    // layer 5
    float shoulder_hist[FA_CAM_DELAY_MAX + 1];
    FaSpring cam[FA_CH];
    float fov, fov_t, roll_tw, screen, blur;
    float cam_k;                          // the last shot's camera share (cam_hip .. 1 by its ADS)
    int roll_ticks, blur_left;
    float roll_sign, roll_amp;
    u32 tick;
    float hand_hist[FA_CAM_DELAY_MAX + 1][FA_CH];
    // previous tick output (render interpolation)
    FireAnimOut prev, last;
} FireAnimState;

void fire_anim_class_defaults(FireAnimDef *d, int cls);
// Class JSON (assets/weapons/fire_poses/<class>.json) or a weapon's "fire_anim" override block:
// only the keys present change.
void fire_anim_parse(FireAnimDef *d, const struct cJSON *j);
char *fire_anim_to_json(const FireAnimDef *d);   // the full definition (tuner save), caller frees
// Attachment "fire" block.
void fire_anim_att_parse(FaAttMod *m, const struct cJSON *j);
// Adds an attachment's mass / modifiers (before resolve).
void fire_anim_add_attachment(FireAnimDef *d, const FaAttMod *m);
// Derives spring constants, inertias and impulses from the mass model. Call after parsing and
// after every attachment was added.
void fire_anim_resolve(FireAnimDef *d);
// Effective translation mass / rotation inertia per channel (tests, tuner).
float fire_anim_inertia(const FireAnimDef *d, int ch);

void fire_anim_reset(FireAnimState *s);
void fire_anim_shot(FireAnimState *s, const FireAnimDef *d, const FireShot *shot);
// One fixed tick. trigger_held ends a string when released (release settle). perceptual: 0..1
// scale of FOV punch / roll twitch / blur / chroma (Engine Fidelity: 0 at Low).
void fire_anim_step(FireAnimState *s, const FireAnimDef *d, float dt, bool trigger_held, float perceptual, FireAnimOut *out);
// Render interpolation between the previous and the current tick output.
void fire_anim_sample(const FireAnimState *s, const FireAnimOut *cur, float alpha, FireAnimOut *out);
// True once per shot when the slide / bolt is back past half its travel (casing ejection).
bool fire_anim_take_eject(FireAnimState *s);

// Peak / settle metrics of a channel over a recorded series (tests, tuner, fire_compare).
typedef struct FaMetrics { float peak; int peak_tick; int settle_tick; float overshoot; } FaMetrics;
FaMetrics fire_anim_metrics(const float *series, int n, float settle_frac);

#endif
