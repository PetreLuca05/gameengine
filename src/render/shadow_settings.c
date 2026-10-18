// Unity-style shadow controls: defaults, the per-tier table, behaviour formulas, names, the local
// light atlas packer (shadow_settings.h, docs/SHADOWS_UNITY_MODEL.md).
#include "render/shadow_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float clampf_(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

void shs_light_default(LightShadowSettings *s, ShadowLightKind kind) {
    // Unity's new-light defaults, in this engine's units: every light casts (the engine always did),
    // Soft, quality resolution, full strength, bias / normal bias 1 (= the engine's default look).
    s->type = SHADOW_SOFT;
    s->resolution = SHADOW_RES_QUALITY;
    s->strength = 1.0f;
    s->bias = 1.0f;
    s->normal_bias = 1.0f;
    s->near_plane = kind == SHADOW_LIGHT_DIRECTIONAL ? 0.0f : 0.2f;
    s->culling_mask = SHADOW_LAYERS_ALL;
    s->distance = 0.0f;
}

void shs_quality_default(QualityShadowSettings *q) { shs_quality_for_tier(q, 4); }

void shs_quality_for_tier(QualityShadowSettings *q, int level) {
    // Off / Low / Medium / High / Modern (fidelity.h FID_*); assets/render/fidelity/*.json mirror it
    // (tests/test_shadow_settings.c checks they agree).
    // The distance is locked (assets/render/consistency.json r_shadow_max_distance, docs/
    // SETTINGS_AUDIT.md): every level shadows out to 150 m and has at least hard shadows; the levels
    // trade resolution, cascades and the filter.
    static const struct { ShadowsMode mode; ShadowRes res; int casc; float dist; SoftQuality soft; } k[5] = {
        { SHADOWS_HARD_ONLY, SHADOW_RES_LOW, 2, 150.0f, SOFT_LOW },
        { SHADOWS_HARD_ONLY, SHADOW_RES_LOW, 4, 150.0f, SOFT_LOW },
        { SHADOWS_HARD_AND_SOFT, SHADOW_RES_MEDIUM, 4, 150.0f, SOFT_LOW },
        { SHADOWS_HARD_AND_SOFT, SHADOW_RES_HIGH, 4, 150.0f, SOFT_MEDIUM },
        { SHADOWS_HARD_AND_SOFT, SHADOW_RES_VERY_HIGH, 4, 150.0f, SOFT_ULTRA },
    };
    int l = level < 0 ? 0 : (level > 4 ? 4 : level);
    memset(q, 0, sizeof(*q));
    q->mode = k[l].mode;
    q->resolution = k[l].res;
    q->projection = SHADOW_STABLE_FIT;
    q->distance = k[l].dist;
    q->shadowmask = SHADOWMASK_DISTANCE;
    q->shadowmask_distance = 150.0f;
    q->near_plane_offset = 3000.0f;
    q->cascades = k[l].casc;
    q->split2 = 1.0f / 3.0f;            // Unity: 33.3 %
    q->split4[0] = 0.067f;              // Unity: 6.7 / 20 / 46.7 %
    q->split4[1] = 0.2f;
    q->split4[2] = 0.467f;
    q->border = 0.1f;
    q->soft_quality = k[l].soft;
}

void shs_quality_sanitize(QualityShadowSettings *q) {
    q->mode = (ShadowsMode)(q->mode < 0 ? 0 : (q->mode >= SHADOWS_MODE_COUNT ? SHADOWS_MODE_COUNT - 1 : q->mode));
    q->resolution = (ShadowRes)(q->resolution < 0 ? 0 : (q->resolution >= SHADOW_RES_COUNT ? SHADOW_RES_COUNT - 1 : q->resolution));
    q->projection = q->projection == SHADOW_CLOSE_FIT ? SHADOW_CLOSE_FIT : SHADOW_STABLE_FIT;
    q->distance = clampf_(q->distance, 10.0f, 2000.0f);
    q->shadowmask = q->shadowmask == SHADOWMASK_SHADOWMASK ? SHADOWMASK_SHADOWMASK : SHADOWMASK_DISTANCE;
    q->shadowmask_distance = clampf_(q->shadowmask_distance, 0.0f, 2000.0f);
    q->near_plane_offset = clampf_(q->near_plane_offset, 2.0f, 5000.0f);
    q->cascades = q->cascades >= 3 ? 4 : (q->cascades == 2 ? 2 : 1);
    q->split2 = clampf_(q->split2, 0.01f, 0.99f);
    float lo = 0.01f;
    for (int i = 0; i < 3; i++) {
        q->split4[i] = clampf_(q->split4[i], lo, 0.99f - 0.01f * (float)(2 - i));
        lo = q->split4[i] + 0.01f;
    }
    q->border = clampf_(q->border, 0.0f, 0.5f);
    q->soft_quality = (SoftQuality)(q->soft_quality < 0 ? 0 : (q->soft_quality >= SOFT_QUALITY_COUNT ? SOFT_QUALITY_COUNT - 1 : q->soft_quality));
}

void shs_light_sanitize(LightShadowSettings *s) {
    s->type = (ShadowType)(s->type < 0 ? 0 : (s->type >= SHADOW_TYPE_COUNT ? SHADOW_TYPE_COUNT - 1 : s->type));
    s->resolution = s->resolution < SHADOW_RES_QUALITY ? SHADOW_RES_QUALITY : (s->resolution >= SHADOW_RES_COUNT ? SHADOW_RES_COUNT - 1 : s->resolution);
    s->strength = clampf_(s->strength, 0.0f, 1.0f);
    s->bias = clampf_(s->bias, 0.0f, 2.0f);
    s->normal_bias = clampf_(s->normal_bias, 0.0f, 3.0f);
    s->near_plane = clampf_(s->near_plane, 0.0f, 10.0f);
    s->culling_mask &= SHADOW_LAYERS_ALL;
    s->distance = clampf_(s->distance, 0.0f, 2000.0f);
}

// ------------------------------------------------------------------ behaviour

ShadowType shs_effective_type(ShadowType light, ShadowsMode mode) {
    if (mode == SHADOWS_DISABLE || light == SHADOW_NONE) return SHADOW_NONE;
    if (mode == SHADOWS_HARD_ONLY) return SHADOW_HARD;
    return light;
}

u32 shs_res_texels(int res, ShadowLightKind kind) {
    int r = res < 0 ? 0 : (res >= SHADOW_RES_COUNT ? SHADOW_RES_COUNT - 1 : res);
    return (kind == SHADOW_LIGHT_DIRECTIONAL ? 512u : 256u) << r;
}

u32 shs_light_texels(const LightShadowSettings *s, const QualityShadowSettings *q, ShadowLightKind kind) {
    return shs_res_texels(s->resolution == SHADOW_RES_QUALITY ? (int)q->resolution : s->resolution, kind);
}

float shs_tent_texels(SoftQuality q) { return q == SOFT_LOW ? 3.0f : (q == SOFT_MEDIUM ? 5.0f : 7.0f); }

int shs_tent_fetches(float n) {
    int f = (int)ceilf((n + 1.0f) * 0.5f - 1e-4f);
    return f < 1 ? 1 : (f > 4 ? 4 : f);
}

ShadowFilter shs_filter(ShadowType effective, SoftQuality q, ShadowLightKind kind) {
    ShadowFilter f = { SHF_NONE, 0.0f };
    if (effective == SHADOW_HARD) f.mode = SHF_HARD;
    else if (effective == SHADOW_SOFT) {
        // Ultra is contact-hardening PCSS on the sun (and the viewmodel tile); local lights use the
        // widest tent (their maps are perspective: no blocker distance in metres).
        if (q == SOFT_ULTRA && kind == SHADOW_LIGHT_DIRECTIONAL) f.mode = SHF_PCSS;
        else { f.mode = SHF_TENT; f.tent = shs_tent_texels(q); }
    }
    return f;
}

// Area of the tent of base n (height 2 / n, area 1) left of x, x relative to its centre.
static float tent_cdf(float x, float n) {
    float h = n * 0.5f;
    if (x <= -h) return 0.0f;
    if (x >= h) return 1.0f;
    if (x < 0.0f) { float t = x + h; return 0.5f * t * t / (h * h); }
    float t = h - x;
    return 1.0f - 0.5f * t * t / (h * h);
}

int shs_tent_axis(float c, float n, float pos[4], float w[4]) {
    int nf = shs_tent_fetches(n);
    // The texels the tent touches start at floor(c - n / 2); fetch k blends texels i0 + 2k, i0 + 2k + 1.
    int i0 = (int)floorf(c - n * 0.5f);
    for (int k = 0; k < nf; k++) {
        float a = (float)(i0 + 2 * k);
        float wa = tent_cdf(a + 1.0f - c, n) - tent_cdf(a - c, n);
        float wb = tent_cdf(a + 2.0f - c, n) - tent_cdf(a + 1.0f - c, n);
        w[k] = wa + wb;
        pos[k] = a + 0.5f + (w[k] > 1e-8f ? wb / w[k] : 0.0f);
    }
    return nf;
}

float shs_apply_strength(float s, float strength) { return 1.0f - clampf_(strength, 0.0f, 1.0f) * (1.0f - s); }

void shs_bias_m(float texel_m, float bias, float normal_bias, float low_sun, float *const_m, float *normal_m) {
    // bias 1 / normal bias 1 = the validated defaults of docs/SHADOWS.md (0.75 / 1.5 texels).
    *const_m = 0.75f * texel_m * bias * low_sun;
    *normal_m = 1.5f * texel_m * normal_bias * low_sun;
}

int shs_cascade_splits(const QualityShadowSettings *q, float s[4]) {
    float d = q->distance;
    if (q->cascades >= 4) {
        for (int i = 0; i < 3; i++) s[i] = q->split4[i] * d;
        s[3] = d;
        return 4;
    }
    if (q->cascades == 2) {
        s[0] = q->split2 * d;
        s[1] = s[2] = s[3] = d;
        return 2;
    }
    s[0] = s[1] = s[2] = s[3] = d;
    return 1;
}

bool shs_casts(CastShadows c) { return c != CAST_SHADOWS_OFF; }
bool shs_visible(CastShadows c) { return c != CAST_SHADOWS_ONLY; }
bool shs_two_sided_caster(CastShadows c, bool material_two_sided) { return c == CAST_SHADOWS_TWO_SIDED || (c != CAST_SHADOWS_OFF && material_two_sided); }

// ------------------------------------------------------------------ names

static const char *const k_type[] = { "none", "hard", "soft" };
static const char *const k_res[] = { "low", "medium", "high", "very_high" };
static const char *const k_cast[] = { "off", "on", "two_sided", "shadows_only" };
static const char *const k_mode[] = { "disable", "hard_only", "hard_and_soft" };
static const char *const k_soft[] = { "low", "medium", "high", "ultra" };
static const char *const k_proj[] = { "close_fit", "stable_fit" };

static int find(const char *const *names, int n, const char *s) {
    for (int i = 0; i < n; i++) if (s && !strcmp(s, names[i])) return i;
    return -1;
}

const char *shs_type_name(ShadowType t) { return t >= 0 && t < SHADOW_TYPE_COUNT ? k_type[t] : "?"; }
int shs_type_from_name(const char *s) { return find(k_type, SHADOW_TYPE_COUNT, s); }
const char *shs_res_name(int r) { return r == SHADOW_RES_QUALITY ? "use_quality" : (r >= 0 && r < SHADOW_RES_COUNT ? k_res[r] : "?"); }
int shs_res_from_name(const char *s, int *out) {
    if (s && !strcmp(s, "use_quality")) { *out = SHADOW_RES_QUALITY; return 1; }
    int r = find(k_res, SHADOW_RES_COUNT, s);
    if (r < 0) return 0;
    *out = r;
    return 1;
}
const char *shs_cast_name(CastShadows c) { return c >= 0 && c < CAST_SHADOWS_COUNT ? k_cast[c] : "?"; }
int shs_cast_from_name(const char *s) { return find(k_cast, CAST_SHADOWS_COUNT, s); }
const char *shs_mode_name(ShadowsMode m) { return m >= 0 && m < SHADOWS_MODE_COUNT ? k_mode[m] : "?"; }
const char *shs_soft_name(SoftQuality q) { return q >= 0 && q < SOFT_QUALITY_COUNT ? k_soft[q] : "?"; }
const char *shs_projection_name(ShadowProjection p) { return p >= 0 && p < SHADOW_PROJECTION_COUNT ? k_proj[p] : "?"; }

static bool parse_f(const char *v, float *out) {
    char *e;
    float f = strtof(v, &e);
    if (e == v || *e) return false;
    *out = f;
    return true;
}

bool shs_light_set(LightShadowSettings *s, const char *key, const char *value, char *err, int err_size) {
    LightShadowSettings n = *s;
    bool ok = true;
    float f;
    if (!strcmp(key, "type")) { int t = shs_type_from_name(value); ok = t >= 0; if (ok) n.type = (ShadowType)t; }
    else if (!strcmp(key, "resolution")) ok = shs_res_from_name(value, &n.resolution) != 0;
    else if (!strcmp(key, "strength")) { ok = parse_f(value, &f); n.strength = f; }
    else if (!strcmp(key, "bias")) { ok = parse_f(value, &f); n.bias = f; }
    else if (!strcmp(key, "normal_bias")) { ok = parse_f(value, &f); n.normal_bias = f; }
    else if (!strcmp(key, "near_plane")) { ok = parse_f(value, &f); n.near_plane = f; }
    else if (!strcmp(key, "distance")) { ok = parse_f(value, &f); n.distance = f; }
    else if (!strcmp(key, "culling_mask")) {
        char *e;
        unsigned long m = strtoul(value, &e, 0);
        ok = e != value && !*e;
        n.culling_mask = (u32)m;
    } else {
        snprintf(err, (size_t)err_size, "unknown key '%s' (type resolution strength bias normal_bias near_plane culling_mask distance)", key);
        return false;
    }
    if (!ok) { snprintf(err, (size_t)err_size, "bad value '%s' for %s", value, key); return false; }
    shs_light_sanitize(&n);
    *s = n;
    return true;
}

int shs_light_describe(const LightShadowSettings *s, char *buf, int size) {
    return snprintf(buf, (size_t)size, "type %s resolution %s strength %.2f bias %.2f normal_bias %.2f near_plane %.2f culling_mask 0x%02x distance %.1f", shs_type_name(s->type),
                    shs_res_name(s->resolution), s->strength, s->bias, s->normal_bias, s->near_plane, s->culling_mask, s->distance);
}

// ------------------------------------------------------------------ atlas

// Buddy allocator: free squares per size level (atlas >> level).
#define LEVELS 12
typedef struct Free { u32 n[LEVELS]; u32 x[LEVELS][SHS_ATLAS_MAX * 2], y[LEVELS][SHS_ATLAS_MAX * 2]; } Free;

static int level_of(u32 atlas, u32 size) {
    int l = 0;
    while ((atlas >> l) > size && l < LEVELS - 1) l++;
    return (atlas >> l) == size ? l : -1;
}

static bool alloc_sq(Free *f, u32 atlas, u32 size, u32 *ox, u32 *oy) {
    int want = level_of(atlas, size);
    if (want < 0) return false;
    int l = want;
    while (l >= 0 && !f->n[l]) l--;
    if (l < 0) return false;
    u32 x = f->x[l][f->n[l] - 1], y = f->y[l][f->n[l] - 1];
    f->n[l]--;
    // split down: keep the first quadrant, free the other three
    while (l < want) {
        l++;
        u32 h = atlas >> l;
        if (f->n[l] + 3 > SHS_ATLAS_MAX * 2) return false;
        // pushed in reverse so the next allocation takes the lowest x / y first
        f->x[l][f->n[l]] = x + h; f->y[l][f->n[l]] = y + h; f->n[l]++;
        f->x[l][f->n[l]] = x;     f->y[l][f->n[l]] = y + h; f->n[l]++;
        f->x[l][f->n[l]] = x + h; f->y[l][f->n[l]] = y;     f->n[l]++;
    }
    *ox = x;
    *oy = y;
    return true;
}

int shs_atlas_pack(u32 atlas, u32 min_size, const ShsAtlasReq *req, int n, ShsAtlasRect *out) {
    static Free f, snap;
    memset(&f, 0, sizeof(f));
    f.n[0] = 1;
    int placed = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && req[j].light == req[i].light) j++;
        u32 size = req[i].size;
        bool ok = false;
        while (!ok && size >= min_size && size > 0) {
            snap = f;
            ok = true;
            for (int k = i; k < j && ok; k++) {
                u32 s = req[k].size < size ? req[k].size : size;
                out[k].size = s;
                ok = alloc_sq(&f, atlas, s, &out[k].x, &out[k].y);
            }
            if (!ok) { f = snap; size >>= 1; }
        }
        for (int k = i; k < j; k++) {
            if (!ok) out[k].size = out[k].x = out[k].y = 0;
            else placed++;
        }
        i = j;
    }
    return placed;
}
