// Viewmodel sway (docs/FIRE_ANIM.md "Viewmodel sway"): the first-person weapon's procedural motion,
// in layers composed additively into ONE camera-space offset that the arms layer applies to the
// drawn arms + item (src/render/arms_layer.c). Client-side and cosmetic: never networked, never the
// body pose, the aim or the camera; remote players' third-person weapons never see it.
//
// Layers (each its own function, each with an ADS multiplier, each can be switched off):
//   look      the view's rotation rate (filtered) -> a lagging yaw / pitch / roll + position drag on a
//             damped spring (slight overshoot: the weight); the ADS transition kicks it a little (the
//             gun settles into the sight instead of sliding)
//   move      camera-relative velocity -> roll into the strafe, shift against it, forward push / pitch,
//             on a softer spring; sudden starts / stops kick it (the gun settles)
//   bob       walk / sprint figure-eight: the phase follows the distance walked on the ground, freezes
//             in the air / standing, the amplitude decays; crouch / sprint scale it; the sprint pose
//             blends in (eased) and out (faster); the phase and its footstep crossings are exposed
//   air       jump (the gun dips, nose up), airborne float from the vertical speed, landing (a dip +
//             pitch on a heavy spring that bounces once, scaled by the impact; crouched less), stance
//             changes (crouch / stand: a small dip). Also a camera pitch dip on hard landings, read
//             apart (weapon_sway_camera_offset; the game decides whether the camera uses it)
//   idle      breathing: two sines with a non-integer ratio + slow noise, fading out while moving and
//             back in when still; crouched its own amplitude; holding the breath steadies it
// Composition: per weapon `weight` (0 light .. 1 heavy) scales every spring's stiffness and every
// amplitude from one value; the total is clamped (the gun never leaves the view); rotations are
// summed in degrees (pitch, yaw, roll) every frame, never accumulated. Weapons differ by config only
// (assets/weapons/viewmodel_sway.json: defaults, per fire class, per weapon id). All filters /
// springs are frame-rate independent (exp(-k dt) filters, semi-implicit Euler springs sub-stepped at
// <= 1/240 s). Pure: no engine state.
#ifndef DG_WEAPON_SWAY_H
#define DG_WEAPON_SWAY_H

#include "core/base.h"
#include "core/dgmath.h"

struct cJSON;

enum { WSW_LOOK, WSW_MOVE, WSW_BOB, WSW_AIR, WSW_IDLE, WSW_LAYERS };

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
    // Jump / fall / land / stance (the air spring: position m up, pitch deg up).
    float jump_impulse;               // m/s down into the spring on a jump (the gun dips) ...
    float jump_pitch;                 // ... and deg/s nose up
    float air_pos, air_pitch;         // airborne: m / deg per m/s of falling speed (the gun floats up, nose up)
    float air_max_pos, air_max_rot;   // their clamps
    float land_min_speed;             // m/s impact below which a landing barely registers
    float land_scale;                 // m/s into the spring per m/s of impact above land_min_speed ...
    float land_max;                   // ... clamped (m/s)
    float land_pitch;                 // deg/s of pitch (muzzle down) per m/s of the landing impulse
    float land_crouch;                // x crouched (a crouch-landing absorbs it)
    float air_k, air_zeta;            // the heavy spring (one bounce)
    float stance_down, stance_up;     // m/s into the spring on crouching (down) / standing up (up)
    // The camera's landing dip (weapon_sway_camera_offset; used only if the game enables it).
    float cam_land_pitch;             // deg/s of camera pitch down per m/s of the landing impulse
    float cam_k, cam_zeta;
    // Idle / breathing.
    float idle_rot;                   // deg (hip, the two sines)
    float idle_pos;                   // m (the slow vertical breath)
    float idle_f1, idle_f2;           // Hz (a non-integer ratio: it never repeats visibly)
    float idle_noise, idle_noise_hz;  // deg of slow value noise on the rotation, its rate
    float idle_fade_in, idle_fade_out;    // s: back in when still, out when moving
    float idle_move_speed;            // m/s above which the idle fades out (movement sway takes over)
    float idle_crouch;                // amplitude x crouched (stance)
    float hold_breath;                // x while holding the breath
    // ADS transition.
    float ads_settle_pitch;           // deg/s into the look spring per unit of ADS change (in: dips, out: lifts)
    float ads_settle_pos;             // m/s back per unit of ADS change
    float ads_fov_delta;              // deg added to the arms FOV x ads_blend (0: the FOV model alone)
    bool ads_ease;                    // smoothstep the ADS weight for the multipliers
    // ADS: x per layer, rotation / position.
    float ads_rot[WSW_LAYERS], ads_pos[WSW_LAYERS];
    bool layer_on[WSW_LAYERS];        // debug: a layer off is zero (its state reset)
    // Composition.
    float weight;                     // 0 light (stiffer, smaller) .. 0.5 as configured .. 1 heavy (softer, larger)
    vec3 pivot_offset;                // m (camera space) added to the rotation pivot at the hip (towards the hands / shoulder)
    vec3 max_rot, max_pos;            // the final clamp: deg per axis / m per axis
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
    int step;                         // a footstep crossed this update: 0 none, 1 left, 2 right
    float sprint_t;                   // linear 0..1 of the sprint pose (eased when applied)
    float tac_t;
    WsSpring3 air_pos, air_rot;       // y (m, + up) / x (deg, pitch up)
    float cam, cam_v;                 // the camera's landing dip (deg, pitch up)
    bool crouch_prev, have_crouch;
    float idle_w;                     // 0..1 (linear, eased when applied)
    float breath;                     // the smoothed hold-breath factor
    float ads_prev;
    bool have_ads;
    float t;                          // s
} WeaponSwayState;

typedef struct WeaponSwayInput {
    float mouse_dx, mouse_dy;         // this frame's view rotation (deg, right / up); the rate is / dt
    vec3 velocity;                    // world m/s
    vec3 velocity_local;              // camera-relative: x right, y up, z forward (m/s)
    float speed;                      // horizontal m/s
    bool grounded, just_jumped, just_landed;
    float land_impact_speed;          // m/s (vertical speed at the landing, either sign)
    bool crouching, sprinting, tactical_sprint;
    float ads_blend;                  // 0 hip .. 1 aimed
    bool fire_pulse;                  // fired this frame (no layer uses it: the recoil is the fire animation's)
    float aim_pitch, aim_yaw;         // rad
    vec2 move_input;                  // x right, y forward (-1..1)
    bool is_slide, is_mantle;
    bool holding_breath;
    float bob_scale;                  // 0..1: 0 while the run clip itself carries the gun (no second bob, no sprint pose on top)
} WeaponSwayInput;

typedef struct SwayOutput {
    vec3 pos_offset;                  // camera space m: x right, y up, z back
    quat rot_offset;                  // camera space (= quat_euler of rot_deg)
    float fov_delta;                  // deg added to the arms FOV
    vec3 rot_deg;                     // pitch up, yaw left, roll (the hold point's euler order)
    vec3 pivot_offset;                // the config's, x the hip share (the sight stays the pivot aimed)
    vec3 layer_pos[WSW_LAYERS], layer_rot[WSW_LAYERS];   // each layer (after its ADS multiplier), for tools / tests
    float sprint_w;                   // the eased sprint pose weight
    float idle_w;                     // the eased idle weight
    float ads;                        // the ADS weight the multipliers used (eased)
    bool clamped;                     // the final clamp acted
} SwayOutput;

void weapon_sway_defaults(WeaponSwayConfig *c);
void weapon_sway_parse(WeaponSwayConfig *c, const struct cJSON *j);   // only the keys present (overrides)
void weapon_sway_reset(WeaponSwayState *s);
void weapon_sway_update(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float dt, SwayOutput *out);

// The layers (weapon_sway_update composes them on the weight-scaled config; exposed for the tests).
// `ads` is the (eased) ADS weight.
void weapon_sway_look(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos);
void weapon_sway_move(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos);
void weapon_sway_bob(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos, float *sprint_w);
void weapon_sway_air(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos);
void weapon_sway_idle(WeaponSwayState *s, const WeaponSwayConfig *c, const WeaponSwayInput *in, float ads, float dt, vec3 *rot, vec3 *pos);

// The stance-change hook: crouching (down = true) dips the gun, standing up lifts it a little.
// weapon_sway_update calls it on the input's crouch edges; a prone / slide system can call it too.
void weapon_sway_stance_change(WeaponSwayState *s, const WeaponSwayConfig *c, bool down);
// The camera's landing dip (deg: pitch up, yaw, roll; pitch < 0 = down) of the last update. Cosmetic;
// the caller decides whether the camera uses it.
vec3 weapon_sway_camera_offset(const WeaponSwayState *s);
// The bob phase (rad, 0 .. 2 pi; the footsteps at pi / 2 and 3 pi / 2) and the footstep the last
// update crossed (0 none, 1 left, 2 right): the footstep audio can follow the bob.
float weapon_sway_bob_phase(const WeaponSwayState *s);
int weapon_sway_footstep(const WeaponSwayState *s);
// The effective config the layers run on: `weight` applied (stiffness / amplitude).
void weapon_sway_weighted(const WeaponSwayConfig *c, WeaponSwayConfig *out);

// The config's fields (parse, save, the tuner): JSON section + key, type, a sensible edit range.
enum { WSF_FLOAT, WSF_VEC3, WSF_ADS, WSF_LAYER, WSF_BOOL };
typedef struct WeaponSwayField {
    const char *section;              // NULL = top level
    const char *key;
    int type;                         // WSF_*
    size_t off;                       // into WeaponSwayConfig (WSF_ADS / WSF_LAYER: the layer index)
    float lo, hi, step;
} WeaponSwayField;
const WeaponSwayField *weapon_sway_fields(int *count);
const char *weapon_sway_layer_name(int layer);   // "look", "move", "bob", "air", "idle"
// The fields of c that differ from base, as one JSON object in the file's layout (only those keys).
struct cJSON *weapon_sway_diff_json(const WeaponSwayConfig *c, const WeaponSwayConfig *base);

#endif
