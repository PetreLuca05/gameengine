// Aim model (docs/AIM.md): bullets leave the weapon's muzzle socket and converge on the point the
// camera ray hits, so the reticle stays true at any distance while the shot is physically fired
// from the barrel. When something sits between the muzzle and that aim point the shot lands on it
// instead, and the crosshair moves there.
//   P  aim point: first hit of the camera ray (eye + view dir), or eye + dir * max_range
//   M  muzzle: the shooter's held-weapon muzzle socket (replicated as a camera-space claim in the
//      UserCmd, validated by the host against its own copy of the pose)
//   d  bullet direction: normalize(P - M), spread / recoil cones around it
//   Q  obstruction: first hit of M -> P before P (barrel blocked within obstruct_near, else partial)
// This header is pure (no world, no physics): the traces live in gameplay/weapons.c.
#ifndef DG_AIM_H
#define DG_AIM_H

#include "core/base.h"
#include "core/dgmath.h"

typedef enum AimClass {
    AIM_CLEAR,        // muzzle -> P is free
    AIM_PARTIAL,      // something farther along the line (corner, window frame, bodywork)
    AIM_BARREL,       // within obstruct_near of the muzzle (barrel against a wall)
    AIM_FRIENDLY,     // a teammate in the line of fire (any distance)
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

// Bullet direction from the muzzle to the aim point; falls back to the camera direction when P is
// (nearly) at the muzzle.
vec3 aim_converge(vec3 muzzle, vec3 p, vec3 cam_dir);

// Obstruction classification of the muzzle -> P segment: hit at t (m along it, < 0 = none), segment
// length len; friendly: what was hit is a teammate.
AimClass aim_classify(float t, float len, float obstruct_near, bool friendly);

// Muzzle inside geometry: a cast from the eye (inside the player's capsule, always free) to the
// muzzle hit at fraction f of the way: the origin is pulled back to the last free point, margin m
// short of the surface. f >= 1 (no hit): the muzzle itself.
vec3 aim_recover_origin(vec3 eye, vec3 muzzle, float f, float margin);

// Crosshair: frame-rate independent exponential approach (tau seconds to ~63 %).
DG_INLINE float aim_lerp_k(float dt, float tau) { return tau <= 1e-4f ? 1.0f : 1.0f - expf(-dt / tau); }
// The spread ring around the impact point: the cone's radius at distance dist from the muzzle,
// seen from the camera at distance cam_dist (px for a vertical FOV fov_v, screen height h).
float aim_spread_px(float spread_deg, float dist, float cam_dist, float fov_v_rad, float screen_h);

#endif
