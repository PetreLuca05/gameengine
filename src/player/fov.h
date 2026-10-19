// Field of view model (docs/CAMERA_HOLDPOINT.md "Field of view"), after Call of Duty: Modern Warfare
// II: three player settings, pure math.
//   Field of View         the world camera at the hip, 80 .. 150, horizontal degrees at 16:9; Hor+:
//                         the vertical FOV is the same on every aspect (21:9 sees more to the sides)
//   ADS Field of View     Independent: aiming goes back to the default 80, then the sight's own zoom;
//                         Affected: the sight's FOV at 80 scaled by Field of View / 80
//   Weapon Field of View  Narrow / Default / Wide: the arms layer's projection only (never the world);
//                         the layer moves sideways so the sight keeps its place on screen
// Sprint widening and the fire FOV punch are multipliers on the current FOV.
#ifndef DG_FOV_H
#define DG_FOV_H

#include "core/base.h"

#define FOV_MIN 80.0f
#define FOV_MAX 150.0f
#define FOV_DEFAULT 80.0f              // the Field of View default and the Independent ADS base
#define FOV_REF_ASPECT (16.0f / 9.0f)  // the settings' horizontal FOVs are at this aspect
#define FOV_PUNCH_REF 75.0f            // fire FOV punch degrees are authored against this vertical FOV

typedef enum FovAdsMode { FOV_ADS_INDEPENDENT, FOV_ADS_AFFECTED, FOV_ADS_MODES } FovAdsMode;
typedef enum WeaponFov { WEAPON_FOV_NARROW, WEAPON_FOV_DEFAULT, WEAPON_FOV_WIDE, WEAPON_FOV_COUNT } WeaponFov;

// One player's three settings (the local cvars, or a spectated player's forwarded in its UserCmd).
typedef struct FovPrefs {
    float fov;                         // Field of View: horizontal deg at 16:9
    int ads_mode;                      // FovAdsMode
    int weapon_fov;                    // WeaponFov
} FovPrefs;

// Vertical deg of a horizontal FOV at 16:9 (Hor+: the vertical FOV every aspect uses) and back.
float fov_h16_to_v(float h16);
float fov_v_to_h16(float v);
// The horizontal FOV a vertical FOV shows on a screen of this aspect (w / h).
float fov_v_to_h(float v, float aspect);
// A FOV narrowed by a magnification (tangent ratio, >= 1): 2 atan(tan(fov / 2) / zoom), any axis.
float fov_zoom(float fov, float zoom);
// The ADS base (horizontal deg at 16:9): the default 80 (Independent) or Field of View (Affected).
// Scopes magnify inside the lens against it.
float fov_ads_base(int ads_mode, float fov_h16);
// The aimed world FOV (horizontal deg at 16:9) for a sight's world zoom (tangent ratio at 80):
// Independent: H_s = fov_zoom(80, zoom), whatever Field of View is; Affected: H_s x Field of View / 80.
float fov_ads(int ads_mode, float fov_h16, float sight_zoom);
// The world FOV along the ADS progress t (0 hip .. 1 aimed, vertical deg): the magnification eases
// in along smoothstep(t) (the hip -> ADS curve; exactly ads_v at t = 1).
float fov_ads_blend(float hip_v, float ads_v, float t);
// Sprint widening: the tangent x (1 + widen). The fire punch: fov x (1 - punch / FOV_PUNCH_REF).
float fov_sprint(float fov, float widen);
float fov_punch(float fov, float punch_deg);
// The Weapon Field of View preset's arms FOV (horizontal deg at 16:9) from the presets
// (narrow, default, wide).
float fov_weapon(const float presets[WEAPON_FOV_COUNT], int weapon_fov);
// How far the arms layer moves sideways when its FOV changes from ref_v to arms_v (vertical deg) so a
// point at the same depth stays on the same pixel: x, y scale by tan(arms_v / 2) / tan(ref_v / 2).
float fov_lateral_scale(float arms_v, float ref_v);
// ADS look sensitivity scale from the resulting FOVs (vertical deg): "fov_ratio" c x sight / hip,
// "monitor_distance" atan(c tan(sight / 2)) / atan(c tan(hip / 2)), "off" 1.
float fov_sens_scale(const char *mode, float coef, float sight_v, float hip_v);
// The settings packed for a UserCmd (spectators see the player's own FOVs) and back. Zero = none.
u8 fov_pack_h16(float fov_h16);
u8 fov_pack_flags(int ads_mode, int weapon_fov);
bool fov_unpack(u8 fov_h16, u8 flags, FovPrefs *out);

#endif
