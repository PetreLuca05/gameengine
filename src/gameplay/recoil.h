// Procedural recoil (three layers) + bullet spread. Pure module: no engine state, so it is
// unit-testable (tests/test_recoil.c) and deterministic for client prediction.
//
//   1. Aim recoil      RecoilState (inside the replicated PlayerSim). Rotates the real aim.
//                      Deterministic from (params, mag_seed, shot_index); recovery returns
//                      recovery_fraction of what the player did NOT counter.
//   2. Camera kick     RecoilVisual.cam_*  spring offset on the camera only (aim unaffected).
//   3. Viewmodel kick  RecoilVisual.vm_*   spring offset on the weapon grip frame.
//   Spread            recoil_spread(): full cone angle that bullets are drawn from and that the
//                      crosshair visualizes exactly.
//
// Conventions: degrees everywhere; pitch + = up; yaw + = RIGHT (the game's yaw is +left).
#ifndef DG_RECOIL_H
#define DG_RECOIL_H

#include "core/base.h"
#include "core/dgmath.h"
#include "core/spring.h"

#define RECOIL_MAX_PATTERN 32

enum FireModeBits { FIRE_SINGLE = 1, FIRE_BURST = 2, FIRE_AUTO = 4 };
typedef enum FireMode { FM_SINGLE = 0, FM_BURST = 1, FM_AUTO = 2 } FireMode;

typedef struct RecoilParams {
    // aim recoil
    float v_first, v_per, v_min, v_max;
    float h_per, h_min, h_max, h_bias;
    vec2 pattern[RECOIL_MAX_PATTERN];   // x = horizontal, y = vertical multipliers per shot
    int pattern_count;
    float pattern_randomness;
    int ramp_shots;
    float recovery_delay, recovery_speed, recovery_fraction;
    bool recovery_stop_on_input;
    // camera kick (spring)
    float cam_pitch, cam_yaw, cam_roll, cam_back;
    SpringParams cam_spring;
    // viewmodel kick (spring)
    float vm_back, vm_up, vm_pitch, vm_yaw, vm_roll, vm_random;
    SpringParams vm_spring;
    float vm_ads_scale;                 // viewmodel kick scale while aiming
    // spread (full cone, degrees)
    float base_hip, base_ads, per_shot_bloom, bloom_max, bloom_recovery;
    float s_move, s_crouch, s_prone, s_air;
    // aim recoil modifiers
    float ads_mult, crouch_mult, prone_mult, moving_mult, air_mult;
    float fm_single, fm_burst, fm_auto;
    // feel
    float rpm;                          // automatic / max semi rate
    float burst_rpm;
    int burst_count;
    u8 fire_modes;                      // FireModeBits
    bool seed_per_mag;
    float sprint_to_fire;               // s after sprint before firing is allowed
    float after_sprint_kick;            // aim kick multiplier for the first shot after sprinting
    float ads_time;                     // s to fully aim
    float idle_sway, scope_sway;        // degrees amplitude
    float hold_breath_time;             // s of breath hold capacity
    // Hip-fire vs ADS (blended by the aim progress; docs/RECOIL.md "Hip vs ADS"). Hip: the camera
    // climbs less (aim_mult) and the cone governs accuracy (spread / bloom multipliers). ADS:
    // the aim recoil as configured (x ads.aim_mult), the gun kicks about the reticle centre.
    float hip_aim_mult, hip_kick_mult, hip_spread_mult, hip_bloom_mult, hip_bloom_max_mult, hip_bloom_recovery_mult;
    float ads_aim_mult, ads_forward_push, ads_roll, ads_max_misalign_deg;
    u8 anchor_hip, anchor_ads;          // RecoilAnchor: what the item kick turns about
    u8 sway_anchor_hip, sway_anchor_ads;
    // Weapon sway of the held item (cosmetic, docs/RECOIL.md "Weapon sway"): per-component
    // multipliers (breathing, look lag, movement bob, landing) at the hip and in ADS; in ADS the
    // sway turns the gun about the reticle centre so the sight never leaves the view axis.
    float sway_hip[4], sway_ads[4];
    float hold_breath_mult;             // ADS sway while holding breath (x)
} RecoilParams;

typedef enum RecoilAnchor { RA_PIVOT, RA_HOLD_POINT, RA_SIGHT } RecoilAnchor;
typedef enum RecoilType { RT_RIFLE, RT_PISTOL, RT_SMG, RT_LMG, RT_SHOTGUN, RT_SNIPER } RecoilType;
// Hip / ADS defaults for a weapon type (before the JSON, which overrides them).
void recoil_type_defaults(RecoilParams *p, RecoilType t);
// 0..1 hip -> ADS blends of the multipliers above.
float recoil_bloom_mult(const RecoilParams *p, float ads_t);
float recoil_bloom_max(const RecoilParams *p, float ads_t);
float recoil_kick_mult(const RecoilParams *p, float ads_t);   // item (viewmodel) kick

typedef struct RecoilState {
    u32 mag_seed;
    u16 shot_index;                     // shots in the current pattern run
    u8 recovering;
    u8 have_prev;
    float acc_pitch, acc_yaw;           // recoil added by the system, relative to the original aim
    float countered_pitch, countered_yaw; // part of acc the player already pulled back (magnitudes)
    float pool_pitch, pool_yaw;         // what recovery still has to return (signed, along acc)
    float pool_start;                   // magnitude when recovery started (rate normalisation)
    float since_shot;
    float bloom;                        // extra cone degrees from firing / flinch
    float prev_cmd_pitch, prev_cmd_yaw; // last command view (deg, yaw + = right)
    float emitted_pitch, emitted_yaw;   // what the system applied to the view last tick
    float ads_t;                        // aim progress this tick (bloom recovery hip / ADS), set by the caller
} RecoilState;

typedef struct RecoilMods {
    float ads_t;        // 0 hip .. 1 fully aimed
    float move01;       // horizontal speed / reference speed, 0..1
    bool crouched, prone, airborne;
    FireMode fire_mode;
    float extra;        // e.g. first-shot-after-sprint multiplier (1 = none)
    float scale;        // global wpn_recoil_scale / wpn_spread_scale
} RecoilMods;

typedef struct RecoilKick { float pitch, yaw; } RecoilKick;

void recoil_params_default(RecoilParams *p);
void recoil_reset(RecoilState *s, u32 seed);           // new magazine / reload: pattern restarts
u32 recoil_mag_seed(u32 a, u32 b);                      // deterministic seed from shared values

float recoil_aim_mult(const RecoilParams *p, const RecoilMods *m);
// Raw (unmodified) pattern step for shot index i: pure function of (params, seed, i).
RecoilKick recoil_pattern_step(const RecoilParams *p, u32 seed, int i);
// One shot: returns the aim delta to add to the view, updates accumulation + bloom.
RecoilKick recoil_shot(const RecoilParams *p, RecoilState *s, const RecoilMods *m);
// Call at the start of every tick with the command's view angles: derives how much of the
// player's input countered the recoil (deterministic on host and client).
void recoil_observe_input(const RecoilParams *p, RecoilState *s, float cmd_pitch_deg, float cmd_yaw_right_deg);
// Per tick: bloom decay and recovery. Returns the recovery delta to add to the view.
RecoilKick recoil_tick(const RecoilParams *p, RecoilState *s, float dt);
// Record what was applied to the view this tick (shots + recovery) for the next observe call.
DG_INLINE void recoil_set_emitted(RecoilState *s, RecoilKick k) { s->emitted_pitch = k.pitch; s->emitted_yaw = k.yaw; }
// Current bullet cone (full angle, degrees). The crosshair draws exactly this.
float recoil_spread(const RecoilParams *p, const RecoilState *s, const RecoilMods *m);
// Bullet direction inside a cone of full angle cone_deg (truncated gaussian, never outside).
vec3 recoil_cone_dir(vec3 dir, float cone_deg, Rng *rng);
// Fire timing with sub-tick accuracy: subtracts dt from *cooldown and returns how many shots
// fit this tick (each adds `interval`). Exact RPM independent of the tick rate.
int recoil_fire_budget(float *cooldown, float interval, float dt, bool trigger, int max_shots);
// Cumulative (x = yaw right, y = pitch) path of n shots without recovery (debug preview).
int recoil_preview(const RecoilParams *p, u32 seed, vec2 *out, int n);

// Client-only visual layers.
typedef struct RecoilVisual {
    SpringVec3 cam_rot;     // pitch, yaw(right), roll in degrees
    SpringF32 cam_back;     // meters
    SpringVec3 vm_pos;      // x right, y up, z back (meters, camera space)
    SpringVec3 vm_rot;      // pitch, yaw(right), roll in degrees
} RecoilVisual;

void recoil_visual_kick(const RecoilParams *p, RecoilVisual *v, float scale, float ads_t, u32 seed);
void recoil_visual_step(const RecoilParams *p, RecoilVisual *v, float dt);

#endif
