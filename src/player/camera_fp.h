// True first-person camera on the head bone.
//
// Rotation is always the player's view angles (mouse input), never the bone's. Position:
//   head eye   = the animated Head bone x eye socket (after the spine look solved the view)
//   camera     = head eye (+ cam_bob_scale x the old stabilizer offset, default 0)
// The stabilizer (ideal capsule eye + clamped spring towards the head) is kept for tuning only:
// on its own it left the camera behind the head whenever the animation leaned forward. A sphere
// cast keeps the eye out of walls and ledges.
// Solve order (no feedback loop): view angles -> spine / head solve (tick + per-frame
// correction) -> read the head eye -> stabilizer -> camera -> hold point / arms per frame.
#ifndef DG_CAMERA_FP_H
#define DG_CAMERA_FP_H

#include "core/dgmath.h"

typedef struct CamHeadStab {
    bool init;
    vec3 disp, vel;            // stabilized displacement head - ideal (camera yaw space)
    vec3 last_raw;
    float max_seen;            // largest |disp| since the last reset (tests / cam_debug)
    // death camera
    bool dead;
    vec3 death_pos, death_vel;
    float death_t;
    // collision
    float col_push;
} CamHeadStab;

typedef struct CamFpCvars {
    struct Cvar *stiffness, *damping, *follow, *follow_lateral, *clamp, *bob_scale, *recon_smooth, *third, *debug;
    struct Cvar *radius, *death_follow, *death_spectate, *weapon_fov, *kick_scale, *kick_rot;
} CamFpCvars;

// Pure stabilizer parameters (camera_fp_math.c, unit tested); camera_fp_stabilize fills them
// from the cam_head_* cvars.
typedef struct CamStabParams { float stiffness, damping, follow, follow_lateral, clamp, bob_scale; } CamStabParams;
CamStabParams cam_stab_params_default(void);
vec3 camera_fp_stabilize_ex(CamHeadStab *s, vec3 ideal, vec3 head_eye, float yaw, float dt, const CamStabParams *p);

void camera_fp_register_cvars(void);
const CamFpCvars *camera_fp_cvars(void);
void camera_fp_reset(CamHeadStab *s);
// Stabilized eye from the ideal and the animated head eye (world), camera yaw for the spring frame.
vec3 camera_fp_stabilize(CamHeadStab *s, vec3 ideal, vec3 head_eye, float yaw, float dt);
// Damped follow of a (ragdoll) head; after cam_death_spectate seconds the camera lifts away.
vec3 camera_fp_death(CamHeadStab *s, vec3 head, float dt);
typedef bool (*CamSphereCastFn)(void *ctx, vec3 from, vec3 delta, float radius, float *fraction);
// Keeps the eye at least `radius` from geometry along the line from a safe point (capsule center).
vec3 camera_fp_collide(CamHeadStab *s, vec3 safe, vec3 eye, CamSphereCastFn cast, void *ctx, float dt);

#endif
