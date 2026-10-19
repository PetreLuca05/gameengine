// Shot origin math (docs/AIM.md). Pure: unit tested by tests/test_aim.c.
#define DG_DETMATH   // cross-platform bit-identical math (core/dgmath.h)
#include "gameplay/aim.h"
#include "core/detmath.h"
#include <math.h>

static i16 mm(float v) { return (i16)DG_CLAMP((int)lrintf(v * AIM_CLAIM_MM), -32767, 32767); }

void aim_claim_pack(vec3 cam, i16 out[3]) {
    out[0] = mm(cam.x);
    out[1] = mm(cam.y);
    out[2] = mm(cam.z);
    // (0, 0, 0) means "no claim": a real camera is never exactly on the simulated eye.
    if (!out[0] && !out[1] && !out[2]) out[2] = -1;
}

vec3 aim_claim_unpack(const i16 in[3]) { return v3((float)in[0] / AIM_CLAIM_MM, (float)in[1] / AIM_CLAIM_MM, (float)in[2] / AIM_CLAIM_MM); }

bool aim_claim_valid(const i16 in[3]) { return in[0] || in[1] || in[2]; }

static vec3 cap_len(vec3 v, float max_len) {
    float l = v3_len(v);
    return l > max_len && l > 1e-6f ? v3_scale(v, max_len / l) : v;
}

AimValidate aim_validate_claim(vec3 claim, bool have_claim, vec3 host, bool have_host, float tolerance, float reach) {
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

vec3 aim_recover_origin(vec3 eye, vec3 point, float f, float margin) {
    if (f >= 1.0f) return point;
    vec3 d = v3_sub(point, eye);
    float l = v3_len(d);
    if (l < 1e-5f) return eye;
    float t = fmaxf(0.0f, f * l - margin);
    return v3_madd(eye, v3_scale(d, 1.0f / l), t);
}
