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
    float fov_hip;             // smoothed hip FOV (sprint widening), the ADS blend starts from it
} ViewState;

// ADS FOV input (game_sights.c): the blend follows the weapon's ADS progress exactly.
typedef struct CameraAds {
    float t;                   // 0 hip .. 1 fully aimed (WeaponState.ads_t)
    float fov;                 // main camera FOV at t = 1 (deg, already scaled for cl_fov); 0 = none
    float snap_fov;            // > 0: full-screen scope FOV, used once t reaches 1 (exactly)
} CameraAds;

void camera_register_cvars(void);
// Applies look input to the view angles (call every render frame).
void camera_apply_look(ViewState *v, vec2 look_delta);
// Builds the render camera from the interpolated movement state.
RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, const CameraAds *ads, bool dead);
float camera_base_fov(void);

#endif
