// Where the shots start (docs/AIM.md): the first-person camera, along the view. The bullets fly from
// the camera the player looks through (the head's "camera" socket), so they land where the crosshair
// says at any distance; the drawn gun only shows the flash, the tracer's start and the casing.
//   eye claim: the camera relative to the simulated eye (player_eye_pos), in camera space, carried
//      by the UserCmd (eye_cam, mm) and checked by the host against its own copy of the pose.
// This header is pure (no world, no physics): the trace lives in gameplay/weapons.c.
#ifndef DG_AIM_H
#define DG_AIM_H

#include "core/base.h"
#include "core/dgmath.h"

// Camera space: x right, y up, -z forward (the view rotation of dir_from_angles).
DG_INLINE quat aim_view_rot(float yaw, float pitch) { return quat_euler(yaw, pitch, 0.0f); }
DG_INLINE vec3 aim_cam_to_world(vec3 eye, float yaw, float pitch, vec3 cam) { return v3_add(eye, quat_rotate(aim_view_rot(yaw, pitch), cam)); }
DG_INLINE vec3 aim_world_to_cam(vec3 eye, float yaw, float pitch, vec3 world) { return quat_rotate(quat_conj(aim_view_rot(yaw, pitch)), v3_sub(world, eye)); }

// UserCmd camera claim: camera-space millimetres (i16 each, +-32 m). All zero = no claim.
#define AIM_CLAIM_MM 1000.0f
void aim_claim_pack(vec3 cam, i16 out[3]);
vec3 aim_claim_unpack(const i16 in[3]);
bool aim_claim_valid(const i16 in[3]);

// Host check of a client's claimed point (camera space) against its own reconstruction.
typedef struct AimValidate {
    vec3 cam;          // the point to use (camera space)
    float error;       // |claim - host| (m), 0 without a host pose
    bool clamped;      // the claim was out of tolerance (or out of reach) and was moved
} AimValidate;
// tolerance: max claim distance from the host's point; reach: max |point| from the eye (a hard cap
// whatever the host has). have_host false: only the reach cap applies.
AimValidate aim_validate_claim(vec3 claim, bool have_claim, vec3 host, bool have_host, float tolerance, float reach);

// The camera inside geometry: a cast from the eye (inside the player's capsule, always free) to the
// camera hit at fraction f of the way: pulled back to the last free point, margin m short of the
// surface. f >= 1 (no hit): the point itself.
vec3 aim_recover_origin(vec3 eye, vec3 point, float f, float margin);

#endif
