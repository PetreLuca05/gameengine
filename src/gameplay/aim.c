// Aim model math (docs/AIM.md). Pure: unit tested by tests/test_aim.c.
#include "gameplay/aim.h"
#include "core/detmath.h"
#include <math.h>

const char *aim_class_name(int c) {
    switch (c) {
    case AIM_PARTIAL: return "partial";
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

vec3 aim_converge(vec3 muzzle, vec3 p, vec3 cam_dir) {
    vec3 d = v3_sub(p, muzzle);
    float l = v3_len(d);
    if (l < 0.05f) return cam_dir;
    d = v3_scale(d, 1.0f / l);
    // P behind the muzzle (aim point inside the barrel's reach): keep the camera direction.
    return v3_dot(d, cam_dir) > 0.0f ? d : cam_dir;
}

AimClass aim_classify(float t, float len, float obstruct_near, bool friendly) {
    if (t < 0.0f || t >= len) return AIM_CLEAR;
    if (friendly) return AIM_FRIENDLY;
    return t < obstruct_near ? AIM_BARREL : AIM_PARTIAL;
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
