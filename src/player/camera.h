// First-person camera: client-owned view angles, stair smoothing, head bob, FOV changes.
#ifndef DG_CAMERA_H
#define DG_CAMERA_H

#include "core/dgmath.h"
#include "player.h"
#include "render/render.h"

typedef struct ViewState {
    float yaw, pitch;          // authoritative view angles on this machine (sent in UserCmds)
    float smooth_eye_y;
    bool smooth_init;
    float bob_phase, bob_amount;
    float fov;
    float land_offset, land_vel;
    u32 last_land_tick;
    float roll;
    vec3 eye;
    float bob_roll;
} ViewState;

void camera_register_cvars(void);
// Applies look input to the view angles (call every render frame).
void camera_apply_look(ViewState *v, vec2 look_delta);
// Builds the render camera from the interpolated movement state.
RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, bool ads, float ads_fov, bool dead);
float camera_base_fov(void);

#endif
