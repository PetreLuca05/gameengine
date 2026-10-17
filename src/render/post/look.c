// Look grading stack (docs/LOOKS.md): the analytic evaluation baked into each look's LUT, the
// finish / fidelity caps, the modulators and the dev tuning reflection. Pure C (tests/test_looks.c).
#include "look.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static float luma3(vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
static vec3 vmax0(vec3 c) { return v3(fmaxf(c.x, 0.0f), fmaxf(c.y, 0.0f), fmaxf(c.z, 0.0f)); }
static vec3 vclamp01(vec3 c) { return v3(saturate(c.x), saturate(c.y), saturate(c.z)); }
// GLSL mat3(a0..a8) (columns) times v.
static vec3 mul_cols(const float m[9], vec3 v) {
    return v3(m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z, m[2] * v.x + m[5] * v.y + m[8] * v.z);
}
// GLSL v * mat3(a0..a8): dot with each column.
static vec3 mul_row(vec3 v, const float m[9]) {
    return v3(v.x * m[0] + v.y * m[1] + v.z * m[2], v.x * m[3] + v.y * m[4] + v.z * m[5], v.x * m[6] + v.y * m[7] + v.z * m[8]);
}
static float srgb_enc(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }
static float srgb_dec(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// ---------------------------------------------------------------- tonemappers (shaders/post/tonemap.comp)

static vec3 aces_fit(vec3 v) {
    static const float in[9] = { 0.59719f, 0.07600f, 0.02840f, 0.35458f, 0.90834f, 0.13383f, 0.04823f, 0.01566f, 0.83777f };
    static const float out[9] = { 1.60475f, -0.10208f, -0.00327f, -0.53108f, 1.10813f, -0.07276f, -0.07367f, -0.00605f, 1.07602f };
    v = mul_cols(in, v);
    vec3 r;
    float *pv = &v.x, *pr = &r.x;
    for (int i = 0; i < 3; i++) {
        float a = pv[i] * (pv[i] + 0.0245786f) - 0.000090537f;
        float b = pv[i] * (0.983729f * pv[i] + 0.4329510f) + 0.238081f;
        pr[i] = a / b;
    }
    return vclamp01(mul_cols(out, r));
}

static vec3 agx(vec3 v) {
    static const float in[9] = { 0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f, 0.0784335999999992f, 0.878468636469772f,
                                 0.0784336f, 0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f };
    static const float out[9] = { 1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f, -0.0980208811401368f, 1.15190312990417f,
                                  -0.0980434501171241f, -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f };
    const float lo = -12.47393f, hi = 4.026069f;
    v = mul_cols(in, v3(fmaxf(v.x, 1e-10f), fmaxf(v.y, 1e-10f), fmaxf(v.z, 1e-10f)));
    float *p = &v.x;
    for (int i = 0; i < 3; i++) {
        float x = (clampf(log2f(fmaxf(p[i], 1e-10f)), lo, hi) - lo) / (hi - lo);
        float x2 = x * x, x4 = x2 * x2;
        p[i] = 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    }
    v = vmax0(mul_cols(out, v));
    return vclamp01(v3(powf(v.x, 2.2f), powf(v.y, 2.2f), powf(v.z, 2.2f)));
}

static vec3 pbr_neutral(vec3 c) {
    const float start = 0.8f - 0.04f, desat = 0.15f;
    float x = fminf(c.x, fminf(c.y, c.z));
    float off = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c = v3(c.x - off, c.y - off, c.z - off);
    float peak = fmaxf(c.x, fmaxf(c.y, c.z));
    if (peak < start) return vclamp01(c);
    const float d = 1.0f - start;
    float np = 1.0f - d * d / (peak + d - start);
    c = v3_scale(c, np / peak);
    float g = 1.0f - 1.0f / (desat * (peak - np) + 1.0f);
    return vclamp01(v3_lerp(c, v3(np, np, np), g));
}

// Hable / Uncharted 2 filmic with the toe (D) and shoulder (A) strengths exposed.
static float hable(float x, float a, float d) {
    const float b = 0.50f, c = 0.10f, e = 0.02f, f = 0.30f;
    return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}
static vec3 filmic(vec3 v, float toe, float shoulder) {
    float a = shoulder > 0 ? shoulder : 0.15f, d = toe > 0 ? toe : 0.20f;
    const float white = 11.2f;
    float w = 1.0f / hable(white, a, d);
    return vclamp01(v3(hable(v.x * 2.0f, a, d) * w, hable(v.y * 2.0f, a, d) * w, hable(v.z * 2.0f, a, d) * w));
}

vec3 look_tonemap(int tm, float toe, float shoulder, vec3 c) {
    c = vmax0(c);
    switch (tm) {
    case LOOK_TM_ACES: return aces_fit(c);
    case LOOK_TM_NEUTRAL: return pbr_neutral(c);
    case LOOK_TM_FILMIC: return filmic(c, toe, shoulder);
    default: return agx(c);
    }
}

// ---------------------------------------------------------------- colour helpers

static vec3 white_balance(vec3 c, float temp, float tint) {
    if (fabsf(temp) < 1e-4f && fabsf(tint) < 1e-4f) return c;
    float t1 = temp * 10.0f / 6.0f, t2 = tint * 10.0f / 6.0f;
    float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    float y = 2.87f * x - 3.0f * x * x - 0.27509507f + t2 * 0.05f;
    vec3 xyz = v3(x / y, 1.0f, (1.0f - x - y) / y);
    vec3 w2 = v3(0.7328f * xyz.x + 0.4296f * xyz.y - 0.1624f * xyz.z, -0.7036f * xyz.x + 1.6975f * xyz.y + 0.0061f * xyz.z,
                 0.0030f * xyz.x + 0.0136f * xyz.y + 0.9834f * xyz.z);
    vec3 bal = v3(0.949237f / w2.x, 1.03542f / w2.y, 1.08728f / w2.z);
    static const float to_lms[9] = { 3.90405e-1f, 5.49941e-1f, 8.92632e-3f, 7.08416e-2f, 9.63172e-1f, 1.35775e-3f, 2.31082e-2f, 1.28021e-1f, 9.36245e-1f };
    static const float from_lms[9] = { 2.85847e+0f, -1.62879e+0f, -2.48910e-2f, -2.10182e-1f, 1.15820e+0f, 3.24281e-4f, -4.18120e-2f, -1.18169e-1f, 1.06867e+0f };
    vec3 lms = mul_row(c, to_lms);
    lms = v3(lms.x * bal.x, lms.y * bal.y, lms.z * bal.z);
    return vmax0(mul_row(lms, from_lms));
}

float look_hue_deg(vec3 c) {
    float a = c.x - 0.5f * (c.y + c.z), b = 0.8660254f * (c.y - c.z);
    if (fabsf(a) < 1e-12f && fabsf(b) < 1e-12f) return 0.0f;
    float h = atan2f(b, a) * DG_RAD2DEG;
    return h < 0 ? h + 360.0f : h;
}

float look_saturation(vec3 c) {
    float mx = fmaxf(c.x, fmaxf(c.y, c.z)), mn = fminf(c.x, fminf(c.y, c.z));
    return mx > 1e-10f ? (mx - mn) / mx : 0.0f;
}

static vec3 hsv_rgb(float h_deg) {   // full saturation / value
    float h = fmodf(h_deg, 360.0f) / 60.0f;
    if (h < 0) h += 6.0f;
    float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    if (h < 1) return v3(1, x, 0);
    if (h < 2) return v3(x, 1, 0);
    if (h < 3) return v3(0, 1, x);
    if (h < 4) return v3(0, x, 1);
    if (h < 5) return v3(x, 0, 1);
    return v3(1, 0, x);
}

// Luma-neutral chroma direction of a hue (split-toning).
static vec3 hue_tint(float h_deg) {
    vec3 c = hsv_rgb(h_deg);
    float y = luma3(c);
    return v3(c.x - y, c.y - y, c.z - y);
}

// Periodic Catmull-Rom over the hue knots.
static float hue_curve(const float k[LOOK_HUE_KNOTS], float h_deg) {
    float t = h_deg / (360.0f / LOOK_HUE_KNOTS);
    int i = (int)floorf(t);
    float f = t - (float)i;
    float p0 = k[(i - 1 + 2 * LOOK_HUE_KNOTS) % LOOK_HUE_KNOTS], p1 = k[(i + LOOK_HUE_KNOTS) % LOOK_HUE_KNOTS];
    float p2 = k[(i + 1) % LOOK_HUE_KNOTS], p3 = k[(i + 2) % LOOK_HUE_KNOTS];
    float f2 = f * f, f3 = f2 * f;
    return 0.5f * (2.0f * p1 + (-p0 + p2) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
}

static float sat_curve(const float k[LOOK_SAT_KNOTS], float s) {
    float t = saturate(s) * (LOOK_SAT_KNOTS - 1);
    int i = DG_MIN((int)t, LOOK_SAT_KNOTS - 2);
    return lerpf(k[i], k[i + 1], t - (float)i);
}

// Rotation about the grey axis (Rodrigues, k = (1,1,1)/sqrt 3).
static vec3 hue_rotate(vec3 c, float deg) {
    if (fabsf(deg) < 1e-4f) return c;
    float a = deg * DG_DEG2RAD, cs = cosf(a), sn = sinf(a);
    const float k = 0.57735027f;
    vec3 kxv = v3(k * (c.z - c.y), k * (c.x - c.z), k * (c.y - c.x));   // k x c
    float kd = k * (c.x + c.y + c.z);
    return v3(c.x * cs + kxv.x * sn + k * kd * (1 - cs), c.y * cs + kxv.y * sn + k * kd * (1 - cs), c.z * cs + kxv.z * sn + k * kd * (1 - cs));
}

// Chroma around the luminance. A boost (k > 1) approaches the gamut edge (the lowest channel at 0)
// smoothly instead of clipping there: no kink for the LUT to miss.
static vec3 chroma_scale(vec3 c, float y, float k) {
    float mn = fminf(c.x, fminf(c.y, c.z));
    if (k > 1.0f && mn < y) {
        float kmax = y / (y - mn);   // the boost that brings the lowest channel to 0
        if (kmax > 1.0f) k = 1.0f + (kmax - 1.0f) * tanhf((k - 1.0f) / (kmax - 1.0f));
        else k = 1.0f;
    }
    return vmax0(v3(y + (c.x - y) * k, y + (c.y - y) * k, y + (c.z - y) * k));
}

// Procedural creative LUTs (display-referred, on sRGB-encoded values).
static vec3 creative_lut(int kind, vec3 o) {
    vec3 e = v3(srgb_enc(saturate(o.x)), srgb_enc(saturate(o.y)), srgb_enc(saturate(o.z)));
    float *pe = &e.x;
    if (kind == LOOK_LUT_PRINT) {
        // Print film: a soft S-curve, warm highlights / cool shadows, highlights slightly desaturated.
        for (int i = 0; i < 3; i++) pe[i] += 0.55f * (smoothstepf(0.0f, 1.0f, pe[i]) - pe[i]);
        float l = luma3(e);
        e = v3_add(e, v3_scale(v3(0.025f, 0.004f, -0.03f), (l - 0.42f) * 2.0f));
        e = v3_lerp(e, v3(l, l, l), 0.18f * smoothstepf(0.65f, 1.0f, l));
    } else if (kind == LOOK_LUT_BLEACH) {
        // Bleach bypass: the luminance overlaid on the colour (silver retained).
        float l = luma3(e);
        for (int i = 0; i < 3; i++) pe[i] = l < 0.5f ? 2.0f * pe[i] * l : 1.0f - 2.0f * (1.0f - pe[i]) * (1.0f - l);
    }
    e = vclamp01(e);
    return v3(srgb_dec(e.x), srgb_dec(e.y), srgb_dec(e.z));
}

// ---------------------------------------------------------------- the stack

vec3 look_eval(const LookParams *p, vec3 c) {
    c = vmax0(c);
    // 1. exposure, white balance
    c = v3_scale(c, exp2f(p->exposure_ev));
    c = white_balance(c, p->wb_temp, p->wb_tint);
    // 2. tonal, log2 around mid grey
    float y0 = fmaxf(luma3(c), 1e-8f);
    float ly = p->pivot_ev + (log2f(y0 / 0.18f) - p->pivot_ev) * p->contrast;
    float soft = fmaxf(p->range_soft, 0.05f);
    float ms = 1.0f - smoothstepf(p->shadow_end - soft, p->shadow_end, ly);
    float mh = smoothstepf(p->highlight_start, p->highlight_start + soft, ly);
    float mm = fmaxf(0.0f, 1.0f - ms - mh);
    {
        float *pc = &c.x;
        const float *lf = &p->lift.x, *gm = &p->gamma.x, *gn = &p->gain.x;
        for (int i = 0; i < 3; i++) {
            float l = p->pivot_ev + (log2f(fmaxf(pc[i], 1e-8f) / 0.18f) - p->pivot_ev) * p->contrast;
            l += lf[i] * ms + gm[i] * mm + gn[i] * mh;
            pc[i] = 0.18f * exp2f(l);
        }
    }
    if (p->rolloff > 0.0f) {
        float y = luma3(c), y1 = 0.18f * exp2f(p->rolloff_start);
        if (y > y1) c = v3_scale(c, (y1 + (y - y1) / (1.0f + p->rolloff * (y - y1) / y1)) / y);
    }
    // 3. colour
    float y = luma3(c);
    c = chroma_scale(c, y, p->saturation);
    float h = look_hue_deg(c), s = look_saturation(c);
    if (fabsf(p->vibrance) > 1e-5f) {
        float dh = fabsf(fmodf(h - 30.0f + 540.0f, 360.0f) - 180.0f);   // distance to skin hue (30 deg)
        float skin = expf(-(dh / 25.0f) * (dh / 25.0f));
        float amount = p->vibrance * (p->vibrance > 0 ? 1.0f - s : s) * (1.0f - p->skin_protect * skin);
        c = chroma_scale(c, y, 1.0f + amount);
    }
    c = vmax0(hue_rotate(c, hue_curve(p->hue_hue, h)));
    y = luma3(c);
    c = chroma_scale(c, y, fmaxf(hue_curve(p->hue_sat, h), 0.0f) * sat_curve(p->sat_sat, s));
    vec3 b = v3_add(v3_add(v3_scale(p->bal_shadows, ms), v3_scale(p->bal_mids, mm)), v3_scale(p->bal_highs, mh));
    float bl = luma3(b);
    c = vmax0(v3_add(c, v3_scale(v3(b.x - bl, b.y - bl, b.z - bl), y)));
    if (p->split_shadow_amt > 0 || p->split_high_amt > 0) {
        float s_sh = 1.0f - smoothstepf(p->split_balance - 2.0f, p->split_balance + 1.0f, ly);
        float s_hi = smoothstepf(p->split_balance - 1.0f, p->split_balance + 2.0f, ly);
        vec3 t = v3_add(v3_scale(hue_tint(p->split_shadow_hue), p->split_shadow_amt * s_sh), v3_scale(hue_tint(p->split_high_hue), p->split_high_amt * s_hi));
        c = vmax0(v3_add(c, v3_scale(t, y)));
    }
    // 4. tonemapper
    vec3 o = look_tonemap(p->tonemapper, p->tm_toe, p->tm_shoulder, c);
    // 5. creative LUT
    if (p->lut != LOOK_LUT_NONE && p->lut_strength > 0) o = v3_lerp(o, creative_lut(p->lut, o), saturate(p->lut_strength));
    // 6. output levels
    vec3 bp = v3_scale(p->black_tint, p->black_point);
    float wp = p->white_point > 0 ? p->white_point : 1.0f;
    return vclamp01(v3(bp.x + o.x * (wp - bp.x), bp.y + o.y * (wp - bp.y), bp.z + o.z * (wp - bp.z)));
}

// ---------------------------------------------------------------- LUT

static float shaper_inv(float s) { return 0.18f * exp2f(LOOK_EV_MIN + s * (LOOK_EV_MAX - LOOK_EV_MIN)); }

vec3 look_lut_coord(vec3 c) {
    const float k = 1.0f / (LOOK_EV_MAX - LOOK_EV_MIN);
    return v3(saturate((log2f(fmaxf(c.x, 1e-10f) / 0.18f) - LOOK_EV_MIN) * k), saturate((log2f(fmaxf(c.y, 1e-10f) / 0.18f) - LOOK_EV_MIN) * k),
              saturate((log2f(fmaxf(c.z, 1e-10f) / 0.18f) - LOOK_EV_MIN) * k));
}

void look_bake_lut(const LookParams *p, float *rgb) {
    const int n = LOOK_LUT_N;
    for (int bi = 0; bi < n; bi++)
        for (int gi = 0; gi < n; gi++)
            for (int ri = 0; ri < n; ri++) {
                vec3 in = v3(shaper_inv((float)ri / (n - 1)), shaper_inv((float)gi / (n - 1)), shaper_inv((float)bi / (n - 1)));
                vec3 o = look_eval(p, in);
                float *d = rgb + 3 * ((bi * n + gi) * n + ri);
                d[0] = o.x;
                d[1] = o.y;
                d[2] = o.z;
            }
}

vec3 look_lut_sample(const float *rgb, vec3 c) {
    const int n = LOOK_LUT_N;
    vec3 s = look_lut_coord(c);
    float f[3] = { s.x * (n - 1), s.y * (n - 1), s.z * (n - 1) };
    int i0[3], i1[3];
    float t[3];
    for (int k = 0; k < 3; k++) {
        i0[k] = DG_MIN((int)f[k], n - 1);
        i1[k] = DG_MIN(i0[k] + 1, n - 1);
        t[k] = f[k] - (float)i0[k];
    }
    vec3 o = v3(0, 0, 0);
    for (int corner = 0; corner < 8; corner++) {
        int r = corner & 1 ? i1[0] : i0[0], g = corner & 2 ? i1[1] : i0[1], b = corner & 4 ? i1[2] : i0[2];
        float w = (corner & 1 ? t[0] : 1 - t[0]) * (corner & 2 ? t[1] : 1 - t[1]) * (corner & 4 ? t[2] : 1 - t[2]);
        const float *d = rgb + 3 * ((b * n + g) * n + r);
        o = v3_add(o, v3(d[0] * w, d[1] * w, d[2] * w));
    }
    return o;
}

// ---------------------------------------------------------------- finish / fidelity caps

// Engine Fidelity bounds of the finish (docs/LOOKS.md "Fidelity"). Off drops every finish effect
// (the LUT grade stays, in final_direct.frag); Low / Medium cap the costly or noisy ones.
static const LookCaps k_caps[5] = {
    /* Off    */ { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0 },
    /* Low    */ { 0.04f, 0.03f, 0.0f, 0.0f, 1.0f, 1.0f, 1 },
    /* Medium */ { 0.06f, 0.08f, 0.0f, 0.2f, 1.0f, 1.0f, 1 },
    /* High   */ { 1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1 },
    /* Modern */ { 1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 1 },
};

const LookCaps *look_caps(int fid_level) { return &k_caps[DG_CLAMP(fid_level, 0, 4)]; }

void look_finish(const LookParams *p, const LookUser *u, int fid_level, LookFinish *out) {
    const LookCaps *k = look_caps(fid_level);
    memset(out, 0, sizeof(*out));
    out->bloom = fminf(p->bloom * clampf(u->bloom, 0, 4), k->bloom);
    out->bloom_threshold = p->bloom_threshold;
    out->bloom_tint = p->bloom_tint;
    out->vignette = fminf(p->vignette * clampf(u->vignette, 0, 4), k->vignette);
    out->grain = fminf(p->grain * clampf(u->grain, 0, 4), k->grain);
    out->chromatic = fminf(p->chromatic * clampf(u->chromatic, 0, 4), k->chromatic);
    out->sharpen = fminf(p->sharpen * clampf(u->sharpen, 0, 4), k->sharpen);
    out->halation = u->bloom > 0 ? fminf(p->halation, k->halation) : 0.0f;   // rides the bloom chain
    out->letterbox = p->letterbox && k->letterbox;
}

void look_apply_post(PostSettings *s, int id, const LookParams *p, const LookUser *u, int fid_level) {
    LookFinish f;
    look_finish(p, u, fid_level, &f);
    s->look_lut = id;
    s->bloom_intensity = f.bloom;
    s->bloom_threshold = f.bloom_threshold;
    s->bloom_tint = f.bloom_tint;
    s->vignette = f.vignette;
    s->grain = f.grain;
    s->chromatic = f.chromatic;
    s->sharpen = f.sharpen;
    s->halation = f.halation;
    s->letterbox = f.letterbox ? 2.39f : 0.0f;
    s->fb_damage_color = p->damage_color;
    s->fb_lowhealth_color = p->lowhealth_color;
}

// ---------------------------------------------------------------- modulators

static void mod_add(LookMod *m, const LookModSet *s, float w) {
    if (w <= 0.0f) return;
    m->temp += s->temp * w;
    m->tint += s->tint * w;
    m->exposure += s->exposure * w;
    m->sat_mul *= 1.0f + s->sat * w;
    m->contrast_mul *= 1.0f + s->contrast * w;
    m->bloom_mul *= 1.0f + s->bloom * w;
    m->grain_mul *= 1.0f + s->grain * w;
    m->vignette_add += s->vignette * w;
}

void look_modulate(const LookParams *p, const LookEnv *e, LookMod *m) {
    memset(m, 0, sizeof(*m));
    m->sat_mul = m->contrast_mul = m->bloom_mul = m->grain_mul = 1.0f;
    float overcast = smoothstepf(0.45f, 0.95f, e->cloud);
    float storm = saturate(fmaxf(e->lightning, smoothstepf(0.35f, 0.9f, e->precip) * (0.5f + 0.5f * smoothstepf(4.0f, 12.0f, e->wind))));
    // Golden hour: the sun from just under the horizon to ~18 deg, only under a clear-ish sky.
    float golden = smoothstepf(-3.0f, 2.0f, e->sun_elev_deg) * (1.0f - smoothstepf(6.0f, 18.0f, e->sun_elev_deg)) * (1.0f - overcast);
    float night = 1.0f - saturate(e->daylight);
    mod_add(m, &p->golden, golden);
    mod_add(m, &p->overcast, overcast * saturate(e->daylight));
    mod_add(m, &p->storm, storm);
    mod_add(m, &p->night, night);
}

void look_apply_mod(PostSettings *s, const LookMod *m) {
    s->wb_temp = clampf(s->wb_temp + m->temp, -1.0f, 1.0f);
    s->wb_tint = clampf(s->wb_tint + m->tint, -1.0f, 1.0f);
    s->exposure_comp += m->exposure;
    s->saturation = fmaxf(s->saturation * m->sat_mul, 0.0f);
    s->contrast = fmaxf(s->contrast * m->contrast_mul, 0.1f);
    s->bloom_intensity = clampf(s->bloom_intensity * m->bloom_mul, 0.0f, 0.5f);
    s->grain = clampf(s->grain * m->grain_mul, 0.0f, 1.0f);
    if (s->vignette > 0.0f || m->vignette_add > 0.0f) s->vignette = clampf(s->vignette + m->vignette_add, 0.0f, 1.0f);
}

// ---------------------------------------------------------------- dev tuning

#if DG_LOOK_TUNE
bool look_tune_compiled(void) { return true; }

#define FV(n, f) { n, offsetof(LookParams, f), 1, false }
#define F3(n, f) { n, offsetof(LookParams, f), 3, false }
#define FI(n, f) { n, offsetof(LookParams, f), 1, true }
#define FA(n, f, k) { n, offsetof(LookParams, f), k, false }
#define FM(set)                                                                                                                   \
    FV(#set ".temp", set.temp), FV(#set ".tint", set.tint), FV(#set ".exposure", set.exposure), FV(#set ".sat", set.sat),        \
        FV(#set ".contrast", set.contrast), FV(#set ".bloom", set.bloom), FV(#set ".grain", set.grain), FV(#set ".vignette", set.vignette)
static const LookField k_fields[] = {
    FV("exposure_ev", exposure_ev), FV("wb_temp", wb_temp), FV("wb_tint", wb_tint), FV("contrast", contrast), FV("pivot_ev", pivot_ev),
    F3("lift", lift), F3("gamma", gamma), F3("gain", gain), FV("shadow_end", shadow_end), FV("highlight_start", highlight_start),
    FV("range_soft", range_soft), FV("rolloff", rolloff), FV("rolloff_start", rolloff_start), FV("saturation", saturation),
    FV("vibrance", vibrance), FV("skin_protect", skin_protect), FA("hue_hue", hue_hue, LOOK_HUE_KNOTS), FA("hue_sat", hue_sat, LOOK_HUE_KNOTS),
    FA("sat_sat", sat_sat, LOOK_SAT_KNOTS), F3("bal_shadows", bal_shadows), F3("bal_mids", bal_mids), F3("bal_highs", bal_highs),
    FV("split_shadow_hue", split_shadow_hue), FV("split_shadow_amt", split_shadow_amt), FV("split_high_hue", split_high_hue),
    FV("split_high_amt", split_high_amt), FV("split_balance", split_balance), FI("tonemapper", tonemapper), FV("tm_toe", tm_toe),
    FV("tm_shoulder", tm_shoulder), FI("lut", lut), FV("lut_strength", lut_strength), FV("black_point", black_point),
    FV("white_point", white_point), F3("black_tint", black_tint), FV("bloom", bloom), FV("bloom_threshold", bloom_threshold),
    F3("bloom_tint", bloom_tint), FV("vignette", vignette), FV("grain", grain), FV("chromatic", chromatic), FV("sharpen", sharpen),
    FV("halation", halation), FI("letterbox", letterbox), F3("damage_color", damage_color), F3("lowhealth_color", lowhealth_color),
    FM(golden), FM(overcast), FM(storm), FM(night),
};

const LookField *look_fields(int *count) {
    *count = (int)(sizeof(k_fields) / sizeof(k_fields[0]));
    return k_fields;
}

// "%g" with at least one decimal so the C reads as float literals.
static int fmt_f(char *b, size_t n, float v) {
    char t[20];
    snprintf(t, sizeof(t), "%.4g", (double)v);
    bool dot = strchr(t, '.') || strchr(t, 'e');
    return snprintf(b, n, "%s%sf", t, dot ? "" : ".0");
}

int look_format(const LookParams *p, char *buf, size_t n) {
    static const char *const tms[] = { "LOOK_TM_ACES", "LOOK_TM_AGX", "LOOK_TM_NEUTRAL", "LOOK_TM_FILMIC" };
    static const char *const luts[] = { "LOOK_LUT_NONE", "LOOK_LUT_PRINT", "LOOK_LUT_BLEACH" };
    size_t o = 0;
#define OUT(...) (o += (size_t)snprintf(buf + DG_MIN(o, n), o < n ? n - o : 0, __VA_ARGS__))
    OUT("    {\n        .id = \"%s\", .name = \"%s\",\n        .desc = \"%s\",\n", p->id, p->name, p->desc);
    int count;
    const LookField *f = look_fields(&count);
    const char *group = NULL;
    for (int i = 0; i < count; i++) {
        const char *dot = strchr(f[i].name, '.');
        const float *v = (const float *)((const char *)p + f[i].offset);
        if (dot) {
            // A modulator set: one line per set.
            size_t gl = (size_t)(dot - f[i].name);
            if (!group || strncmp(group, f[i].name, gl)) {
                const LookModSet *m = (const LookModSet *)v;
                char s[8][24];
                const float *mv = &m->temp;
                for (int k = 0; k < 8; k++) fmt_f(s[k], sizeof(s[k]), mv[k]);
                OUT("        .%.*s = { %s, %s, %s, %s, %s, %s, %s, %s },\n", (int)gl, f[i].name, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
                group = f[i].name;
            }
            continue;
        }
        if (f[i].is_int) {
            int iv = *(const int *)v;
            if (!strcmp(f[i].name, "tonemapper") && iv >= 0 && iv < 4) OUT("        .%s = %s,\n", f[i].name, tms[iv]);
            else if (!strcmp(f[i].name, "lut") && iv >= 0 && iv < 3) OUT("        .%s = %s,\n", f[i].name, luts[iv]);
            else OUT("        .%s = %d,\n", f[i].name, iv);
            continue;
        }
        char s[24];
        if (f[i].count == 1) {
            fmt_f(s, sizeof(s), v[0]);
            OUT("        .%s = %s,\n", f[i].name, s);
        } else {
            OUT("        .%s = %s", f[i].name, f[i].count == 3 ? "{ " : "{ ");
            for (int k = 0; k < f[i].count; k++) {
                fmt_f(s, sizeof(s), v[k]);
                OUT("%s%s", s, k + 1 < f[i].count ? ", " : " },\n");
            }
        }
    }
    OUT("    },\n");
#undef OUT
    return (int)o;
}
#else
bool look_tune_compiled(void) { return false; }
#endif
