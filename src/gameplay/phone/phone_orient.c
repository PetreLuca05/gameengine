// Phone orientation math (see phone_orient.h).
#include "phone_orient.h"

static quat qhemi(quat q, quat ref) {
    if (q.x * ref.x + q.y * ref.y + q.z * ref.z + q.w * ref.w < 0) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    return q;
}

quat phone_device_quat(float alpha_deg, float beta_deg, float gamma_deg) {
    quat qz = quat_axis_angle(v3(0, 0, 1), alpha_deg * DG_DEG2RAD);
    quat qx = quat_axis_angle(v3(1, 0, 0), beta_deg * DG_DEG2RAD);
    quat qy = quat_axis_angle(v3(0, 1, 0), gamma_deg * DG_DEG2RAD);
    return quat_norm(quat_mul(quat_mul(qz, qx), qy));
}

quat phone_orient_game(quat q_dev) {
    quat m = quat_axis_angle(v3(1, 0, 0), -90.0f * DG_DEG2RAD);
    return quat_norm(quat_mul(m, quat_norm(q_dev)));
}

float phone_heading(quat q) {
    vec3 f = quat_rotate(q, v3(0, 0, -1));
    vec3 top = quat_rotate(q, v3(0, 1, 0));
    float fh = sqrtf(f.x * f.x + f.z * f.z), th = sqrtf(top.x * top.x + top.z * top.z);
    // Blend between the lens and the screen top by how horizontal each is (no jump at the switch).
    float w = smoothstepf(0.25f, 0.55f, fh);
    vec3 d = v3_add(v3_scale(v3(f.x, 0, f.z), w), v3_scale(v3(top.x, 0, top.z), (1.0f - w) * (th > 1e-4f ? 1.0f : 0.0f)));
    if (v3_len2(d) < 1e-10f) return 0.0f;
    return atan2f(-d.x, -d.z);
}

quat phone_apply_calibration(quat q_game, float calib_yaw) { return quat_norm(quat_mul(quat_yaw(-calib_yaw), q_game)); }

vec3 phone_euler_ypr(quat q) {
    q = quat_norm(q);
    vec3 f = quat_rotate(q, v3(0, 0, -1));
    float yaw = atan2f(-f.x, -f.z);
    float pitch = asinf(clampf(f.y, -1.0f, 1.0f));
    if (fabsf(f.y) > 0.99999f) {
        // Straight up / down: yaw is undefined, take it from the screen top.
        vec3 up = quat_rotate(q, v3(0, 1, 0));
        yaw = f.y > 0 ? atan2f(up.x, up.z) : atan2f(-up.x, -up.z);
    }
    quat rest = quat_mul(quat_conj(quat_mul(quat_yaw(yaw), quat_axis_angle(v3(1, 0, 0), pitch))), q);
    float roll = 2.0f * atan2f(rest.z, rest.w);
    return v3(yaw, pitch, wrap_angle(roll));
}

quat phone_from_ypr(vec3 a) { return quat_norm(quat_euler(a.x, a.y, a.z)); }

quat phone_shape(quat q, float sens, bool iy, bool ip, bool ir) {
    if (sens == 1.0f && !iy && !ip && !ir) return q;
    vec3 a = phone_euler_ypr(q);
    a = v3_scale(a, sens);
    if (iy) a.x = -a.x;
    if (ip) a.y = -a.y;
    if (ir) a.z = -a.z;
    a.y = clampf(a.y, -89.0f * DG_DEG2RAD, 89.0f * DG_DEG2RAD);
    return phone_from_ypr(a);
}

PhoneReach phone_reach_default(void) {
    PhoneReach r = { 55.0f, 70.0f, 70.0f, 100.0f, 0.8f };
    return r;
}

// Linear up to knee * lim, then an exponential approach to lim (C1 continuous, never past it).
static float soft_limit(float x, float lim, float knee) {
    float a = fabsf(x), k = lim * knee;
    if (a <= k || lim <= 0) return lim <= 0 ? 0.0f : x;
    float span = lim - k;
    float y = k + span * (1.0f - expf(-(a - k) / fmaxf(span, 1e-6f)));
    return copysignf(fminf(y, lim), x);
}

quat phone_clamp_reach(quat q, const PhoneReach *r) {
    vec3 a = phone_euler_ypr(q);
    float knee = clampf(r->knee, 0.0f, 1.0f);
    a.x = soft_limit(a.x, r->yaw * DG_DEG2RAD, knee);
    a.y = a.y >= 0 ? soft_limit(a.y, r->pitch_up * DG_DEG2RAD, knee) : soft_limit(a.y, r->pitch_down * DG_DEG2RAD, knee);
    a.z = soft_limit(a.z, r->roll * DG_DEG2RAD, knee);
    return phone_from_ypr(a);
}

bool phone_in_reach(quat q, const PhoneReach *r, float tol) {
    vec3 a = v3_scale(phone_euler_ypr(q), DG_RAD2DEG);
    return fabsf(a.x) <= r->yaw + tol && a.y <= r->pitch_up + tol && -a.y <= r->pitch_down + tol && fabsf(a.z) <= r->roll + tol;
}

quat phone_to_camera(quat q_rel, float pitch) { return quat_norm(quat_mul(quat_conj(quat_axis_angle(v3(1, 0, 0), pitch)), q_rel)); }

// ------------------------------------------------------------------ 1-euro

static float euro_alpha(float cutoff, float dt) {
    float tau = 1.0f / (2.0f * DG_PI * fmaxf(cutoff, 1e-3f));
    return 1.0f / (1.0f + tau / fmaxf(dt, 1e-5f));
}

void phone_one_euro_init(PhoneOneEuro *f, float min_cutoff, float beta, float d_cutoff) {
    memset(f, 0, sizeof(*f));
    f->min_cutoff = min_cutoff;
    f->beta = beta;
    f->d_cutoff = d_cutoff;
}

quat phone_one_euro(PhoneOneEuro *f, quat q, float dt) {
    q = quat_norm(q);
    if (!f->init || dt <= 0) {
        if (!f->init) {
            f->x = q;
            f->speed = 0;
            f->init = true;
        }
        return f->x;
    }
    q = qhemi(q, f->x);
    float d = fabsf(f->x.x * q.x + f->x.y * q.y + f->x.z * q.z + f->x.w * q.w);
    float ang = 2.0f * acosf(fminf(d, 1.0f));
    float speed = ang / dt;
    f->speed = lerpf(f->speed, speed, euro_alpha(f->d_cutoff, dt));
    float a = euro_alpha(f->min_cutoff + f->beta * f->speed, dt);
    f->x = quat_nlerp(f->x, q, a);
    return f->x;
}

// ------------------------------------------------------------------ packing

u32 phone_quat_pack(quat q) {
    q = quat_norm(q);
    float c[4] = { q.x, q.y, q.z, q.w };
    int big = 0;
    for (int i = 1; i < 4; i++)
        if (fabsf(c[i]) > fabsf(c[big])) big = i;
    float s = c[big] < 0 ? -1.0f : 1.0f;
    u32 v = (u32)big << 30;
    int sh = 20;
    const float R = 0.70710678f;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        float n = clampf(c[i] * s / R, -1.0f, 1.0f);
        u32 b = (u32)lrintf((n * 0.5f + 0.5f) * 1023.0f);
        v |= (b & 1023u) << sh;
        sh -= 10;
    }
    return v;
}

quat phone_quat_unpack(u32 v) {
    int big = (int)(v >> 30);
    float c[4];
    int sh = 20;
    float sum = 0;
    const float R = 0.70710678f;
    for (int i = 0; i < 4; i++) {
        if (i == big) continue;
        float n = (float)((v >> sh) & 1023u) / 1023.0f * 2.0f - 1.0f;
        c[i] = n * R;
        sum += c[i] * c[i];
        sh -= 10;
    }
    c[big] = sqrtf(fmaxf(0.0f, 1.0f - sum));
    quat q = { c[0], c[1], c[2], c[3] };
    return quat_norm(q);
}

quat phone_quat_from_i16(const i16 v[4]) {
    quat q = { v[0] / 32767.0f, v[1] / 32767.0f, v[2] / 32767.0f, v[3] / 32767.0f };
    return quat_norm(q);
}

void phone_quat_to_i16(quat q, i16 out[4]) {
    q = quat_norm(q);
    float c[4] = { q.x, q.y, q.z, q.w };
    for (int i = 0; i < 4; i++) out[i] = (i16)lrintf(clampf(c[i], -1.0f, 1.0f) * 32767.0f);
}
