#include "fov.h"
#include "core/dgmath.h"
#include <math.h>
#include <string.h>

float fov_h16_to_v(float h16) { return 2.0f * atanf(tanf(h16 * 0.5f * DG_DEG2RAD) / FOV_REF_ASPECT) * DG_RAD2DEG; }

float fov_v_to_h16(float v) { return fov_v_to_h(v, FOV_REF_ASPECT); }

float fov_v_to_h(float v, float aspect) { return 2.0f * atanf(tanf(v * 0.5f * DG_DEG2RAD) * aspect) * DG_RAD2DEG; }

float fov_zoom(float fov, float zoom) { return 2.0f * atanf(tanf(fov * 0.5f * DG_DEG2RAD) / fmaxf(zoom, 1e-3f)) * DG_RAD2DEG; }

float fov_ads_base(int ads_mode, float fov_h16) { return ads_mode == FOV_ADS_AFFECTED ? fov_h16 : FOV_DEFAULT; }

float fov_ads(int ads_mode, float fov_h16, float sight_zoom) {
    float hs = fov_zoom(FOV_DEFAULT, fmaxf(sight_zoom, 1.0f));
    return ads_mode == FOV_ADS_AFFECTED ? hs * fov_h16 / FOV_DEFAULT : hs;
}

float fov_ads_blend(float hip_v, float ads_v, float t) {
    float th = tanf(hip_v * 0.5f * DG_DEG2RAD), ta = tanf(ads_v * 0.5f * DG_DEG2RAD);
    if (ta <= 1e-6f) return hip_v;
    float k = 1.0f + (th / ta - 1.0f) * smoothstepf(0.0f, 1.0f, saturate(t));
    return 2.0f * atanf(th / k) * DG_RAD2DEG;
}

float fov_sprint(float fov, float widen) { return 2.0f * atanf(tanf(fov * 0.5f * DG_DEG2RAD) * (1.0f + fmaxf(widen, 0.0f))) * DG_RAD2DEG; }

float fov_punch(float fov, float punch_deg) { return fov * (1.0f - punch_deg / FOV_PUNCH_REF); }

float fov_weapon(const float presets[WEAPON_FOV_COUNT], int weapon_fov) { return presets[DG_CLAMP(weapon_fov, 0, WEAPON_FOV_COUNT - 1)]; }

float fov_lateral_scale(float arms_v, float ref_v) {
    float r = tanf(ref_v * 0.5f * DG_DEG2RAD);
    return r > 1e-6f ? tanf(arms_v * 0.5f * DG_DEG2RAD) / r : 1.0f;
}

float fov_sens_scale(const char *mode, float coef, float sight_v, float hip_v) {
    if (sight_v <= 1.0f || hip_v <= 1.0f || !mode || !strcmp(mode, "off")) return 1.0f;
    if (!strcmp(mode, "fov_ratio")) return coef * sight_v / hip_v;
    // monitor_distance: the same mouse motion moves a point at c x (half the screen height) from
    // the centre by the same amount in both FOVs (c -> 0: zoom ratio, c = 1: the screen edge).
    float k = fmaxf(coef, 1e-3f);
    float a = atanf(k * tanf(sight_v * 0.5f * DG_DEG2RAD)), b = atanf(k * tanf(hip_v * 0.5f * DG_DEG2RAD));
    return b > 1e-6f ? a / b : 1.0f;
}

u8 fov_pack_h16(float fov_h16) { return (u8)DG_CLAMP((int)lroundf(fov_h16), 1, 255); }

u8 fov_pack_flags(int ads_mode, int weapon_fov) {
    return (u8)(0x80u | (ads_mode == FOV_ADS_AFFECTED ? 1u : 0u) | ((u32)DG_CLAMP(weapon_fov, 0, WEAPON_FOV_COUNT - 1) << 1));
}

bool fov_unpack(u8 fov_h16, u8 flags, FovPrefs *out) {
    if (!fov_h16 || !(flags & 0x80u)) return false;
    out->fov = clampf((float)fov_h16, FOV_MIN, FOV_MAX);
    out->ads_mode = (flags & 1u) ? FOV_ADS_AFFECTED : FOV_ADS_INDEPENDENT;
    out->weapon_fov = DG_CLAMP((int)((flags >> 1) & 3u), 0, WEAPON_FOV_COUNT - 1);
    return true;
}
