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

// The shortest rotation taking unit a onto unit b (antiparallel: half a turn about an axis normal to a).
static quat from_to(vec3 a, vec3 b) {
    float d = v3_dot(a, b);
    if (d < -0.999999f) {
        vec3 ax = fabsf(a.x) < 0.9f ? v3_cross(a, v3(1, 0, 0)) : v3_cross(a, v3(0, 1, 0));
        ax = v3_norm(ax);
        quat q = { ax.x, ax.y, ax.z, 0.0f };
        return q;
    }
    vec3 c = v3_cross(a, b);
    quat q = { c.x, c.y, c.z, 1.0f + d };
    return quat_norm(q);
}

// Unit `from` turned towards unit `to` by ang (rad) in their common plane.
static vec3 turn_towards(vec3 from, vec3 to, float ang) {
    vec3 perp = v3_sub(to, v3_scale(from, v3_dot(to, from)));
    float pl = v3_len(perp);
    if (pl < 1e-9f) return from;
    vec3 pn = v3_scale(perp, 1.0f / pl);
    return v3_norm(v3_add(v3_scale(from, dm_cosf(ang)), v3_scale(pn, dm_sinf(ang))));
}

float aim_line_miss(vec3 origin, vec3 dir, vec3 p) {
    vec3 v = v3_sub(p, origin);
    float t = fmaxf(0.0f, v3_dot(v, dir));
    return v3_len(v3_madd(v, dir, -t));
}

float aim_reach_tol(float far) { return fmaxf(AIM_REACH_MIN, fmaxf(far, 0.0f) * dm_tanf(AIM_REACH_DEG * DG_DEG2RAD)); }

AimHold aim_hold_solve(const AimHoldRaw *raw, vec3 p) {
    AimHold h;
    h.rot = quat_identity();
    h.muzzle = raw->muzzle;
    h.barrel = raw->barrel;
    h.angle_deg = 0.0f;
    h.clamped = false;
    vec3 B = raw->barrel;
    // P (nearly) at the muzzle: no turn, eased in so a P sliding past the threshold never snaps the gun.
    float ahead = v3_dot(v3_sub(p, raw->muzzle), B);
    float fade = clampf((ahead - AIM_HOLD_MIN_AHEAD) / AIM_HOLD_FADE, 0.0f, 1.0f);
    float w = raw->w * fade * fade * (3.0f - 2.0f * fade);
    if (raw->valid && w > 0.0f && raw->max_rad > 0.0f) {
        // The bore line relative to the pivot C: its perpendicular offset b (M - C = b + s B). Turned by
        // R it is {C + R b + s R B}: through P when R B = u = normalize(P - C - R b). R depends on u, a
        // contraction (ratio ~|b| / |P - C|): a fixed number of iterations, so every machine stops at
        // the same bits.
        vec3 C = raw->pivot;
        vec3 mc = v3_sub(raw->muzzle, C);
        vec3 b = v3_madd(mc, B, -v3_dot(mc, B));
        quat R = quat_identity();
        vec3 u = B;
        for (int i = 0; i < AIM_HOLD_ITERS; i++) {
            vec3 t = v3_sub(v3_sub(p, C), quat_rotate(R, b));
            float l = v3_len(t);
            if (l < 1e-4f) break;
            u = v3_scale(t, 1.0f / l);
            R = from_to(B, u);
        }
        float ang = angle_between(B, u);
        if (ang > raw->max_rad) {
            // Out of reach: as far as the limit allows (the line then misses P: obstructed).
            u = turn_towards(B, u, raw->max_rad);
            ang = raw->max_rad;
            h.clamped = true;
        }
        if (w < 1.0f) u = turn_towards(B, u, ang * w);
        R = from_to(B, u);
        h.rot = R;
        h.muzzle = v3_add(C, quat_rotate(R, mc));
        h.barrel = quat_rotate(R, B);
        h.angle_deg = angle_between(B, h.barrel) * DG_RAD2DEG;
    }
    h.miss = aim_line_miss(h.muzzle, h.barrel, p);
    return h;
}

float aim_obstruct_far(float cam_hit, float max_range) { return cam_hit >= 0.0f ? fminf(cam_hit, max_range) : max_range; }

float aim_obstruct_margin(const AimObstructRule *r, float far, float parallax) {
    return fmaxf(fmaxf(r->margin_abs, r->margin_rel * fmaxf(far, 0.0f)), parallax);
}

float aim_parallax(float separation, float sin_graze, float far) {
    float s = fmaxf(fminf(sin_graze, 1.0f), 0.005f);
    float cot = sqrtf(fmaxf(1.0f - s * s, 0.0f)) / s;
    return fminf(fmaxf(separation, 0.0f) * cot, fmaxf(3.0f, 0.25f * fmaxf(far, 0.0f)));
}

AimClass aim_classify(const AimClassIn *in, const AimObstructRule *r) {
    if (in->friendly && in->hit >= 0.0f) return AIM_FRIENDLY;
    if (in->recovered || (in->hit >= 0.0f && in->hit <= r->near)) return AIM_BARREL;
    if (in->hit >= 0.0f && in->hit + in->depth < in->far - aim_obstruct_margin(r, in->far, in->parallax)) return AIM_OBSTRUCTED;
    // The barrel line does not end at P (the gun was moved): the bullet lands where the line does.
    if (in->miss > aim_reach_tol(in->far)) return AIM_OBSTRUCTED;
    return AIM_CLEAR;
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
