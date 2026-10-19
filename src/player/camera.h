// First-person camera: client-owned view angles, stair smoothing, head bob, FOV changes.
#ifndef DG_CAMERA_H
#define DG_CAMERA_H

#include "core/dgmath.h"
#include "player.h"
#include "player/fov.h"
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

// ADS input (game_sights.c). The world FOV blends to ads_fov and the arms / weapon projection narrows
// by vm_zoom along the weapon's ADS progress (exactly those once t reaches 1).
typedef struct CameraAds {
    float t;                   // 0 hip .. 1 fully aimed (WeaponState.ads_t)
    float vm_zoom;             // arms / weapon magnification at t = 1 (tan ratio, >= 1; 0 / 1 = none)
    float world_zoom;          // the sight's world magnification (tan ratio at the ADS base; 0 / 1 = none)
    float ads_fov;             // the aimed world FOV (vertical deg, camera_ads_world_fov; 0 = no ADS)
} CameraAds;

void camera_register_cvars(void);
// Applies look input to the view angles (call every render frame).
void camera_apply_look(ViewState *v, vec2 look_delta);
// Builds the render camera from the interpolated movement state.
RenderCamera camera_update(ViewState *v, const PlayerMove *m, float dt, const CameraAds *ads, bool dead);
// The FOV settings (player/fov.h): the local cvars, and the viewed player's (a spectated player's
// forwarded values while set: camera_set_view_prefs, NULL = the local ones again).
FovPrefs camera_fov_prefs_local(void);
FovPrefs camera_fov_prefs(void);
void camera_set_view_prefs(const FovPrefs *p);
// The vertical FOV for this screen of a horizontal FOV at 16:9 (Hor+; sv_fov_max_aspect caps it when set).
float camera_fov_v(float h16);
// The world camera's vertical FOV for this screen: Field of View (or the host's sv_fov override /
// sv_fov_max cap); before sprint widening, ADS and the fire FOV punch.
float camera_base_fov(void);
// The ADS base (the default 80 or Field of View: ADS Field of View; scopes magnify against it) and the
// aimed world FOV for a sight's world zoom, vertical deg for this screen.
float camera_ads_base_fov(void);
float camera_ads_world_fov(float sight_zoom);
// The arms layer's hip FOV (vertical deg): the Weapon Field of View preset (narrow, default, wide:
// horizontal deg at 16:9), or cl_hands_fov (dev).
float camera_hands_fov(const float presets[WEAPON_FOV_COUNT]);
float camera_fov_for_aspect(float fov_v, float max_aspect, float aspect);   // pure
float camera_fov_setting(float cl_fov, float sv_fov, float sv_fov_max);    // pure: override > cap > the player's
float camera_ads_world_zoom_default(void);   // cl_ads_world_zoom

#endif
