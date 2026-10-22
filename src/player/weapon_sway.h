// Viewmodel sway (docs/FIRE_ANIM.md "Viewmodel sway"): the first-person weapon's procedural motion,
// in layers composed additively into ONE camera-space offset that the arms layer applies to the
// drawn arms + item (src/render/arms_layer.c). Client-side and cosmetic: never networked, never the
// body pose, the aim or the camera; remote players' third-person weapons never see it.
//
// Layers (each its own function, each with an ADS multiplier):
//   look      the view's rotation rate (filtered) -> a lagging yaw / pitch / roll + position drag on a
//             damped spring (slight overshoot: the weight)
//   move      camera-relative velocity -> roll into the strafe, shift against it, forward push / pitch,
//             on a softer spring; sudden starts / stops kick it (the gun settles)
//   bob       walk / sprint figure-eight: the phase follows the distance walked on the ground, freezes
//             in the air / standing, the amplitude decays; crouch / sprint scale it; the sprint pose
//             blends in (eased) and out (faster)
// Weapons differ by config only (assets/weapons/viewmodel_sway.json: defaults, per fire class, per
// weapon id). All filters / springs are frame-rate independent (exp(-k dt) filters, semi-implicit
// Euler springs sub-stepped at <= 1/240 s). Pure: no engine state.
#ifndef DG_WEAPON_SWAY_H
#define DG_WEAPON_SWAY_H

#include "core/base.h"
#include "core/dgmath.h"

struct cJSON;

enum { WSW_LOOK, WSW_MOVE, WSW_BOB, WSW_LAYERS };

typedef struct WeaponSwayConfig {
    // Look sway: deg (rotation) / m (position) per deg/s of view rotation (right / up rates).
    float look_filter_hz;             // the rate's exponential filter (high-DPI jitter)
    float look_yaw, look_pitch, look_roll, look_pos;
    float look_max_rot, look_max_pos; // clamps: deg / m (hip)
    float look_k, look_zeta;          // spring stiffness (1/s^2), damping ratio
    // Movement sway: at the walk speed (scaled by speed / walk_speed, up to move_max_speed x).
    float move_roll, move_strafe_pos; // deg into the strafe / m against it
    float move_fwd_pitch, move_fwd_pos;   // deg muzzle down / m back when moving forward
    float move_max_speed;             // x walk speed the input saturates at
    float move_k, move_zeta;
    float move_accel_threshold;       // m/s^2: sudden starts / stops kick the spring ...
    float move_accel_impulse;         // ... by this x the velocity change (m/s -> m/s and deg/s)
    // Walk / sprint bob.
    float walk_speed;                 // m/s: amplitude 1 (pl_walk_speed)
    float bob_freq;                   // cycles per metre on the ground (one cycle = two steps)
    float bob_x, bob_y;               // m: side sway, the two footstep dips
    float bob_roll, bob_pitch, bob_yaw;   // deg
    float bob_attack, bob_decay;      // s: amplitude in / out (airborne, stopping)
    float bob_crouch;                 // amplitude x crouched
    float sprint_bob_mult;            // sprint amplitude x (a distinct, larger motion)
    float sprint_freq_mult;           // sprint stride: cycles per metre x
    float sprint_roll, sprint_yaw;    // deg: the sprint bob's own rotation amplitudes (x sprint_bob_mult)
    // Sprint pose (on top of the hold point's sprint state): rotation deg (pitch up, yaw left, roll), position m.
    vec3 sprint_rot, sprint_pos;
    vec3 tac_rot, tac_pos;            // tactical sprint (a more exaggerated pose), when the game has one
    float sprint_in, sprint_out;      // s, eased in-out
    // ADS: x per layer, rotation / position.
    float ads_rot[WSW_LAYERS], ads_pos[WSW_LAYERS];
    float scale;                      // the whole sway
} WeaponSwayConfig;

typedef struct WsSpring3 { vec3 x, v; } WsSpring3;

typedef struct WeaponSwayState {
    bool init;
    vec2 rate_f;                      // filtered view rate (deg/s: right, up)
    WsSpring3 look_rot, look_pos;
    WsSpring3 move_rot, move_pos;
    vec3 prev_vel;                    // camera-relative velocity (right, up, forward) of the last update
    float bob_phase, bob_amt, bob_sprint;   // phase (rad), amplitude 0..2, sprint mix of the bob shape 0..1
    float sprint_t;                   // linear 0..1 of the sprint pose (eased when applied)
    float tac_t;
    float t;                          // s
} WeaponSwayState;

typedef struct WeaponSwayInput {
    float mouse_dx, mouse_dy;         // this frame's view rotation (deg, right / up); the rate is / dt
    vec3 velocity;                    // world m/s
    vec3 velocity_local;              // camera-relative: x right, y up, z forward (m/s)
    float speed;                      // horizontal m/s
    bool grounded, just_jumped, just_landed;
    float land_impact_speed;          // m/s (vertical speed at the landing)
    bool crouching, sprinting, tactical_sprint;
    float ads_blend;                  // 0 hip .. 1 aimed
    bool fire_pulse;                  // fired this frame (no layer uses it: the recoil is the fire animation's)
    float aim_pitch, aim_yaw;         // rad
    vec2 move_input;                  // x right, y forward (-1..1)
    bool is_slide, is_mantle;
    float bob_scale;                  // 0..1: 0 while the run clip itself carries the gun (no second bob, no sprint pose on top)
} WeaponSwayInput;

typedef struct SwayOutput {
    vec3 pos_offset;                  // camera space m: x right, y up, z back
    quat rot_offset;                  // camera space (= quat_euler of rot_deg)
    float fov_delta;                  // deg (no layer uses it yet)
    vec3 rot_deg;                     // pitch up, yaw left, roll (the hold point's euler order)
    vec3 layer_pos[WSW_LAYERS], layer_rot[WSW_LAYERS];   // each layer (after its ADS multiplier), for tools / tests
    float sprint_w;                   // the eased sprint pose weight
} SwayOutput;

void weapon_sway_defaults(WeaponSwayConfig *c);
void weapon_sway_parse(WeaponSwayConfig *c, const struct cJSON *j);   // only the keys present (overrides)
void weapon_sway_reset(WeaponSwayState *s);
void weapon_sway_update(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, SwayOutput *out);

// The layers (weapon_sway_update composes them; exposed for the tests).
void weapon_sway_look(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos);
void weapon_sway_move(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos);
void weapon_sway_bob(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, vec3 *rot, vec3 *pos, float *sprint_w);

#endif
