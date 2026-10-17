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

// ADS input (game_sights.c). The world FOV never changes with ADS; the arms / weapon projection
// narrows by vm_zoom along the weapon's ADS progress (exactly vm_zoom once t reaches 1).
typedef struct CameraAds {
    float t;                   // 0 hip .. 1 fully aimed (WeaponState.ads_t)
    float vm_zoom;             // arms / weapon magnification at t = 1 (tan ratio, >= 1; 0 / 1 = none)
} CameraAds;

void camera_register_cvars(void);
// Applies look input to the view angles (call every render frame).
void camera_apply_look(ViewState *v, vec2 look_delta);
// Builds the render camera from the interpolated movement state.
RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, const CameraAds *ads, bool dead);
// The world camera's vertical FOV for this screen: sv_fov, kept at sv_fov_max_aspect's horizontal
// FOV on wider screens (no extra view to the sides); before sprint widening / the fire FOV punch.
float camera_base_fov(void);
float camera_fov_for_aspect(float fov_v, float max_aspect, float aspect);   // pure

#endif
