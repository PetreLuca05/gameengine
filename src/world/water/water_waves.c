// Ocean swell: seeded Gerstner set + JONSWAP amplitudes (water_waves.h). Built with FP contraction
// off (CMakeLists.txt): keep every expression here free of libm calls other than sqrtf.
#include "water_waves.h"
#include <math.h>
#include <string.h>

#define W_PI 3.14159265358979f
#define W_TAU 6.28318530717959f

// ------------------------------------------------------------------ deterministic polynomials
// Cephes-style minimax on a Cody-Waite reduced argument (3-part pi/2: q * c1 is exact for |q| < 2^15).

// Nearest integer (ties to even) for |x| < 2^22: adding and removing 1.5 * 2^23 rounds in the FPU's
// default mode. Exact IEEE arithmetic (no libm, no SSE4.1 roundss needed), so deterministic everywhere.
static float round_near(float x) { return (x + 12582912.0f) - 12582912.0f; }

void wsincos(float x, float *s_out, float *c_out) {
    float q = round_near(x * 0.636619772f);
    float r = ((x - q * 1.5703125f) - q * 4.837512969970703125e-4f) - q * 7.54978995489188216e-8f;
    float z = r * r;
    float v[2];
    v[0] = r + r * z * (-1.6666654611e-1f + z * (8.3321608736e-3f + z * -1.9515295891e-4f));
    v[1] = 1.0f - 0.5f * z + z * z * (4.166664568298827e-2f + z * (-1.388731625493765e-3f + z * 2.443315711809948e-5f));
    // Quadrant without branches (random quadrants mispredict): sin = +s, +c, -s, -c; cos = +c, -s, -c, +s.
    int qi = (int)q & 3;
    static const float k_sign[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    *s_out = v[qi & 1] * k_sign[qi];
    *c_out = v[(qi + 1) & 1] * k_sign[(qi + 1) & 3];
}

float wsin(float x) { float s, c; wsincos(x, &s, &c); return s; }
float wcos(float x) { float s, c; wsincos(x, &s, &c); return c; }

float wexp(float x) {
    if (x > 88.0f) x = 88.0f;
    if (x < -87.0f) x = -87.0f;
    float n = round_near(x * 1.44269504f);
    float r = (x - n * 0.693359375f) - n * -2.12194440e-4f;
    float y = (((((1.9875691500e-4f * r + 1.3981999507e-3f) * r + 8.3334519073e-3f) * r + 4.1665795894e-2f) * r + 1.6666665459e-1f) * r
               + 5.0000001201e-1f) * r * r + r + 1.0f;
    union { u32 u; float f; } b;
    b.u = (u32)((int)n + 127) << 23;
    return y * b.f;
}

float wlog(float x) {
    if (!(x > 0.0f)) return -87.0f;
    union { u32 u; float f; } b;
    b.f = x;
    int e = (int)((b.u >> 23) & 255u) - 127;
    b.u = (b.u & 0x007FFFFFu) | 0x3F800000u;      // mantissa in [1, 2)
    float m = b.f;
    if (m > 1.41421356f) { m *= 0.5f; e++; }
    float f = m - 1.0f, z = f * f;
    float y = ((((((((7.0376836292e-2f * f - 1.1514610310e-1f) * f + 1.1676998740e-1f) * f - 1.2420140846e-1f) * f + 1.4249322787e-1f) * f
                  - 1.6668057665e-1f) * f + 2.0000714765e-1f) * f - 2.4999993993e-1f) * f + 3.3333331174e-1f) * f * z;
    float fe = (float)e;
    y += fe * -2.12194440e-4f;
    y += -0.5f * z;
    return f + y + fe * 0.693359375f;
}

static float wpow(float a, float b) { return a > 0.0f ? wexp(b * wlog(a)) : 0.0f; }

static u32 hash_u32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
static float rand01(u32 seed, u32 i) { return (float)(hash_u32(seed * 0x9E3779B9u + i * 0x85EBCA6Bu + 0x632BE5ABu) >> 8) * (1.0f / 16777216.0f); }

// ------------------------------------------------------------------ model

WaveParams wave_params_default(void) {
    WaveParams p;
    memset(&p, 0, sizeof(p));
    p.wind = 5.0f;
    p.fetch_km = 30.0f;
    p.scale = 1.0f;
    p.choppiness = 0.6f;
    p.spread = 6.0f;
    return p;
}

void waves_init(WaveModel *m, u32 seed) {
    memset(m, 0, sizeof(*m));
    m->seed = seed;
    const float quantum = W_TAU / (float)WAVE_REPEAT;
    float omega[WAVE_FREQS];
    float ln_ratio = wlog(WAVE_MIN_LEN / WAVE_MAX_LEN);
    for (int i = 0; i < WAVE_FREQS; i++) {
        float len = WAVE_MAX_LEN * wexp(ln_ratio * (float)i / (float)(WAVE_FREQS - 1));
        float w = sqrtf(WAVE_GRAVITY * W_TAU / len);
        float n = round_near(w / quantum);
        omega[i] = n * quantum;
    }
    for (int i = 0; i < WAVE_FREQS; i++) {
        float lo = i > 0 ? 0.5f * (omega[i] + omega[i - 1]) : omega[i] - 0.5f * (omega[i + 1] - omega[i]);
        float hi = i < WAVE_FREQS - 1 ? 0.5f * (omega[i] + omega[i + 1]) : omega[i] + 0.5f * (omega[i] - omega[i - 1]);
        float base = rand01(seed, (u32)i * 7u) * W_TAU;
        for (int j = 0; j < WAVE_DIRS; j++) {
            WaveComp *c = &m->comp[i * WAVE_DIRS + j];
            u32 r = (u32)(i * WAVE_DIRS + j) * 7u;
            float th = base + (float)j * (W_TAU / (float)WAVE_DIRS) + (rand01(seed, r + 1u) - 0.5f) * 0.6f;
            wsincos(th, &c->dx, &c->dz);
            c->dz = -c->dz;                 // heading convention: 0 = -Z, 90 deg = +X
            c->omega = omega[i];
            c->k = omega[i] * omega[i] / WAVE_GRAVITY;
            c->phase = rand01(seed, r + 2u) * W_TAU;
            c->domega = hi - lo;
        }
    }
}

void waves_set_params(WaveModel *m, const WaveParams *p_in) {
    WaveParams p = *p_in;
    if (p.fetch_km <= 0.0f) p.fetch_km = 30.0f;
    if (p.spread <= 0.0f) p.spread = 6.0f;
    if (p.choppiness < 0.0f) p.choppiness = 0.0f;
    if (m->params_valid && memcmp(&p, &m->p, sizeof(p)) == 0) return;
    m->p = p;
    m->params_valid = true;

    const float g = WAVE_GRAVITY;
    float U = p.wind > 0.5f ? p.wind : 0.5f;
    float F = p.fetch_km * 1000.0f;
    float wp = 22.0f * wexp(wlog(g * g / (U * F)) * (1.0f / 3.0f));
    float alpha = 0.076f * wpow(U * U / (F * g), 0.22f);
    const float ln_gamma = 1.19392247f;    // ln 3.3
    float th = p.wind_dir_deg * (W_PI / 180.0f);
    float wx, wz;
    wsincos(th, &wx, &wz);
    wz = -wz;

    float amp[WAVE_MAX];
    for (int i = 0; i < WAVE_FREQS; i++) {
        const WaveComp *c0 = &m->comp[i * WAVE_DIRS];
        float w = c0->omega;
        float sigma = w <= wp ? 0.07f : 0.09f;
        float dw = (w - wp) / (sigma * wp);
        float r = wexp(-0.5f * dw * dw);
        float q = wp / w, q2 = q * q;
        float w2 = w * w;
        float S = alpha * g * g / (w2 * w2 * w) * wexp(-1.25f * q2 * q2) * wexp(r * ln_gamma);
        float cw[WAVE_DIRS], sum = 0.0f;
        for (int j = 0; j < WAVE_DIRS; j++) {
            const WaveComp *c = &m->comp[i * WAVE_DIRS + j];
            float half_cos2 = 0.5f * (1.0f + c->dx * wx + c->dz * wz);   // cos^2 of half the angle to the wind
            cw[j] = half_cos2 > 1e-6f ? wpow(half_cos2, p.spread) : 0.0f;
            sum += cw[j];
        }
        for (int j = 0; j < WAVE_DIRS; j++) {
            float e = sum > 0.0f ? 2.0f * S * c0->domega * cw[j] / sum : 0.0f;
            amp[i * WAVE_DIRS + j] = (e > 0.0f ? sqrtf(e) : 0.0f) * p.scale;
        }
    }

    // Active set: the WAVE_ACTIVE largest (ties by index, so the order is deterministic).
    int idx[WAVE_MAX];
    for (int i = 0; i < WAVE_MAX; i++) idx[i] = i;
    for (int i = 1; i < WAVE_MAX; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && (amp[idx[j]] < amp[v] || (amp[idx[j]] == amp[v] && idx[j] > v))) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = v;
    }
    m->active_n = 0;
    float steep = 0.0f, e2 = 0.0f;
    for (int n = 0; n < WAVE_ACTIVE; n++) {
        int i = idx[n];
        if (amp[i] < 1e-4f) break;
        const WaveComp *c = &m->comp[i];
        WaveActive *a = &m->act[m->active_n++];
        a->dx = c->dx; a->dz = c->dz; a->k = c->k; a->omega = c->omega;
        a->amp = amp[i]; a->phase = c->phase; a->pad = 0.0f;
        steep += c->k * amp[i];
        e2 += amp[i] * amp[i];
    }
    float chop = p.choppiness;
    if (steep > 0.0f && chop * steep > 0.85f) chop = 0.85f / steep;
    for (int n = 0; n < m->active_n; n++) m->act[n].qa = chop * m->act[n].amp;
    m->steepness = chop * steep;
    m->hs = 4.0f * sqrtf(0.5f * e2);
    m->peak_len = W_TAU * g / (wp * wp);
}

float waves_time(u32 tick, float frac, int hz) {
    u32 period = (u32)WAVE_REPEAT * (u32)hz;
    return ((float)(tick % period) + frac) / (float)hz;
}

void waves_displace(const WaveModel *m, float x0, float z0, float t, float out[3]) {
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    for (int i = 0; i < m->active_n; i++) {
        const WaveActive *a = &m->act[i];
        float s, c;
        wsincos(a->k * (a->dx * x0 + a->dz * z0) - a->omega * t + a->phase, &s, &c);
        ox -= a->dx * a->qa * s;
        oy += a->amp * c;
        oz -= a->dz * a->qa * s;
    }
    out[0] = ox; out[1] = oy; out[2] = oz;
}

static void invert(const WaveModel *m, float x, float z, float t, int iters, float *x0, float *z0) {
    float px = x, pz = z, d[3];
    for (int i = 0; i < iters; i++) {
        waves_displace(m, px, pz, t, d);
        px = x - d[0];
        pz = z - d[2];
    }
    *x0 = px; *z0 = pz;
}

float waves_height(const WaveModel *m, float x, float z, float t, int iters) {
    if (m->active_n == 0) return 0.0f;
    float x0, z0, d[3];
    invert(m, x, z, t, iters, &x0, &z0);
    waves_displace(m, x0, z0, t, d);
    return d[1];
}

void waves_patch(const WaveModel *m, float x, float z, float t, float e, float px[5], float pz[5], float h[5], float vel[5][3]) {
    // One fixed-point step is enough here: px / pz come out as the true displaced positions, so the
    // patch only needs to be roughly centred on (x, z) (the plane fit is exact at its own points).
    float x0, z0;
    if (m->active_n) invert(m, x, z, t, 1, &x0, &z0);
    else { x0 = x; z0 = z; }
    const float ux[5] = { 0, 1, -1, 0, 0 }, uz[5] = { 0, 0, 0, 1, -1 };
    for (int j = 0; j < 5; j++) {
        px[j] = x0 + ux[j] * e;
        pz[j] = z0 + uz[j] * e;
        h[j] = 0.0f;
        vel[j][0] = vel[j][1] = vel[j][2] = 0.0f;
    }
    for (int i = 0; i < m->active_n; i++) {
        const WaveActive *a = &m->act[i];
        float S, C, sx, cx, sz, cz;
        wsincos(a->k * (a->dx * x0 + a->dz * z0) - a->omega * t + a->phase, &S, &C);
        wsincos(a->k * a->dx * e, &sx, &cx);
        wsincos(a->k * a->dz * e, &sz, &cz);
        float s[5] = { S, S * cx + C * sx, S * cx - C * sx, S * cz + C * sz, S * cz - C * sz };
        float c[5] = { C, C * cx - S * sx, C * cx + S * sx, C * cz - S * sz, C * cz + S * sz };
        for (int j = 0; j < 5; j++) {
            px[j] -= a->dx * a->qa * s[j];
            pz[j] -= a->dz * a->qa * s[j];
            h[j] += a->amp * c[j];
            float qwc = a->qa * a->omega * c[j];
            vel[j][0] += a->dx * qwc;
            vel[j][1] += a->amp * a->omega * s[j];
            vel[j][2] += a->dz * qwc;
        }
    }
}

void waves_sample(const WaveModel *m, float x, float z, float t, WaveSample *o) {
    memset(o, 0, sizeof(*o));
    o->normal[1] = 1.0f;
    o->jacobian = 1.0f;
    if (m->active_n == 0) return;
    float x0, z0;
    invert(m, x, z, t, 2, &x0, &z0);
    float h = 0.0f, vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float xx = 1.0f, xz = 0.0f, zz = 1.0f, yx = 0.0f, yz = 0.0f;   // dPx/dx0, dPx/dz0 (= dPz/dx0), dPz/dz0, dPy/dx0, dPy/dz0
    for (int i = 0; i < m->active_n; i++) {
        const WaveActive *a = &m->act[i];
        float s, c;
        wsincos(a->k * (a->dx * x0 + a->dz * z0) - a->omega * t + a->phase, &s, &c);
        float qkc = a->qa * a->k * c, aks = a->amp * a->k * s;
        h += a->amp * c;
        xx -= a->dx * a->dx * qkc;
        xz -= a->dx * a->dz * qkc;
        zz -= a->dz * a->dz * qkc;
        yx -= aks * a->dx;
        yz -= aks * a->dz;
        float qwc = a->qa * a->omega * c;
        vx += a->dx * qwc;
        vy += a->amp * a->omega * s;
        vz += a->dz * qwc;
    }
    // normal = cross(dP/dz0, dP/dx0)
    float nx = yz * xz - zz * yx;
    float ny = zz * xx - xz * xz;
    float nz = xz * yx - xx * yz;
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    if (l > 1e-8f) { nx /= l; ny /= l; nz /= l; }
    o->height = h;
    o->normal[0] = nx; o->normal[1] = ny; o->normal[2] = nz;
    o->vel[0] = vx; o->vel[1] = vy; o->vel[2] = vz;
    o->jacobian = xx * zz - xz * xz;
}
