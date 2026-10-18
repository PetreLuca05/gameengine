// Vector / matrix / quaternion math. Right-handed, Y up, camera looks down -Z.
// Matrices are column-major (m[col*4+row]) to match GLSL.
#ifndef DG_MATH_H
#define DG_MATH_H

#include <math.h>
#include "base.h"

// Deterministic translation units (host / predicting client must agree bit for bit across platforms:
// ballistics, recoil, sway, fire animation, aim) define DG_DETMATH before their first include: the
// inline helpers below then use the cross-platform kernels of detmath.h instead of the platform libm
// (docs/PLATFORM_MACOS.md "Determinism").
#ifdef DG_DETMATH
#include "detmath.h"
#define DGM_SINF dm_sinf
#define DGM_COSF dm_cosf
#define DGM_TANF dm_tanf
#define DGM_EXPF dm_expf
#define DGM_ATAN2F dm_atan2f
#else
#define DGM_SINF sinf
#define DGM_COSF cosf
#define DGM_TANF tanf
#define DGM_EXPF expf
#define DGM_ATAN2F atan2f
#endif

#define DG_PI 3.14159265358979323846f
#define DG_DEG2RAD (DG_PI / 180.0f)
#define DG_RAD2DEG (180.0f / DG_PI)

typedef struct vec2 { float x, y; } vec2;
typedef struct vec3 { float x, y, z; } vec3;
typedef struct vec4 { float x, y, z, w; } vec4;
typedef struct quat { float x, y, z, w; } quat;
typedef struct mat4 { float m[16]; } mat4;

DG_INLINE vec2 v2(float x, float y) { vec2 r = { x, y }; return r; }
DG_INLINE vec3 v3(float x, float y, float z) { vec3 r = { x, y, z }; return r; }
DG_INLINE vec4 v4(float x, float y, float z, float w) { vec4 r = { x, y, z, w }; return r; }
DG_INLINE vec3 v3s(float s) { return v3(s, s, s); }

DG_INLINE float lerpf(float a, float b, float t) { return a + (b - a) * t; }
DG_INLINE float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
DG_INLINE float saturate(float x) { return clampf(x, 0.0f, 1.0f); }
DG_INLINE float signf(float x) { return x < 0.0f ? -1.0f : 1.0f; }
DG_INLINE float smoothstepf(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
DG_INLINE float approachf(float cur, float target, float delta) {
    if (cur < target) return cur + delta > target ? target : cur + delta;
    return cur - delta < target ? target : cur - delta;
}
// Frame-rate independent exponential smoothing factor.
DG_INLINE float exp_decay(float rate, float dt) { return 1.0f - DGM_EXPF(-rate * dt); }
DG_INLINE float wrap_angle(float a) {
    // Huge or non-finite input (a teleport / degenerate spring) would never leave the loops below:
    // reduce it first (ordinary angles keep the exact loop results).
    if (!(a > -1e4f && a < 1e4f)) a = isfinite(a) ? fmodf(a, 2.0f * DG_PI) : 0.0f;
    while (a > DG_PI) a -= 2.0f * DG_PI;
    while (a < -DG_PI) a += 2.0f * DG_PI;
    return a;
}

DG_INLINE vec2 v2_add(vec2 a, vec2 b) { return v2(a.x + b.x, a.y + b.y); }
DG_INLINE vec2 v2_sub(vec2 a, vec2 b) { return v2(a.x - b.x, a.y - b.y); }
DG_INLINE vec2 v2_scale(vec2 a, float s) { return v2(a.x * s, a.y * s); }
DG_INLINE float v2_len(vec2 a) { return sqrtf(a.x * a.x + a.y * a.y); }

DG_INLINE vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
DG_INLINE vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
DG_INLINE vec3 v3_mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
DG_INLINE vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
DG_INLINE vec3 v3_neg(vec3 a) { return v3(-a.x, -a.y, -a.z); }
DG_INLINE vec3 v3_madd(vec3 a, vec3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
DG_INLINE float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
DG_INLINE vec3 v3_cross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
DG_INLINE float v3_len2(vec3 a) { return v3_dot(a, a); }
DG_INLINE float v3_len(vec3 a) { return sqrtf(v3_dot(a, a)); }
DG_INLINE float v3_dist(vec3 a, vec3 b) { return v3_len(v3_sub(a, b)); }
DG_INLINE vec3 v3_norm(vec3 a) { float l = v3_len(a); return l > 1e-8f ? v3_scale(a, 1.0f / l) : v3(0, 0, 0); }
DG_INLINE vec3 v3_lerp(vec3 a, vec3 b, float t) { return v3(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t)); }
DG_INLINE vec3 v3_min(vec3 a, vec3 b) { return v3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)); }
DG_INLINE vec3 v3_max(vec3 a, vec3 b) { return v3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)); }
DG_INLINE vec3 v3_flat(vec3 a) { return v3(a.x, 0.0f, a.z); }
DG_INLINE bool v3_eq(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

DG_INLINE vec4 v4_lerp(vec4 a, vec4 b, float t) { return v4(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t)); }

// Yaw rotates around +Y; yaw=0, pitch=0 looks down -Z.
DG_INLINE vec3 dir_from_angles(float yaw, float pitch) {
    float cp = DGM_COSF(pitch);
    return v3(-DGM_SINF(yaw) * cp, DGM_SINF(pitch), -DGM_COSF(yaw) * cp);
}
DG_INLINE vec3 right_from_yaw(float yaw) { return v3(DGM_COSF(yaw), 0.0f, -DGM_SINF(yaw)); }
DG_INLINE vec3 forward_from_yaw(float yaw) { return v3(-DGM_SINF(yaw), 0.0f, -DGM_COSF(yaw)); }

// ------------------------------------------------------------------ quaternion
DG_INLINE quat quat_identity(void) { quat q = { 0, 0, 0, 1 }; return q; }
DG_INLINE quat quat_axis_angle(vec3 axis, float angle) {
    vec3 n = v3_norm(axis);
    float s = DGM_SINF(angle * 0.5f);
    quat q = { n.x * s, n.y * s, n.z * s, DGM_COSF(angle * 0.5f) };
    return q;
}
DG_INLINE quat quat_mul(quat a, quat b) {
    quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
DG_INLINE quat quat_conj(quat q) { quat r = { -q.x, -q.y, -q.z, q.w }; return r; }
DG_INLINE quat quat_norm(quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-8f) return quat_identity();
    quat r = { q.x / l, q.y / l, q.z / l, q.w / l };
    return r;
}
DG_INLINE vec3 quat_rotate(quat q, vec3 v) {
    vec3 u = v3(q.x, q.y, q.z);
    vec3 t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}
DG_INLINE quat quat_nlerp(quat a, quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    quat r = { lerpf(a.x, b.x, t), lerpf(a.y, b.y, t), lerpf(a.z, b.z, t), lerpf(a.w, b.w, t) };
    return quat_norm(r);
}
DG_INLINE quat quat_yaw(float yaw) { return quat_axis_angle(v3(0, 1, 0), yaw); }
DG_INLINE quat quat_euler(float yaw, float pitch, float roll) {
    quat qy = quat_axis_angle(v3(0, 1, 0), yaw);
    quat qp = quat_axis_angle(v3(1, 0, 0), pitch);
    quat qr = quat_axis_angle(v3(0, 0, 1), roll);
    return quat_mul(quat_mul(qy, qp), qr);
}
DG_INLINE float quat_get_yaw(quat q) {
    vec3 f = quat_rotate(q, v3(0, 0, -1));
    return DGM_ATAN2F(-f.x, -f.z);
}

// ------------------------------------------------------------------ matrix
DG_INLINE mat4 mat4_identity(void) {
    mat4 r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    return r;
}
DG_INLINE mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
DG_INLINE vec4 mat4_mul_v4(mat4 a, vec4 v) {
    vec4 r;
    r.x = a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w;
    r.y = a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w;
    r.z = a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w;
    r.w = a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w;
    return r;
}
DG_INLINE vec3 mat4_mul_point(mat4 a, vec3 p) {
    vec4 r = mat4_mul_v4(a, v4(p.x, p.y, p.z, 1.0f));
    return v3(r.x, r.y, r.z);
}
DG_INLINE vec3 mat4_mul_dir(mat4 a, vec3 p) {
    vec4 r = mat4_mul_v4(a, v4(p.x, p.y, p.z, 0.0f));
    return v3(r.x, r.y, r.z);
}
DG_INLINE mat4 mat4_translate(vec3 t) {
    mat4 r = mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
DG_INLINE mat4 mat4_scale(vec3 s) {
    mat4 r = mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}
DG_INLINE mat4 mat4_from_quat(quat q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat4 r = mat4_identity();
    r.m[0] = 1 - 2 * (yy + zz); r.m[1] = 2 * (xy + wz);     r.m[2] = 2 * (xz - wy);
    r.m[4] = 2 * (xy - wz);     r.m[5] = 1 - 2 * (xx + zz); r.m[6] = 2 * (yz + wx);
    r.m[8] = 2 * (xz + wy);     r.m[9] = 2 * (yz - wx);     r.m[10] = 1 - 2 * (xx + yy);
    return r;
}
// Translation * Rotation * Scale.
DG_INLINE mat4 mat4_trs(vec3 t, quat q, vec3 s) {
    mat4 r = mat4_from_quat(q);
    r.m[0] *= s.x; r.m[1] *= s.x; r.m[2] *= s.x;
    r.m[4] *= s.y; r.m[5] *= s.y; r.m[6] *= s.y;
    r.m[8] *= s.z; r.m[9] *= s.z; r.m[10] *= s.z;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
// Vulkan perspective: depth 0..1, clip Y down (flipped here so +Y is up on screen).
DG_INLINE mat4 mat4_perspective(float fovy, float aspect, float znear, float zfar) {
    float f = 1.0f / DGM_TANF(fovy * 0.5f);
    mat4 r = { { 0 } };
    r.m[0] = f / aspect;
    r.m[5] = -f;
    r.m[10] = zfar / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (znear * zfar) / (znear - zfar);
    return r;
}
DG_INLINE mat4 mat4_ortho(float l, float r_, float b, float t, float n, float f) {
    mat4 r = mat4_identity();
    r.m[0] = 2.0f / (r_ - l);
    r.m[5] = -2.0f / (t - b);
    r.m[10] = -1.0f / (f - n);
    r.m[12] = -(r_ + l) / (r_ - l);
    r.m[13] = (t + b) / (t - b);
    r.m[14] = -n / (f - n);
    return r;
}
DG_INLINE mat4 mat4_look_at(vec3 eye, vec3 target, vec3 up) {
    vec3 f = v3_norm(v3_sub(target, eye));
    vec3 s = v3_norm(v3_cross(f, up));
    vec3 u = v3_cross(s, f);
    mat4 r = mat4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3_dot(s, eye);
    r.m[13] = -v3_dot(u, eye);
    r.m[14] = v3_dot(f, eye);
    return r;
}
// View matrix for an FPS camera at eye with yaw/pitch/roll.
DG_INLINE mat4 mat4_fps_view(vec3 eye, float yaw, float pitch, float roll) {
    quat q = quat_euler(yaw, pitch, roll);
    mat4 rot = mat4_from_quat(quat_conj(q));
    return mat4_mul(rot, mat4_translate(v3_neg(eye)));
}
DG_INLINE mat4 mat4_inverse_rigid(mat4 a) {
    mat4 r = mat4_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i * 4 + j] = a.m[j * 4 + i];
    vec3 t = v3(a.m[12], a.m[13], a.m[14]);
    r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8] * t.z);
    r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9] * t.z);
    r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
    return r;
}
// General inverse (cofactor expansion). Returns identity for singular matrices.
DG_INLINE mat4 mat4_inverse(mat4 a) {
    const float *m = a.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det > -1e-20f && det < 1e-20f) return mat4_identity();
    float id = 1.0f / det;
    mat4 r;
    for (int i = 0; i < 16; i++) r.m[i] = inv[i] * id;
    return r;
}

// ------------------------------------------------------------------ geometry
typedef struct Ray { vec3 origin, dir; } Ray;
typedef struct AABB { vec3 min, max; } AABB;

// Ray vs capsule (segment a-b, radius r). Returns hit distance or -1.
DG_INLINE float ray_capsule(vec3 ro, vec3 rd, vec3 a, vec3 b, float r) {
    vec3 ba = v3_sub(b, a), oa = v3_sub(ro, a);
    float baba = v3_dot(ba, ba), bard = v3_dot(ba, rd), baoa = v3_dot(ba, oa);
    float rdoa = v3_dot(rd, oa), oaoa = v3_dot(oa, oa);
    float qa = baba - bard * bard;
    float qb = baba * rdoa - baoa * bard;
    float qc = baba * oaoa - baoa * baoa - r * r * baba;
    float h = qb * qb - qa * qc;
    if (h >= 0.0f) {
        float t = (-qb - sqrtf(h)) / qa;
        float y = baoa + t * bard;
        if (y > 0.0f && y < baba) return t;
        vec3 oc = (y <= 0.0f) ? oa : v3_sub(ro, b);
        qb = v3_dot(rd, oc);
        qc = v3_dot(oc, oc) - r * r;
        h = qb * qb - qc;
        if (h > 0.0f) return -qb - sqrtf(h);
    }
    return -1.0f;
}

DG_INLINE vec3 closest_point_segment(vec3 p, vec3 a, vec3 b) {
    vec3 ab = v3_sub(b, a);
    float t = v3_dot(v3_sub(p, a), ab) / fmaxf(v3_dot(ab, ab), 1e-8f);
    return v3_madd(a, ab, saturate(t));
}

#endif
