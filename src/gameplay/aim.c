// Aim model math (docs/AIM.md). Pure: unit tested by tests/test_aim.c.
#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "gameplay/aim.h"
#include "core/detmath.h"
#include <math.h>

const char *aim_class_name(int c) {
    switch (c) {
    case AIM_OBSTRUCTED: return "obstructed";
    case AIM_BARREL: return "barrel";
    case AIM_FRIENDLY: return "friendly";
    default: return "clear";
    }
}

static i16 mm(float v) { return (i16)DG_CLAMP((int)lrintf(v * AIM_CLAIM_MM), -32767, 32767); }

void aim_claim_pack(vec3 cam, i16 out[3]) {
    out[0] = mm(cam.x);
    out[1] = mm(cam.y);
    out[2] = mm(cam.z);
    // (0, 0, 0) means "no claim": a real muzzle is never exactly on the eye.
    if (!out[0] && !out[1] && !out[2]) out[2] = -1;
}

vec3 aim_claim_unpack(const i16 in[3]) { return v3((float)in[0] / AIM_CLAIM_MM, (float)in[1] / AIM_CLAIM_MM, (float)in[2] / AIM_CLAIM_MM); }

bool aim_claim_valid(const i16 in[3]) { return in[0] || in[1] || in[2]; }

static vec3 cap_len(vec3 v, float max_len) {
    float l = v3_len(v);
    return l > max_len && l > 1e-6f ? v3_scale(v, max_len / l) : v;
}

AimValidate aim_validate_muzzle(vec3 claim, bool have_claim, vec3 host, bool have_host, float tolerance, float reach) {
    AimValidate r;
    r.error = 0.0f;
    r.clamped = false;
    if (!have_claim) {
        r.cam = have_host ? host : v3(0, 0, 0);
        r.cam = cap_len(r.cam, reach);
        return r;
    }
    vec3 c = claim;
    if (have_host) {
        vec3 d = v3_sub(claim, host);
        r.error = v3_len(d);
        if (r.error > tolerance) {
            c = v3_add(host, v3_scale(d, tolerance / r.error));   // onto the tolerance sphere
            r.clamped = true;
        }
    }
    vec3 capped = cap_len(c, reach);
    if (v3_len2(v3_sub(capped, c)) > 1e-10f) r.clamped = true;
    r.cam = capped;
    return r;
}

static i16 unit16(float v) { return (i16)DG_CLAMP((int)lrintf(v * 32767.0f), -32767, 32767); }

void aim_dir_pack(vec3 cam_dir, i16 out[3]) {
    float l = v3_len(cam_dir);
    vec3 d = l > 1e-6f ? v3_scale(cam_dir, 1.0f / l) : v3(0, 0, -1);
    out[0] = unit16(d.x);
    out[1] = unit16(d.y);
    out[2] = unit16(d.z);
    if (!out[0] && !out[1] && !out[2]) out[2] = -32767;
}

vec3 aim_dir_unpack(const i16 in[3]) {
    vec3 d = v3((float)in[0], (float)in[1], (float)in[2]);
    float l = v3_len(d);
    return l > 1e-6f ? v3_scale(d, 1.0f / l) : v3(0, 0, -1);
}

static float angle_between(vec3 a, vec3 b) { return dm_atan2f(v3_len(v3_cross(a, b)), v3_dot(a, b)); }

AimValidateDir aim_validate_dir(vec3 claim, bool have_claim, vec3 host, bool have_host, float tol_deg) {
    AimValidateDir r;
    r.error_deg = 0.0f;
    r.clamped = false;
    if (!have_claim) {
        r.dir = have_host ? host : v3(0, 0, -1);
        return r;
    }
    r.dir = claim;
    if (!have_host) return r;
    float ang = angle_between(claim, host);
    r.error_deg = ang * DG_RAD2DEG;
    float tol = tol_deg * DG_DEG2RAD;
    if (ang > tol) {
        // Onto the tolerance cone: the host's axis turned by tol towards the claim.
        vec3 perp = v3_sub(claim, v3_scale(host, v3_dot(claim, host)));
        float pl = v3_len(perp);
        vec3 pn = pl > 1e-6f ? v3_scale(perp, 1.0f / pl) : v3(0, 0, 0);
        r.dir = v3_norm(v3_add(v3_scale(host, dm_cosf(tol)), v3_scale(pn, dm_sinf(tol))));
        r.clamped = true;
    }
    return r;
}

vec3 aim_view_delta(vec3 dir, float yaw, float pitch, float dyaw, float dpitch) {
    if (dyaw == 0.0f && dpitch == 0.0f) return dir;
    // Into the old view's camera space, out through the new view.
    vec3 cam = quat_rotate(quat_conj(aim_view_rot(yaw, pitch)), dir);
    return quat_rotate(aim_view_rot(yaw + dyaw, pitch + dpitch), cam);
}

vec3 aim_converge(vec3 muzzle, vec3 p, vec3 cam_dir) {
    vec3 d = v3_sub(p, muzzle);
    float l = v3_len(d);
    if (l < 0.05f) return cam_dir;
    d = v3_scale(d, 1.0f / l);
    // P behind the muzzle (aim point inside the barrel's reach): keep the camera direction.
    return v3_dot(d, cam_dir) > 0.0f ? d : cam_dir;
}

float aim_obstruct_far(float cam_hit, float max_range) { return cam_hit >= 0.0f ? fminf(cam_hit, max_range) : max_range; }

float aim_obstruct_margin(const AimObstructRule *r, float far, float parallax) {
    return fmaxf(fmaxf(r->margin_abs, r->margin_rel * fmaxf(far, 0.0f)), parallax);
}

float aim_parallax(float offset, float sin_graze) {
    float s = fmaxf(fminf(sin_graze, 1.0f), 0.05f);
    float cot = sqrtf(fmaxf(1.0f - s * s, 0.0f)) / s;
    return fminf(fmaxf(offset, 0.0f) * cot, 3.0f);
}

AimClass aim_classify(float hit, float muzzle_depth, float far, float parallax, const AimObstructRule *r, bool friendly) {
    if (hit < 0.0f) return AIM_CLEAR;
    if (friendly) return AIM_FRIENDLY;
    if (hit <= r->near) return AIM_BARREL;
    return hit + muzzle_depth < far - aim_obstruct_margin(r, far, parallax) ? AIM_OBSTRUCTED : AIM_CLEAR;
}

vec3 aim_recover_origin(vec3 eye, vec3 muzzle, float f, float margin) {
    if (f >= 1.0f) return muzzle;
    vec3 d = v3_sub(muzzle, eye);
    float l = v3_len(d);
    if (l < 1e-5f) return eye;
    float t = fmaxf(0.0f, f * l - margin);
    return v3_madd(eye, v3_scale(d, 1.0f / l), t);
}

float aim_spread_px(float spread_deg, float dist, float cam_dist, float fov_v_rad, float screen_h) {
    // World radius of the cone at the impact, then its angular size from the camera.
    float r = dm_tanf(spread_deg * 0.5f * DG_DEG2RAD) * fmaxf(dist, 0.0f);
    float ang = dm_atanf(r / fmaxf(cam_dist, 0.05f));
    return dm_tanf(ang) * (screen_h * 0.5f) / dm_tanf(fov_v_rad * 0.5f);
}
