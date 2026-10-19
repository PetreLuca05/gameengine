// Hold point: the single transform in front of the camera where every held item lives.
//
// Everything about where an item sits is defined relative to the camera (x right, y up, -z
// forward), per hold style and per state, with per-item overrides. Two poses come out:
//   gameplay pose (what the bullet follows, docs/AIM.md):
//     state target (hip / ADS / sprint / crouch / prone / empty / inspect)
//       -> inertialized blend between states (no pops when a state or the item changes)
//       -> ADS computed from the item's sight socket (sight on the view axis at the eye relief)
//       -> item (camera space) = hold point * inverse(anchor socket), soft pitch limit
//       -> weapon collision (forward cast along the item, pull back / up along a curve)  = gp_item
//       -> hold aim: turned about its pivot so the barrel line passes through the aim point P
//          (at most aim_max, faded out by the wall pull and the non-aiming states)       = aim_item
//   drawn pose = aim_item + presentation: look lag, idle sway + breathing, movement bob, landing,
//     the fire kick, the ADS sight anchoring / push, the phone's driven rotation          = item
// Presentation never reaches the gameplay pose, so it never moves a bullet.
// The evaluation is pure and deterministic: the same inputs give bit-identical results, so the
// local player and every remote copy of it solve the same pose (see docs/CAMERA_HOLDPOINT.md).
#ifndef DG_HOLDPOINT_H
#define DG_HOLDPOINT_H

#include "core/base.h"
#include "core/dgmath.h"
#include "anim/anim.h"
#include "player/sway.h"
#include "gameplay/aim.h"

struct cJSON;

typedef enum HoldState {
    HP_HIP, HP_ADS, HP_SPRINT, HP_CROUCH, HP_PRONE, HP_EMPTY, HP_INSPECT, HP_VEHICLE_ONEHAND,
    HP_PHONE_PORTRAIT, HP_PHONE_LANDSCAPE,   // phone camera mode (docs/PHONE.md): pos / rot place the screen socket
    HP_STATE_COUNT
} HoldState;

const char *holdpoint_state_name(int s);
int holdpoint_state_from_name(const char *s);

// One state target: position in camera space as (right, up, forward) meters, rotation as
// (pitch, yaw, roll) degrees (pitch + = muzzle up, yaw + = muzzle left, roll + = cant left).
typedef struct HpPose { vec3 pos, rot; bool set; } HpPose;

typedef struct HoldPointStyle {
    char name[16];
    HpPose state[HP_STATE_COUNT];
    float blend_in[HP_STATE_COUNT];   // blend time into each state (s)
    float eye_relief;                 // ADS: sight distance in front of the eye (m)
    vec3 ads_rot;                     // ADS extra rotation (deg), normally 0
    // procedural layer. Sway (breathing, look lag, bob, landing) is NOT per style: one global
    // profile for every item (src/player/sway.c). kick_scale scales the fire animation's gun kick.
    float ads_scale;
    float kick_scale;
    // weapon collision
    float col_back, col_up, col_pitch, col_curve, col_in_speed, col_out_speed, col_radius;
    float clavicle_gain;              // clavicle additive (deg per m of hold offset from the hip pose)
    float pitch_soft, pitch_follow;   // beyond pitch_soft (deg) the hold point follows the view pitch by pitch_follow
    float ads_pitch_soft, ads_pitch_follow;   // the same in ADS (sight exactly on the view axis below ads_pitch_soft)
    float twist;                      // torso blading (deg, + = left shoulder forward), neck counter-rotated
    vec3 clav_l, clav_r;              // clavicle protraction (deg pitch, yaw, roll) while holding
    float reach_pull;                 // max hold point pull towards the shoulders when out of reach (m)
} HoldPointStyle;

// Per item override (weapon / item JSON "holdpoint" block): only the set fields apply.
typedef struct HoldPointOverride {
    bool any;
    HpPose state[HP_STATE_COUNT];
    float eye_relief;                 // 0 = style
    float eye_relief_add;             // added to the resolved eye relief (attachments: keeps the stock weld)
    bool has_pivot;
    vec3 pivot;                       // item space
} HoldPointOverride;

typedef struct HoldPointState {
    bool init;
    int state, style_key;
    float blend_t, blend_dur;
    vec3 off_pos;                     // inertialization offset at the last switch (camera space)
    quat off_rot;
    // Velocity-preserving inertialization (quintic per component: position + rotation vector)
    float ix0[6], iv0[6], iT[6];
    vec3 base_vel, base_avel;         // last tick's hold point velocity (m/s, rad/s rotation vector)
    Xform base;                       // last blended state transform (hold point, camera space)
    SwayState sway;                   // global sway (look lag, breathing, bob, landing, hold breath)
    float pull;                       // collision pull 0..1 (smoothed)
    float pitch_w;                    // soft pitch limit weight (0 in ADS)
    u8 ads_from;                      // the state the blend came from (ADS anchor blend out of ADS)
} HoldPointState;

typedef bool (*HoldPointRayFn)(void *ctx, vec3 origin, vec3 dir_len, float *fraction, vec3 *point, vec3 *normal);

// How the procedural layer moves the item (docs/RECOIL.md "Hip vs ADS"). Anchors: 0 the pivot
// socket, 1 the hold point, 2 the sight (reticle centre). In ADS the kick (and look lag / sway)
// turns the gun about the sight, translation only along the view axis (forward push) plus roll,
// so the reticle centre never leaves the view axis. ads_misalign_deg > 0 (iron sights) clamps the
// rotation so the rear sight doesn't cover the front post.
typedef struct HoldKickParams {
    bool set;
    float ads_push, ads_roll, ads_misalign_deg;
    u8 kick_hip, kick_ads, sway_hip, sway_ads;
    float ads_in, ads_out;   // blend times into / out of ADS (s, > 0: the weapon's ADS time, so the gun and the zoom finish together)
    // Global weapon sway inputs (docs/FIRE_ANIM.md "Global sway"): the optic's steadiness, the
    // hold-breath factor (1 normal, the profile's hold_breath_mult while held), stance (crouch /
    // prone < 1), an extra wobble 0..1 (low health, aiming right after sprinting) and the global
    // wpn_sway_ads_scale. Never anything weapon-specific.
    bool sway_set;
    float optic_sway, breath_k, stance, wobble, ads_sway_mult;
} HoldKickParams;

typedef struct HoldPointInput {
    const HoldPointStyle *style;
    const HoldPointOverride *ovr;
    int style_key;                    // changes -> inertialized blend (style / item switch)
    // item sockets (item space; anchor = grip_r position unless the item names another). The
    // anchor is used as a point: the hold point's rotation is the item's orientation.
    Xform anchor;
    vec3 sight, pivot;
    float length;
    bool has_item;
    // requested state flags (priority: empty > inspect > sprint > ads > prone > crouch > hip)
    bool ads, sprint, crouch, prone, empty, inspect;
    bool vehicle_onehand;             // driving: the gun held out one-handed (aimed along the sight line)
    u8 phone_mode;                    // phone camera mode: 1 portrait, 2 landscape (HP_PHONE_*), 0 = off
    // Driven rotation (a phone held by a paired real phone): the item's camera-space rotation
    // becomes rot_cam (blended by rot_w), turned about the sight socket (the screen) so it stays
    // in front of the eye. Presentation: applied after the procedural layer.
    float rot_w;
    quat rot_cam;
    // Hold aim (docs/AIM.md "Hold aim", guns only): the gameplay pose turns about its pivot (hip: the
    // pivot socket, ADS: the sight) so the bore line (muzzle socket along the barrel axis, item
    // space) passes through aim_p (camera space: usually (0, 0, -D)), at most aim_max_hip /
    // aim_max_ads (rad), blended with the states (none in sprint / empty / inspect / phone) and faded
    // out by the wall pull.
    bool aim_valid;
    vec3 aim_p;
    vec3 muzzle, barrel;              // item space; barrel 0 = -Z
    float aim_max_hip, aim_max_ads;
    float dt;
    // Deterministic phases (docs/SPECTATE.md): breathing from the player's command clock (s) and
    // the movement bob from PlayerMove.stride, so the owner and its spectators sway in step. Unset
    // (phase_valid false): free-running from creation (NPCs, editor).
    bool phase_valid;
    double clock;
    float stride;
    float aim_yaw, aim_pitch;         // view angles (radians): look lag from their rate
    vec3 vel_cam;                     // velocity in camera space (m/s)
    bool grounded;
    vec3 kick_pos, kick_rot;          // recoil item spring: camera space m (+z back) / deg (pitch up, yaw right, roll)
    Xform cam_world;                  // for the collision cast
    HoldPointRayFn raycast;
    void *ray_ctx;
    bool static_pose;                 // no procedural layer (alignment tests, editor)
    float sway_scale;                 // look lag, idle sway, breathing, sprint bob (wpn_sway_scale; 0 = none)
    float bob_scale;                  // movement bob outside sprint (wpn_bob_scale; 0 = the item stays still)
    HoldKickParams kick_params;       // anchors / ADS kick shaping (unset = sight anchor, push 1, roll 1)
    const SwayProfile *sway;          // NULL = the global profile (sway_global())
} HoldPointInput;

typedef struct HoldPointOutput {
    Xform hold;                       // hold point (camera space): where the anchor socket goes
    Xform item;                       // item transform (camera space): the drawn pose (aim_item + presentation)
    // Gameplay pose (docs/AIM.md): before the hold aim (state, blend, pitch limit, wall pull) and
    // after it (what the bullet follows). aim_raw is what aim_hold_solve ran on (camera space).
    Xform gp_item, aim_item;
    AimHoldRaw aim_raw;
    quat aim_hold;                    // the hold aim's turn about aim_pivot
    vec3 aim_pivot;
    float aim_w, aim_max;             // weight (state x pull fade), limit (rad)
    float aim_angle_deg, aim_miss;    // the turn, and P's distance from the aimed barrel line (m)
    bool aim_clamped;
    vec3 pivot;                       // rotation pivot (camera space)
    vec3 offset;                      // hold point offset from the style's hip pose (camera space)
    Xform ads_item;                   // the exact ADS item transform (tests / debug)
    int state;
    float blend;                      // 0..1 progress of the current blend
    float pull;                       // collision pull 0..1
    vec3 cast_from, cast_to;          // world, last collision cast (debug)
    bool cast_hit;
    vec3 sway_hip_rot, sway_hip_pos, sway_ads_rot;   // this tick's global sway (sway_check)
} HoldPointOutput;

void holdpoint_style_defaults(HoldPointStyle *s, const char *style_name);
void holdpoint_style_parse(HoldPointStyle *s, const struct cJSON *o);
void holdpoint_override_parse(HoldPointOverride *o, const struct cJSON *j);
char *holdpoint_style_to_json(const HoldPointStyle *s);   // caller frees
void holdpoint_reset(HoldPointState *st);
// Resolves the requested state from the flags (documented priority).
int holdpoint_pick_state(const HoldPointInput *in);
// Exact ADS item transform: sight socket on the view axis at the eye relief, barrel along -Z.
// ADS sight distance multiplier for weld-based sights (cvar ads_eye_relief_scale, game_sights.c).
void holdpoint_set_ads_relief_scale(float k);
void holdpoint_set_look_lag_scale(float k);   // wpn_look_lag (game_sights.c)
float holdpoint_ads_relief(float relief);   // the eye relief actually used in ADS (scaled, floored)
Xform holdpoint_ads_item(const HoldPointStyle *s, const HoldPointOverride *ovr, vec3 sight);
void holdpoint_eval(HoldPointState *st, const HoldPointInput *in, HoldPointOutput *out);

DG_INLINE quat hp_euler_deg(vec3 e) { return quat_euler(e.y * DG_DEG2RAD, e.x * DG_DEG2RAD, e.z * DG_DEG2RAD); }
DG_INLINE vec3 hp_cam_pos(vec3 ruf) { return v3(ruf.x, ruf.y, -ruf.z); }

#endif
