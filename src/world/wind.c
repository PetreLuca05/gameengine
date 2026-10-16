#include "wind.h"
#include <math.h>
#include <string.h>

#define TAU 6.2831853f

static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static float smooth5(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static float smoothstepf(float a, float b, float x) {
    float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

// ------------------------------------------------------------------ noise (value noise, deterministic)

static u32 hash3i(i32 x, i32 y, i32 z, u32 seed) {
    u32 h = (u32)x * 0x8DA6B343u ^ (u32)y * 0xD8163841u ^ (u32)z * 0xCB1AB31Fu ^ seed * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
static float rnd3(i32 x, i32 y, i32 z, u32 seed) { return (float)(hash3i(x, y, z, seed) >> 8) * (1.0f / 16777216.0f); }

static float noise2(float x, float y, u32 seed) {
    float fx = floorf(x), fy = floorf(y);
    i32 ix = (i32)fx, iy = (i32)fy;
    float tx = smooth5(x - fx), ty = smooth5(y - fy);
    float a = rnd3(ix, iy, 0, seed), b = rnd3(ix + 1, iy, 0, seed), c = rnd3(ix, iy + 1, 0, seed), d = rnd3(ix + 1, iy + 1, 0, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

static float noise3(float x, float y, float z, u32 seed) {
    float fx = floorf(x), fy = floorf(y), fz = floorf(z);
    i32 ix = (i32)fx, iy = (i32)fy, iz = (i32)fz;
    float tx = smooth5(x - fx), ty = smooth5(y - fy), tz = smooth5(z - fz);
    float v[2];
    for (int k = 0; k < 2; k++) {
        float a = rnd3(ix, iy, iz + k, seed), b = rnd3(ix + 1, iy, iz + k, seed), c = rnd3(ix, iy + 1, iz + k, seed), d = rnd3(ix + 1, iy + 1, iz + k, seed);
        float ab = a + (b - a) * tx, cd = c + (d - c) * tx;
        v[k] = ab + (cd - ab) * ty;
    }
    return v[0] + (v[1] - v[0]) * tz;
}

// ------------------------------------------------------------------ half floats

u16 wind_f2h(float f) {
    union { float f; u32 u; } v = { f };
    u32 s = (v.u >> 16) & 0x8000u, e = (v.u >> 23) & 0xFFu, m = v.u & 0x7FFFFFu;
    if (e == 0xFFu) return (u16)(s | 0x7C00u | (m ? 0x200u : 0));
    int ee = (int)e - 127 + 15;
    if (ee >= 31) return (u16)(s | 0x7C00u);
    if (ee <= 0) {
        if (ee < -10) return (u16)s;
        m |= 0x800000u;
        u32 sh = (u32)(14 - ee), hm = m >> sh, rem = m & ((1u << sh) - 1u), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (hm & 1u))) hm++;
        return (u16)(s | hm);
    }
    u32 hm = m >> 13, rem = m & 0x1FFFu;
    u32 out = s | ((u32)ee << 10) | hm;
    if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) out++;   // round to nearest even (may carry into the exponent)
    return (u16)out;
}

float wind_h2f(u16 h) {
    u32 s = (u32)(h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    union { u32 u; float f; } v;
    if (e == 0) {
        if (!m) { v.u = s; return v.f; }
        float r = (float)m / 1024.0f / 16384.0f;
        return s ? -r : r;
    }
    if (e == 31) v.u = s | 0x7F800000u | (m << 13);
    else v.u = s | ((e - 15 + 127) << 23) | (m << 13);
    return v.f;
}

// ------------------------------------------------------------------ field

void wind_init(WindField *f, u32 seed) {
    memset(f, 0, sizeof(*f));
    f->seed = seed;
    f->dir[1] = -1.0f;
}

void wind_set(WindField *f, const WindBase *b, double time) {
    f->base = *b;
    if (f->base.speed < 0) f->base.speed = 0;
    f->base.gust = clamp01(f->base.gust);
    float a = b->dir_deg * (TAU / 360.0f);
    f->dir[0] = sinf(a);
    f->dir[1] = -cosf(a);
    f->time = time;
}

// Gust noise at pos: the coordinate along the wind moves with the wind (s - U t), so a crest travels
// at U; across it the fronts are wavy (2.5x longer features). 0..1.
static float gust_noise(const WindField *f, float x, float z) {
    float s = x * f->dir[0] + z * f->dir[1], l = -x * f->dir[1] + z * f->dir[0];
    float u = (s - f->base.speed * (float)fmod(f->time, 100000.0)) / WIND_GUST_WAVELENGTH, w = l / (WIND_GUST_WAVELENGTH * 2.5f);
    return 0.65f * noise2(u, w, f->seed) + 0.35f * noise2(u * 2.3f + 17.1f, w * 2.3f, f->seed + 1);
}

static float gust_factor(float gust, float n) {
    float g = 0.15f + 0.85f * gust;      // calm weather keeps light gusts
    float crest = smoothstepf(0.5f, 0.85f, n), lull = smoothstepf(0.4f, 0.12f, n);
    return fmaxf(0.3f, 1.0f + g * (1.05f * crest - 0.65f * lull + 0.2f * (n - 0.5f)));
}

float wind_gust_at(const WindField *f, const float p[3]) { return gust_factor(f->base.gust, gust_noise(f, p[0], p[2])); }

float wind_gust_front(const WindField *f, const float p[3], float *factor) {
    // March upwind (the front arrives from there) in 2 m steps to the first local maximum above the
    // crest threshold: its distance is how far the gust front is from pos.
    float best = 0, bd = 0;
    float prev = gust_noise(f, p[0], p[2]);
    bool rising = false;
    for (int i = 1; i <= 120; i++) {
        float d = (float)i * 2.0f;
        float n = gust_noise(f, p[0] - f->dir[0] * d, p[2] - f->dir[1] * d);
        if (n > prev) rising = true;
        else if (rising && prev > 0.6f) { best = prev; bd = d - 2.0f; break; }
        prev = n;
    }
    if (factor) *factor = best > 0 ? gust_factor(f->base.gust, best) : 1.0f;
    return bd;
}

static void sources_at(const WindField *f, const float p[3], float out[3]) {
    for (int i = 0; i < WIND_SOURCES; i++) {
        const WindSource *s = &f->src[i];
        if (!s->used) continue;
        float dx = p[0] - s->pos[0], dz = p[2] - s->pos[2], d = sqrtf(dx * dx + dz * dz);
        float t = clamp01(s->age / fmaxf(s->life, 1e-3f));
        if (s->kind == WIND_SRC_BLAST) {
            // ring expanding at 60 m/s to the radius, decaying quadratically over the life
            float ring = fminf(s->age * 60.0f, s->radius), w = (d - ring) / fmaxf(s->radius * 0.18f, 1.5f);
            float k = s->strength * expf(-w * w) * (1.0f - t) * (1.0f - t) * (d < s->radius * 1.3f ? 1.0f : 0.0f);
            if (d > 1e-3f) { out[0] += dx / d * k; out[2] += dz / d * k; }
            out[1] += 0.25f * k;
        } else if (s->kind == WIND_SRC_DOWNWASH) {
            if (d < s->radius) {
                float k = s->strength * (1.0f - d / s->radius) * smoothstepf(0.0f, 0.1f, 1.0f - t);
                if (d > 1e-3f) { out[0] += dx / d * k; out[2] += dz / d * k; }
                out[1] -= 0.5f * k;
            }
        } else if (s->kind == WIND_SRC_GUST) {
            // a band across dir whose centre travels at the front speed, fading in / out over its life
            float sp = fmaxf(s->strength / 1.5f, 6.0f), front = s->age * sp;
            float a = (dx * s->dir[0] + dz * s->dir[1]) - front;
            float k = s->strength * expf(-(a * a) / (s->radius * s->radius)) * smoothstepf(0.0f, 0.08f, t) * smoothstepf(1.0f, 0.85f, t);
            out[0] += s->dir[0] * k;
            out[2] += s->dir[1] * k;
        } else {
            // wake: a 3-radius-long tail behind pos along -dir, Gaussian across it
            float a = -(dx * s->dir[0] + dz * s->dir[1]), b = -dx * s->dir[1] + dz * s->dir[0];
            if (a > -s->radius * 0.5f && a < s->radius * 3.0f) {
                float k = s->strength * (1.0f - clamp01(a / (s->radius * 3.0f))) * expf(-(b * b) / (s->radius * s->radius)) * (1.0f - t);
                out[0] += s->dir[0] * k;
                out[2] += s->dir[1] * k;
            }
        }
    }
}

void wind_at(const WindField *f, const float p[3], float out[3]) {
    float U = f->base.speed, g = f->base.gust, tt = (float)fmod(f->time, 100000.0);
    float gf = wind_gust_at(f, p);
    // turbulence, advected with the mean wind (features drift downwind), slowly evolving
    float qx = (p[0] - f->dir[0] * U * tt) / WIND_TURB_SCALE, qz = (p[2] - f->dir[1] * U * tt) / WIND_TURB_SCALE, qy = p[1] / WIND_TURB_SCALE + tt * 0.07f;
    float na = noise3(qx, qy, qz, f->seed + 7), nb = noise3(qx + 31.7f, qy, qz - 13.1f, f->seed + 8), nc = noise3(qx - 5.3f, qy + 9.9f, qz + 3.7f, f->seed + 9);
    float ang = (na - 0.5f) * 2.0f * (0.22f + 0.33f * g), ca = cosf(ang), sa = sinf(ang);
    float mag = U * gf * (1.0f + (nb - 0.5f) * 0.3f * (0.5f + g));
    out[0] = (f->dir[0] * ca - f->dir[1] * sa) * mag;
    out[2] = (f->dir[0] * sa + f->dir[1] * ca) * mag;
    out[1] = (nc - 0.5f) * 0.15f * U;
    sources_at(f, p, out);
    float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (l > WIND_MAX_SPEED) for (int k = 0; k < 3; k++) out[k] *= WIND_MAX_SPEED / l;
}

// ------------------------------------------------------------------ sources

static WindSource *src_alloc(WindField *f) {
    int oldest = 0;
    for (int i = 0; i < WIND_SOURCES; i++) {
        if (!f->src[i].used) return &f->src[i];
        if (f->src[i].age / fmaxf(f->src[i].life, 1e-3f) > f->src[oldest].age / fmaxf(f->src[oldest].life, 1e-3f)) oldest = i;
    }
    return &f->src[oldest];   // full: replace the most finished one
}

void wind_add_blast(WindField *f, const float pos[3], float radius, float strength) {
    WindSource *s = src_alloc(f);
    memset(s, 0, sizeof(*s));
    memcpy(s->pos, pos, 12);
    s->radius = radius;
    s->strength = strength;
    s->life = 0.4f + radius / 60.0f + 0.8f;
    s->kind = WIND_SRC_BLAST;
    s->used = 1;
}

void wind_add_downwash(WindField *f, const float pos[3], float radius, float strength, float life) {
    WindSource *s = src_alloc(f);
    memset(s, 0, sizeof(*s));
    memcpy(s->pos, pos, 12);
    s->radius = radius;
    s->strength = strength;
    s->life = life;
    s->kind = WIND_SRC_DOWNWASH;
    s->used = 1;
}

void wind_add_wake(WindField *f, const float pos[3], const float vel[3], float radius) {
    float sp = sqrtf(vel[0] * vel[0] + vel[2] * vel[2]);
    if (sp < 2.0f) return;
    WindSource *s = src_alloc(f);
    memset(s, 0, sizeof(*s));
    memcpy(s->pos, pos, 12);
    s->dir[0] = vel[0] / sp;
    s->dir[1] = vel[2] / sp;
    s->radius = radius;
    s->strength = fminf(sp * 0.5f, 15.0f);
    s->life = 0.25f;   // vehicles re-add it every few frames while moving
    s->kind = WIND_SRC_WAKE;
    s->used = 1;
}

void wind_add_gust(WindField *f, const float pos[3], float strength, float width, float range) {
    WindSource *s = src_alloc(f);
    memset(s, 0, sizeof(*s));
    memcpy(s->pos, pos, 12);
    s->dir[0] = f->dir[0];
    s->dir[1] = f->dir[1];
    s->radius = fmaxf(width * 0.5f, 2.0f);
    s->strength = strength;
    s->life = range / fmaxf(strength / 1.5f, 6.0f);
    s->kind = WIND_SRC_GUST;
    s->used = 1;
}

void wind_sources_step(WindField *f, float dt) {
    for (int i = 0; i < WIND_SOURCES; i++) {
        WindSource *s = &f->src[i];
        if (!s->used) continue;
        s->age += dt;
        if (s->age >= s->life) s->used = 0;
    }
}

// ------------------------------------------------------------------ render tile

static int wrap(i32 v) { return (int)(((v % WIND_TEX) + WIND_TEX) % WIND_TEX); }

// One damped-spring step towards target (semi-implicit Euler, sub-stepped for large dt).
static void spring(float x[2], float v[2], const float target[2], float dt) {
    const float w0 = TAU / WIND_SETTLE_PERIOD, z = WIND_SETTLE_DAMPING;
    int n = (int)ceilf(dt / (1.0f / 60.0f));
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    float h = dt / (float)n;
    for (int s = 0; s < n; s++)
        for (int k = 0; k < 2; k++) {
            v[k] += (w0 * w0 * (target[k] - x[k]) - 2.0f * z * w0 * v[k]) * h;
            x[k] += v[k] * h;
        }
}

void wind_tile_update(WindField *f, const float cam[3], float dt) {
    if (dt < 0) dt = 0;
    if (dt > 0.25f) dt = 0.25f;
    i32 ox = (i32)floorf(cam[0] / WIND_TEXEL) - WIND_TEX / 2, oz = (i32)floorf(cam[2] / WIND_TEXEL) - WIND_TEX / 2;
    bool moved = !f->tile_valid || ox != f->ox || oz != f->oz;
    float gtarget[2] = { f->dir[0] * f->base.speed, f->dir[1] * f->base.speed };
    if (!f->tile_valid) { memcpy(f->global_bend, gtarget, 8); memset(f->global_bend_v, 0, 8); }
    spring(f->global_bend, f->global_bend_v, gtarget, dt);
    for (int j = 0; j < WIND_TEX; j++)
        for (int i = 0; i < WIND_TEX; i++) {
            i32 wx = ox + i, wz = oz + j;
            int slot = wrap(wz) * WIND_TEX + wrap(wx);
            float p[3] = { ((float)wx + 0.5f) * WIND_TEXEL, cam[1], ((float)wz + 0.5f) * WIND_TEXEL }, w[3];
            wind_at(f, p, w);
            f->inst[slot][0] = w[0];
            f->inst[slot][1] = w[2];
            // a texel entering the window (it held another world cell) starts settled at its wind
            bool fresh = !f->tile_valid || (moved && (wx < f->ox || wx >= f->ox + WIND_TEX || wz < f->oz || wz >= f->oz + WIND_TEX));
            if (fresh) {
                f->bend[slot][0] = w[0];
                f->bend[slot][1] = w[2];
                f->bend_v[slot][0] = f->bend_v[slot][1] = 0;
            } else spring(f->bend[slot], f->bend_v[slot], f->inst[slot], dt);
            u16 *t = &f->texels[slot * 4];
            t[0] = wind_f2h(f->inst[slot][0]);
            t[1] = wind_f2h(f->inst[slot][1]);
            t[2] = wind_f2h(f->bend[slot][0]);
            t[3] = wind_f2h(f->bend[slot][1]);
        }
    f->ox = ox;
    f->oz = oz;
    f->tile_valid = true;
}

bool wind_tile_sample(const WindField *f, float x, float z, float out[4]) {
    // Global fallback: the mean wind and its spring (the shader's view.wind).
    float g[4] = { f->dir[0] * f->base.speed, f->dir[1] * f->base.speed, f->global_bend[0], f->global_bend[1] };
    memcpy(out, g, 16);
    if (!f->tile_valid) return false;
    float lo_x = ((float)f->ox + 0.5f) * WIND_TEXEL, lo_z = ((float)f->oz + 0.5f) * WIND_TEXEL;
    float hi_x = lo_x + (float)(WIND_TEX - 1) * WIND_TEXEL, hi_z = lo_z + (float)(WIND_TEX - 1) * WIND_TEXEL;
    float e = fminf(fminf(x - lo_x, hi_x - x), fminf(z - lo_z, hi_z - z));
    float w = clamp01(e / (2.0f * WIND_TEXEL));
    if (w <= 0) return false;
    // bilinear between texel centres, toroidal (the shader: REPEAT addressing of uv = world / 256 m)
    float u = x / WIND_TEXEL - 0.5f, v = z / WIND_TEXEL - 0.5f, fu = floorf(u), fv = floorf(v);
    float tu = u - fu, tv = v - fv;
    i32 iu = (i32)fu, iv = (i32)fv;
    float s[4] = { 0, 0, 0, 0 };
    for (int c = 0; c < 4; c++) {
        i32 cx = iu + (c & 1), cz = iv + (c >> 1);
        float k = ((c & 1) ? tu : 1.0f - tu) * ((c >> 1) ? tv : 1.0f - tv);
        const u16 *t = &f->texels[(wrap(cz) * WIND_TEX + wrap(cx)) * 4];
        for (int ch = 0; ch < 4; ch++) s[ch] += wind_h2f(t[ch]) * k;
    }
    for (int ch = 0; ch < 4; ch++) out[ch] = g[ch] + (s[ch] - g[ch]) * w;
    return true;
}
