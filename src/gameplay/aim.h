// Aim model (docs/AIM.md): the gun points at the crosshair, the bullet follows the barrel, and the
// crosshair moves only when the barrel line is obstructed.
//   P  the camera ray's aim point (first hit, or max_range): what the centred crosshair is on
//   hold aim: the gameplay hold pose turns the gun (at most wpn_hold_aim_max, ADS 3 deg) about its
//      pivot so its barrel line passes through P. It is a pose of the gun, never a correction of the
//      bullet; sway / look lag / bob / the fire kick only move the drawn gun on top of it.
//   M  muzzle: the muzzle socket of that aimed gameplay pose (replicated as a camera-space claim in
//      the UserCmd, validated by the host against its own copy of the pose re-aimed at P)
//   B  barrel forward: the held item's -Z axis in the same pose (camera-space claim, validated too);
//      the bullet leaves M along B (+ the spread / recoil cone and the ballistic zero), nothing turns it
//   F  obstruct_far: distance of the camera ray's first hit, or max_range
//   Q  first hit of M + B * t (t <= max_range); obstructed when it is clearly closer than F or when
//      the line does not end at P (the hold could not reach it: wall pull, beyond the limit)
// This header is pure (no world, no physics): the traces live in gameplay/weapons.c.
#ifndef DG_AIM_H
#define DG_AIM_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum AimClass {
    AIM_CLEAR,        // the barrel line ends at P: the crosshair is exactly the screen centre
    AIM_OBSTRUCTED,   // the barrel line hits something clearly closer than the camera ray does, or misses P
    AIM_BARREL,       // a hit within obstruct_near of the muzzle (barrel against a wall), or the muzzle in geometry
    AIM_FRIENDLY,     // a teammate on the barrel line (any distance)
} AimClass;

// Hold aim (docs/AIM.md "Hold aim").
#define AIM_HOLD_ADS_MAX_DEG 3.0f   // ADS: the gun turns about the sight by at most this
#define AIM_HOLD_ITERS 3            // fixed-point iterations (ratio ~|bore offset| / |P - pivot|: sub-um)
#define AIM_HOLD_MIN_AHEAD 0.05f    // P not this far ahead of the muzzle along the barrel: no turn ...
#define AIM_HOLD_FADE 0.20f         // ... fading in over this much more (continuous: the tick pose and a frame's re-solve agree)
#define AIM_REACH_DEG 0.1f          // the barrel line "ends at P" within far x tan(this) ...
#define AIM_REACH_MIN 0.005f        // ... or this (m), whichever is larger

const char *aim_class_name(int c);

// Camera space: x right, y up, -z forward (the view rotation of dir_from_angles).
DG_INLINE quat aim_view_rot(float yaw, float pitch) { return quat_euler(yaw, pitch, 0.0f); }
DG_INLINE vec3 aim_cam_to_world(vec3 eye, float yaw, float pitch, vec3 cam) { return v3_add(eye, quat_rotate(aim_view_rot(yaw, pitch), cam)); }
DG_INLINE vec3 aim_world_to_cam(vec3 eye, float yaw, float pitch, vec3 world) { return quat_rotate(quat_conj(aim_view_rot(yaw, pitch)), v3_sub(world, eye)); }

// UserCmd muzzle claim: camera-space millimetres (i16 each, +-32 m). All zero = no claim.
#define AIM_CLAIM_MM 1000.0f
void aim_claim_pack(vec3 cam, i16 out[3]);
vec3 aim_claim_unpack(const i16 in[3]);
bool aim_claim_valid(const i16 in[3]);

// Host check of a client's claimed muzzle (camera space) against its own reconstruction.
typedef struct AimValidate {
    vec3 cam;          // the muzzle to fire from (camera space)
    float error;       // |claim - host| (m), 0 without a host pose
    bool clamped;      // the claim was out of tolerance (or out of reach) and was moved
} AimValidate;
// tolerance: max claim distance from the host's muzzle; reach: max |muzzle| from the eye (a hard
// cap whatever the host has). have_host false: only the reach cap applies.
AimValidate aim_validate_muzzle(vec3 claim, bool have_claim, vec3 host, bool have_host, float tolerance, float reach);

// UserCmd barrel claim: the barrel's forward axis in camera space, unit vector x 32767 (i16 each).
// All zero = no claim (aim_claim_valid).
void aim_dir_pack(vec3 cam_dir, i16 out[3]);
vec3 aim_dir_unpack(const i16 in[3]);

// Host check of a claimed barrel direction against its own pose's: within tol_deg it is used as is,
// beyond it is turned back onto the tolerance cone around the host's. No claim: the host's.
typedef struct AimValidateDir {
    vec3 dir;
    float error_deg;   // angle claim <-> host (0 without a host pose)
    bool clamped;
} AimValidateDir;
AimValidateDir aim_validate_dir(vec3 claim, bool have_claim, vec3 host, bool have_host, float tol_deg);

// A direction turned by a change of the view angles (yaw / pitch, radians) about the view it is
// relative to: the recoil already applied within this tick (multiple shots per tick) moves the barrel
// the way it moves the camera.
vec3 aim_view_delta(vec3 dir, float yaw, float pitch, float dyaw, float dpitch);

// Thrown items only (they leave the palm, which has no barrel): the direction from the hand to the
// camera ray's aim point; falls back to the camera direction when P is (nearly) at the hand.
vec3 aim_converge(vec3 muzzle, vec3 p, vec3 cam_dir);

// The hold aim. raw: the gameplay pose BEFORE the hold aim (camera space): the muzzle, the barrel axis
// (unit), the pivot the gun turns about (the hip pivot, the sight in ADS), the weight (state x wall
// pull fade, 0..1) and the limit (rad). aim_hold_solve turns the gun about the pivot so its barrel
// line passes through p (camera space), at most max_rad, scaled by w; P less than AIM_HOLD_MIN_AHEAD
// ahead of the muzzle: no turn, faded in over the next AIM_HOLD_FADE (past the limit there anyway).
// Deterministic (DG_DETMATH): the owner, the host and every remote copy
// re-aim the same raw at the same P to the same bits.
typedef struct AimHoldRaw { vec3 muzzle, barrel, pivot; float w, max_rad; bool valid; } AimHoldRaw;
typedef struct AimHold {
    quat rot;          // the turn (about raw->pivot), identity when none
    vec3 muzzle, barrel;
    float angle_deg;   // angle of the turn
    float miss;        // distance of p from the aimed barrel ray (0 = the line ends at P)
    bool clamped;      // the limit was reached (miss > 0)
} AimHold;
AimHold aim_hold_solve(const AimHoldRaw *raw, vec3 p);
// Closest distance of p to the ray origin + dir * t, t >= 0 (dir unit).
float aim_line_miss(vec3 origin, vec3 dir, vec3 p);
// How close the barrel line must pass P to count as ending there: max(AIM_REACH_MIN, far x tan(AIM_REACH_DEG)).
float aim_reach_tol(float far);

// The obstruction rule (docs/AIM.md "Obstruction"):
//   far   = the camera ray's first hit distance, or max_range without one (cam_hit < 0)
//   depth = the barrel hit's distance from the muzzle + the muzzle's depth ahead of the camera, so
//           both distances are measured from the camera
//   miss  = P's distance from the barrel ray (aim_line_miss): 0 when the hold aim reached P
// In this order:
//   friendly   the first barrel hit is a teammate (any distance)
//   barrel     the muzzle was inside geometry (recovered), or a hit within obstruct_near of the muzzle
//   obstructed hit >= 0 and hit + depth < far - margin, margin = max(margin_abs, margin_rel * far,
//              parallax): a line landing on the surface the camera sees, at its grazing angle, is not
//   obstructed miss > aim_reach_tol(far): the barrel line does not end at P (the gun was moved: the
//              wall pull, past the hold aim's limit, the soft pitch limit, the ADS 3 deg cap)
//   clear      otherwise (no hit and miss ~0: the sky)
// parallax: how much sooner a line beside the camera ray reaches the surface the camera sees, at its
// grazing angle (the ground ahead, far terrain, a wall seen edge on): aim_parallax(miss, ...).
typedef struct AimObstructRule {
    float near;          // obstruct_near (m from the muzzle)
    float margin_abs;    // obstruct_margin (m)
    float margin_rel;    // obstruct_margin_rel (fraction of far)
} AimObstructRule;
float aim_obstruct_far(float cam_hit, float max_range);
float aim_obstruct_margin(const AimObstructRule *r, float far, float parallax);
// separation: the barrel line's distance from P (m); sin_graze: |dot(camera dir, surface normal)| at
// the camera's hit (1 = head on). Capped at max(3 m, 25 % of far) (sin_graze floored at 0.005).
float aim_parallax(float separation, float sin_graze, float far);
typedef struct AimClassIn {
    float hit;           // the barrel line's first hit from the muzzle (m), < 0 = none
    float depth;         // the muzzle's depth ahead of the camera along the camera ray (m)
    float far;           // aim_obstruct_far
    float parallax;      // aim_parallax(miss, sin_graze, far), 0 without a camera hit
    float miss;          // aim_line_miss(origin, barrel, P)
    bool friendly;       // the first barrel hit is a teammate
    bool recovered;      // the muzzle was inside geometry (origin pulled back)
} AimClassIn;
AimClass aim_classify(const AimClassIn *in, const AimObstructRule *r);

// Muzzle inside geometry: a cast from the eye (inside the player's capsule, always free) to the
// muzzle hit at fraction f of the way: the origin is pulled back to the last free point, margin m
// short of the surface. f >= 1 (no hit): the muzzle itself.
vec3 aim_recover_origin(vec3 eye, vec3 muzzle, float f, float margin);

// Crosshair: frame-rate independent exponential approach (tau seconds to ~63 %).
DG_INLINE float aim_lerp_k(float dt, float tau) { return tau <= 1e-4f ? 1.0f : 1.0f - DGM_EXPF(-dt / tau); }
// The spread ring around the impact point: the cone's radius at distance dist from the muzzle,
// seen from the camera at distance cam_dist (px for a vertical FOV fov_v, screen height h).
float aim_spread_px(float spread_deg, float dist, float cam_dist, float fov_v_rad, float screen_h);

#endif
