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
    vec3 shake;                // the movement shake's eye offset this frame (cl_move_shake: bob + landing dip; world)
} ViewState;

// ADS input (game_sights.c). The world FOV narrows by world_zoom and the arms / weapon projection by
// vm_zoom along the weapon's ADS progress (exactly those once t reaches 1).
typedef struct CameraAds {
    float t;                   // 0 hip .. 1 fully aimed (WeaponState.ads_t)
    float vm_zoom;             // arms / weapon magnification at t = 1 (tan ratio, >= 1; 0 / 1 = none)
    float world_zoom;          // world camera magnification at t = 1 (tan ratio, ~1.1; 0 / 1 = none)
} CameraAds;

void camera_register_cvars(void);
// Applies look input to the view angles (call every render frame).
void camera_apply_look(ViewState *v, vec2 look_delta);
// Builds the render camera from the interpolated movement state.
RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, const CameraAds *ads, bool dead);
// The world camera's vertical FOV for this screen: cl_fov (or the host's sv_fov override / sv_fov_max
// cap), kept at sv_fov_max_aspect's horizontal FOV on wider screens; before sprint widening, the ADS
// world zoom and the fire FOV punch.
float camera_base_fov(void);
float camera_hands_fov(void);   // the arms layer's hip FOV (cl_hands_fov, Hor+)
float camera_fov_for_aspect(float fov_v, float max_aspect, float aspect);   // pure
float camera_fov_setting(float cl_fov, float sv_fov, float sv_fov_max);    // pure: override > cap > the player's
// The world FOV at ADS progress t (0..1) for a sight's world zoom (tangent ratio): pure.
float camera_ads_fov(float hip_fov, float world_zoom, float ads_t);
float camera_ads_world_zoom_default(void);   // cl_ads_world_zoom

#endif
