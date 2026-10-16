#include "foliage_wind.h"
#include "vgeo_gpu.h"
#include "world/wind.h"
#include <math.h>
#include <string.h>

static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static float fractf(float x) { return x - floorf(x); }
static float smoothstepf(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

float vgeo_smooth_triangle(float x) {
    float tri = fabsf(fractf(x + 0.5f) * 2.0f - 1.0f);   // TriangleWave
    return tri * tri * (3.0f - 2.0f * tri) * 2.0f - 1.0f; // SmoothCurve, signed
}

void vgeo_wind_defaults(VgWindParams *p, int preset, float height) {
    memset(p, 0, sizeof(*p));
    p->height = height > 0.01f ? height : 1.0f;
    if (preset == VGWP_GRASS) {
        *p = (VgWindParams){ 0.45f, 0.0f, 0.02f, 12.0f, 0.35f, 1.2f, 3.5f, 35.0f, 0.6f, 0.6f, p->height, 0 };
    } else if (preset == VGWP_BUSH) {
        *p = (VgWindParams){ 0.12f, 0.08f, 0.03f, 16.0f, 0.3f, 1.4f, 5.0f, 14.0f, 1.5f, 0.8f, p->height, 0 };
    } else {
        *p = (VgWindParams){ 0.05f, 0.25f, 0.05f, 22.0f, 0.18f, 1.1f, 6.0f, 7.0f, 2.6f, 1.4f, p->height, 0 };
    }
}

void vgeo_wind_pack(const VgWindParams *p, float wpo[4], float wp[8]) {
    wpo[0] = p->trunk_stiffness;
    wpo[1] = p->branch_stiffness;
    wpo[2] = p->leaf_stiffness;
    wpo[3] = p->full_wind > 0.1f ? p->full_wind : 0.1f;
    wp[0] = p->height > 0.01f ? p->height : 0.01f;
    wp[1] = p->main_freq;
    wp[2] = p->detail_freq;
    wp[3] = p->flutter_freq;
    wp[4] = tanf(fminf(fmaxf(p->max_bend_deg, 0.0f), 75.0f) * (3.14159265f / 180.0f));
    wp[5] = clamp01(p->settle_time / WIND_SETTLE_PERIOD);
    wp[6] = p->response > 0.05f ? p->response : 0.05f;
    wp[7] = 0;
}

void vgeo_wind_unpack(const float wpo[4], const float wp[8], VgWindParams *p) {
    memset(p, 0, sizeof(*p));
    p->trunk_stiffness = wpo[0];
    p->branch_stiffness = wpo[1];
    p->leaf_stiffness = wpo[2];
    p->full_wind = wpo[3];
    p->height = wp[0];
    p->main_freq = wp[1];
    p->detail_freq = wp[2];
    p->flutter_freq = wp[3];
    p->max_bend_deg = atanf(wp[4]) * (180.0f / 3.14159265f);
    p->settle_time = wp[5] * WIND_SETTLE_PERIOD;
    p->response = wp[6];
}

void vgeo_wind_layer_max(const float wpo[4], const float wp[8], float out[3]) {
    // main: the horizontal push |o| = |b| h^2 H <= |b| y <= |b| L (y the height above the pivot, L the
    // distance to it), so the projection back onto the sphere of radius L moves the vertex along a chord
    // of angle <= asin(|o| / L): <= 2 L sin(asin(q) / 2) <= f(|b|) |o| with f(q) = 2 sin(asin(q) / 2) / q
    // (1 .. sqrt 2 for q <= 1; 2 beyond). |b| <= min(trunk x 1.15 (instance variation) x 1.0032 (sway
    // across), tan(max_bend)), |o| <= |b| H.
    float b = fminf(fabsf(wpo[0]) * 1.15f * 1.0032f, wp[4]);
    float f = b <= 1e-4f ? 1.0f : b < 1.0f ? 2.0f * sinf(asinf(b) * 0.5f) / b : 2.0f;
    out[0] = f * 1.0001f * b * wp[0];
    out[1] = 0.9220f * 1.15f * fabsf(wpo[1]);   // max over sy of |(0.4 + 0.3 sy, 0.6 sy)|
    out[2] = fabsf(wpo[2]);
}

// Mirrors wpo.glsl wpo_veg / wpo_veg_normal operation for operation (f32).
void vgeo_wind_eval(const float wpo[4], const float wp[8], const VgWindVertex *v, const float wind[4], float t, u32 layers, const float disp[2], float out[3],
                    float n_out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (n_out) memcpy(n_out, v->n, 12);
    float rel[3] = { v->p[0] - v->ip[0], v->p[1] - v->ip[1], v->p[2] - v->ip[2] };
    float H = wp[0] * v->pscale, h = clamp01(rel[1] / fmaxf(H, 1e-4f)), bf = h * h;
    // displacement buffer first (trample / push): it also takes the wind out of the plant
    float k = 1.0f;
    if (disp) {
        float dx = disp[0], dz = disp[1], l = sqrtf(dx * dx + dz * dz);
        if (l > VG_DISPLACE_MAX) { dx *= VG_DISPLACE_MAX / l; dz *= VG_DISPLACE_MAX / l; }
        float dl = fminf(l, VG_DISPLACE_MAX);
        out[0] += dx * h;
        out[1] += -0.5f * dl * h;
        out[2] += dz * h;
        k = 1.0f - 0.85f * dl / VG_DISPLACE_MAX;
    }
    // wind at the pivot: instant / settled mix (per-asset settle time)
    float W[2] = { wind[0] + (wind[2] - wind[0]) * wp[5], wind[1] + (wind[3] - wind[1]) * wp[5] };
    float s = sqrtf(W[0] * W[0] + W[1] * W[1]);
    float d[2] = { 1.0f, 0.0f };
    if (s > 1e-4f) { d[0] = W[0] / s; d[1] = W[1] / s; }
    float lin = clamp01(s / wpo[3]), r = powf(lin, wp[6]) * k;
    // uniqueness: per-instance frequency and stiffness variation (+-15 %) from the instance phase
    float fj = 0.85f + 0.3f * fractf(v->phase * 13.7f + 0.13f), sj = 0.85f + 0.3f * fractf(v->phase * 7.31f + 0.57f);
    // phases (cycles): object (pivot + instance hash), branch (G), vertex (mesh-space position)
    float obj = v->ip[0] * 0.173f + v->ip[1] * 0.091f + v->ip[2] * 0.137f + v->phase * 7.0f;
    float br = obj + v->col[1] * 3.0f;
    float ps = 1.7f / fmaxf(v->pscale, 1e-3f), vtx = br + (rel[0] + rel[1] + rel[2]) * ps;
    // 1. main bending
    float bvec[2] = { 0, 0 }, bl = 0;
    if (layers & VGW_MAIN) {
        // sway about the lean: two incommensurate smoothed triangle waves along the wind, one across
        float tm = t * wp[1] * fj;
        float om = 0.6f * vgeo_smooth_triangle(tm * 1.31f + obj) + 0.4f * vgeo_smooth_triangle(tm * 0.69f + obj * 1.618f + 0.2f);
        float op = vgeo_smooth_triangle(tm * 0.83f + obj + 0.31f);
        float a = wpo[0] * sj * r * (0.88f + 0.12f * om), c = wpo[0] * sj * r * 0.08f * op;
        bvec[0] = d[0] * a - d[1] * c;
        bvec[1] = d[1] * a + d[0] * c;
        bl = sqrtf(bvec[0] * bvec[0] + bvec[1] * bvec[1]);
        if (bl > wp[4]) { bvec[0] *= wp[4] / bl; bvec[1] *= wp[4] / bl; bl = wp[4]; }
        float o[3] = { bvec[0] * bf * H, 0.0f, bvec[1] * bf * H };
        float nr[3] = { rel[0] + o[0], rel[1], rel[2] + o[2] };
        float L = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]), nl = sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
        float sc = (L > 1e-6f && nl > 1e-6f) ? L / nl : 1.0f;
        for (int i = 0; i < 3; i++) out[i] += nr[i] * sc - rel[i];
    }
    // 2 / 3. detail bending and edge flutter: the chapter's four frequencies (1.975, 0.793 | 0.375,
    // 0.193, normalised to the layer frequency), smoothed triangle waves, summed in pairs
    float w4[4] = { 0, 0, 0, 0 };
    if (layers & (VGW_DETAIL | VGW_FLUTTER)) {
        float a = t * wp[3] * fj + vtx, b = t * wp[2] * fj + br;
        w4[0] = vgeo_smooth_triangle(a * 1.427f);
        w4[1] = vgeo_smooth_triangle(a * 0.573f);
        w4[2] = vgeo_smooth_triangle(b * 1.320f);
        w4[3] = vgeo_smooth_triangle(b * 0.680f);
    }
    float sx = (w4[0] + w4[2]) * 0.5f, sy = (w4[1] + w4[3]) * 0.5f;
    if (layers & VGW_DETAIL) {
        float amp = clamp01(v->col[2]) * wpo[1] * sj * r * v->pscale;
        out[0] += d[0] * (0.4f + 0.3f * sy) * amp;
        out[1] += 0.6f * sy * amp;
        out[2] += d[1] * (0.4f + 0.3f * sy) * amp;
    }
    float rf = smoothstepf(0.3f, 2.5f, s) * (0.35f + 0.65f * lin) * k;
    if (layers & VGW_FLUTTER) {
        float fl = clamp01(v->col[0]) * wpo[2] * rf * v->pscale * sx;
        for (int i = 0; i < 3; i++) out[i] += v->n[i] * fl;
    }
    if (!n_out || !(layers & VGW_NORMALS)) return;
    float n[3] = { v->n[0], v->n[1], v->n[2] };
    if ((layers & VGW_MAIN) && bl > 1e-6f) {
        // the bent stem's slope 2 h |b|: rotate about cross(up, bend direction) (up tilts downwind)
        float th = atanf(2.0f * h * bl), c = cosf(th), sn = sinf(th);
        float kx = bvec[1] / bl, kz = -bvec[0] / bl;   // axis (k.y = 0)
        float kv = kx * n[0] + kz * n[2];
        float cx = -kz * n[1], cy = kz * n[0] - kx * n[2], cz = kx * n[1];   // k x n
        float r0 = n[0] * c + cx * sn + kx * kv * (1 - c), r1 = n[1] * c + cy * sn, r2 = n[2] * c + cz * sn + kz * kv * (1 - c);
        n[0] = r0, n[1] = r1, n[2] = r2;
    }
    if (layers & VGW_FLUTTER) {
        // the fluttering edge tilts the card: a quarter-period-shifted wave along the wind, tangent to n
        float tilt = clamp01(v->col[0]) * rf * fminf(wpo[2] * 10.0f, 1.0f) * 0.6f * w4[1];
        float dn = d[0] * n[0] + d[1] * n[2];
        n[0] += (d[0] - n[0] * dn) * tilt;
        n[1] += (-n[1] * dn) * tilt;
        n[2] += (d[1] - n[2] * dn) * tilt;
    }
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 1e-6f) for (int i = 0; i < 3; i++) n_out[i] = n[i] / l;
}
