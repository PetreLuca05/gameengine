// Aim model (docs/AIM.md): bullets leave the weapon's muzzle socket along the barrel's forward axis,
// with no correction towards the camera's aim point. The crosshair stays at the screen centre unless
// the barrel line is obstructed: then it moves to where that line ends.
//   M  muzzle: the shooter's held-weapon muzzle socket (replicated as a camera-space claim in the
//      UserCmd, validated by the host against its own copy of the pose)
//   B  barrel forward: the held item's -Z axis in the same pose (camera-space claim, validated too);
//      the spread / recoil cones are applied around it
//   F  obstruct_far: distance of the camera ray's first hit, or max_range
//   Q  first hit of M + B * t (t <= max_range); obstructed when it is clearly closer than F
// This header is pure (no world, no physics): the traces live in gameplay/weapons.c.
#ifndef DG_AIM_H
#define DG_AIM_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum AimClass {
    AIM_CLEAR,        // the barrel line ends at (or past) what the camera sees: nothing to show
    AIM_OBSTRUCTED,   // the barrel line hits something clearly closer than the camera ray does
    AIM_BARREL,       // a hit within obstruct_near of the muzzle (barrel against a wall)
    AIM_FRIENDLY,     // a teammate on the barrel line (any distance)
} AimClass;

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

// The obstruction rule (docs/AIM.md "Obstruction"):
//   far   = the camera ray's first hit distance, or max_range without one (cam_hit < 0)
//   depth = the barrel hit's distance from the muzzle + the muzzle's depth ahead of the camera, so
//           both distances are measured from the camera
//   barrel     hit within obstruct_near of the muzzle (whatever the camera sees)
//   friendly   the hit is a teammate (any distance)
//   obstructed depth < far - margin, margin = max(margin_abs, margin_rel * far, parallax): a barrel
//              ray landing beside the aim point at range is parallax, not an obstruction
//   clear      otherwise, and without a hit (hit < 0)
// parallax: how much sooner a line offset by the muzzle's distance from the camera ray reaches the
// surface the camera sees, at its grazing angle (the ground ahead, a wall seen edge on).
typedef struct AimObstructRule {
    float near;          // obstruct_near (m from the muzzle)
    float margin_abs;    // obstruct_margin (m)
    float margin_rel;    // obstruct_margin_rel (fraction of far)
} AimObstructRule;
float aim_obstruct_far(float cam_hit, float max_range);
float aim_obstruct_margin(const AimObstructRule *r, float far, float parallax);
// offset: the muzzle's distance from the camera ray (m); sin_graze: |dot(camera dir, surface normal)|
// at the camera's hit (1 = head on). Capped at 3 m (sin_graze floored at 0.05).
float aim_parallax(float offset, float sin_graze);
AimClass aim_classify(float hit, float muzzle_depth, float far, float parallax, const AimObstructRule *r, bool friendly);

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
