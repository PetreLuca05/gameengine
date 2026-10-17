#include "terrain_params.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MgMaterial names (mapgen.h order): the slot keys of terrain.json and the transitions' a / b.
static const char *const k_slot_names[] = { "grass", "meadow", "forest", "dirt", "rock", "sand", "gravel", "asphalt",
                                            "dirt_road", "concrete", "field", "crop", "plowed", "mud" };
#define SLOT_NAMES (int)(sizeof(k_slot_names) / sizeof(k_slot_names[0]))

static int slot_of(const char *name) {
    for (int i = 0; i < SLOT_NAMES; i++)
        if (!strcmp(k_slot_names[i], name)) return i;
    return -1;
}

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static const char *js(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static const cJSON *jo(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL; }
// rgb multiplier (array of 3, linear); def when missing
static void jrgb(const cJSON *o, const char *k, float out[3], float def) {
    const cJSON *v = jo(o, k);
    for (int i = 0; i < 3; i++) out[i] = def;
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 3)
        for (int i = 0; i < 3; i++) out[i] = (float)cJSON_GetArrayItem(v, i)->valuedouble;
}
static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static bool hex_lin(const char *h, float out[3]) {
    if (!h || strlen(h) != 7 || h[0] != '#') return false;
    unsigned c = (unsigned)strtoul(h + 1, NULL, 16);
    for (int i = 0; i < 3; i++) out[i] = srgb_to_lin((float)((c >> (16 - 8 * i)) & 255u) / 255.0f);
    return true;
}

int terrain_params_tex(TerrainParams *p, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < p->tex_count; i++)
        if (!strcmp(p->tex[i], name)) return i;
    if (p->tex_count >= TER_MAX_TEX) return -1;
    dg_strcpy(p->tex[p->tex_count], sizeof(p->tex[0]), name);
    return p->tex_count++;
}

static int mask_source(const char *s) {
    static const char *const names[] = { "noise", "moisture", "curvature", "slope", "paint", "dry" };
    for (int i = 0; s && i < 6; i++)
        if (!strcmp(names[i], s)) return i;
    return TMS_NOISE;
}

// mapgen_material_color's palette (the untextured look): the mean of a slot without a meta.json.
static const float k_palette[14][3] = { { 0.17f, 0.25f, 0.09f }, { 0.33f, 0.31f, 0.16f }, { 0.13f, 0.12f, 0.07f }, { 0.29f, 0.22f, 0.14f }, { 0.25f, 0.24f, 0.22f },
                                        { 0.58f, 0.52f, 0.38f }, { 0.38f, 0.36f, 0.33f }, { 0.10f, 0.10f, 0.11f }, { 0.34f, 0.27f, 0.18f }, { 0.40f, 0.39f, 0.37f },
                                        { 0.50f, 0.41f, 0.20f }, { 0.20f, 0.29f, 0.09f }, { 0.22f, 0.16f, 0.10f }, { 0.18f, 0.16f, 0.12f } };

static void layer_defaults(TerLayerGpu *L, int slot) {
    memset(L, 0, sizeof(*L));
    L->tex[0] = -1;
    L->tex[1] = -1;
    L->tex[2] = 1.0f / 3.0f;
    L->tex[3] = 1.0f / 40.0f;
    L->tiling[0] = TT_HEX, L->tiling[1] = 1.0f, L->tiling[2] = 0.12f, L->tiling[3] = 1.0f;
    L->multi[0] = 0.23f, L->multi[1] = 0.07f, L->multi[2] = 0.5f, L->multi[3] = 70.0f;
    L->hsv[0] = 0.04f, L->hsv[1] = 0.05f, L->hsv[2] = 0.05f, L->hsv[3] = 1.0f / 9.0f;
    L->blend[0] = 1.0f, L->blend[1] = 0.2f, L->blend[2] = 0.0f, L->blend[3] = 0.7f;
    L->curv[0] = 0.25f, L->curv[1] = 0.12f, L->curv[2] = 0.2f, L->curv[3] = 0.8f;
    for (int i = 0; i < 3; i++) L->moist[i] = L->slope[i] = L->alt[i] = L->aspect[i] = 1.0f;
    L->ranges[0] = 0.95f, L->ranges[1] = 0.75f, L->ranges[2] = 60.0f, L->ranges[3] = 160.0f;
    L->var0[0] = L->var1[0] = -1;
    L->varn[0] = 1.0f / 14.0f, L->varn[1] = 0.6f, L->varn[2] = 1.0f;
    L->detail[0] = 0.0f, L->detail[1] = 0.4f, L->detail[2] = 0.04f, L->detail[3] = 1.0f;
    L->snow[0] = 1.0f, L->snow[1] = 1.0f, L->snow[2] = 0.0f, L->snow[3] = 0.0f;
    L->scatter[1] = 0.8f, L->scatter[3] = 0.12f;
    const float *c = k_palette[slot < 14 ? slot : 3];
    L->mean[0] = c[0], L->mean[1] = c[1], L->mean[2] = c[2], L->mean[3] = 0.9f;
}

static void parse_layer(TerrainParams *p, int slot, const char *json, TerTextFn meta_json, void *user) {
    TerLayerGpu *L = &p->gpu.layer[slot];
    cJSON *r = cJSON_Parse(json);
    if (!r) { log_warn("terrain: layer %s: JSON parse error", p->slot_layer[slot]); return; }
    const char *tex = js(r, "texture");
    L->tex[0] = (float)terrain_params_tex(p, tex);
    const char *macro = js(r, "macro");
    if (!macro && tex && strncmp(tex, "surf:", 5)) macro = tex;
    L->tex[1] = macro ? (float)terrain_params_tex(p, macro) : -1.0f;
    L->tex[2] = 1.0f / fmaxf(jf(r, "tile_m", 3.0f), 0.1f);
    L->tex[3] = 1.0f / fmaxf(jf(r, "macro_scale_m", 40.0f), 1.0f);
    const cJSON *t = jo(r, "tiling");
    const char *mode = js(t, "mode");
    if (mode) L->tiling[0] = !strcmp(mode, "plain") ? TT_PLAIN : (!strcmp(mode, "cells") ? TT_CELLS : TT_HEX);
    L->tiling[1] = jf(t, "cell", L->tiling[1]);
    L->tiling[2] = jf(t, "scale_jitter", L->tiling[2]);
    L->tiling[3] = jf(t, "rotation", L->tiling[3]);
    const cJSON *m = jo(r, "multiscale");
    L->multi[0] = jf(m, "far1", L->multi[0]);
    L->multi[1] = jf(m, "far2", L->multi[1]);
    L->multi[2] = jf(m, "strength", L->multi[2]);
    L->multi[3] = jf(m, "halve_at_m", L->multi[3]);
    const cJSON *h = jo(r, "hsv_jitter");
    L->hsv[0] = jf(h, "hue_deg", L->hsv[0] * 57.29578f) / 57.29578f;
    L->hsv[1] = jf(h, "sat", L->hsv[1]);
    L->hsv[2] = jf(h, "val", L->hsv[2]);
    L->hsv[3] = 1.0f / fmaxf(jf(h, "scale_m", 1.0f / L->hsv[3]), 0.1f);
    const cJSON *b = jo(r, "blend");
    L->blend[0] = jf(b, "height_contrast", L->blend[0]);
    L->blend[1] = jf(b, "sharpness", L->blend[1]);
    L->blend[2] = jf(b, "height_offset", L->blend[2]);
    L->blend[3] = jf(r, "porosity", L->blend[3]);
    const cJSON *c = jo(r, "curvature");
    L->curv[0] = jf(c, "cavity", L->curv[0]);
    L->curv[1] = jf(c, "ridge", L->curv[1]);
    const cJSON *mo = jo(r, "moisture");
    jrgb(mo, "tint", L->moist, 1.0f);
    L->moist[3] = jf(mo, "amount", 0.0f);
    L->curv[2] = jf(mo, "darken", L->curv[2]);
    L->curv[3] = jf(r, "ao", L->curv[3]);
    const cJSON *st = jo(r, "slope_tint");
    jrgb(st, "tint", L->slope, 1.0f);
    L->slope[3] = jf(st, "amount", 0.0f);
    L->ranges[0] = jf(st, "from", L->ranges[0]);
    L->ranges[1] = jf(st, "to", L->ranges[1]);
    const cJSON *at = jo(r, "altitude_tint");
    jrgb(at, "tint", L->alt, 1.0f);
    L->alt[3] = jf(at, "amount", 0.0f);
    L->ranges[2] = jf(at, "from_m", L->ranges[2]);
    L->ranges[3] = jf(at, "to_m", L->ranges[3]);
    const cJSON *as = jo(r, "aspect_tint");
    jrgb(as, "tint", L->aspect, 1.0f);
    L->aspect[3] = jf(as, "amount", 0.0f);
    const cJSON *vars = jo(r, "variants");
    float *vv[2] = { L->var0, L->var1 };
    for (int i = 0; i < 2 && cJSON_IsArray(vars) && i < cJSON_GetArraySize(vars); i++) {
        const cJSON *v = cJSON_GetArrayItem(vars, i);
        vv[i][0] = (float)terrain_params_tex(p, js(v, "texture"));
        vv[i][1] = (float)mask_source(js(v, "mask"));
        vv[i][2] = jf(v, "lo", 0.5f);
        vv[i][3] = jf(v, "hi", 0.7f);
    }
    const cJSON *vn = jo(r, "variant_noise");
    L->varn[0] = 1.0f / fmaxf(jf(vn, "scale_m", 1.0f / L->varn[0]), 0.1f);
    L->varn[1] = jf(vn, "weight", L->varn[1]);
    L->varn[2] = jf(r, "macro_strength", L->varn[2]);
    const cJSON *d = jo(r, "detail");
    L->detail[0] = jf(d, "parallax_m", L->detail[0]);
    L->detail[1] = jf(d, "detail_normal", L->detail[1]);
    L->detail[2] = jf(d, "micro_roughness", L->detail[2]);
    L->detail[3] = jf(d, "normal", L->detail[3]);
    const cJSON *sn = jo(r, "snow");
    L->snow[0] = jf(sn, "accumulation", L->snow[0]);
    L->snow[1] = jf(sn, "wet_darkening", L->snow[1]);
    L->snow[2] = jf(r, "expose_below", L->snow[2]);
    L->snow[3] = jf(r, "expose", L->snow[3]);
    const cJSON *sc = jo(r, "scatter");
    L->scatter[0] = jf(sc, "per_m2", 0.0f);
    L->scatter[1] = jf(sc, "cell_m", L->scatter[1]);
    L->scatter[3] = jf(sc, "size_m", L->scatter[3]);
    const cJSON *spr = jo(sc, "sprites");
    u32 bits = 0;
    const cJSON *e;
    if (cJSON_IsArray(spr)) cJSON_ArrayForEach(e, spr) if (cJSON_IsNumber(e) && e->valueint >= 0 && e->valueint < 24) bits |= 1u << e->valueint;
    L->scatter[2] = (float)bits;
    L->mean[3] = jf(r, "roughness", L->mean[3]);
    // mean albedo: the main texture's meta.json ("mean"), else the palette
    if (tex && meta_json) {
        const char *mj = meta_json(tex, user);
        cJSON *mr = mj ? cJSON_Parse(mj) : NULL;
        float c3[3];
        if (mr && hex_lin(js(mr, "mean"), c3)) memcpy(L->mean, c3, sizeof(c3));
        cJSON_Delete(mr);
    }
    memcpy(p->mean_albedo[slot], L->mean, sizeof(float) * 3);
    cJSON_Delete(r);
}

static void pair_defaults(TerPairGpu *q) {
    memset(q, 0, sizeof(*q));
    q->a[0] = 0.6f, q->a[1] = 1.5f, q->a[2] = 0.5f, q->a[3] = 0.15f;
    q->b[0] = -1, q->b[1] = 0.8f, q->b[2] = 1.0f / 2.5f, q->b[3] = TPM_HEIGHT | TPM_NOISE | TPM_CRUMBS;
    q->d[1] = -1;
}

static u32 mode_bits(const cJSON *arr, u32 def) {
    if (!cJSON_IsArray(arr)) return def;
    static const char *const names[] = { "height", "noise", "crumbs", "set", "scatter", "directional", "slope", "wear", "moisture", "rim" };
    u32 bits = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) for (int i = 0; i < 10; i++) if (cJSON_IsString(e) && !strcmp(e->valuestring, names[i])) bits |= 1u << i;
    return bits;
}

static void parse_pair_fields(TerrainParams *p, TerPairGpu *q, const cJSON *r) {
    q->a[0] = jf(r, "width_m", q->a[0]);
    q->a[1] = jf(r, "edge_noise_scale_m", q->a[1]);
    q->a[2] = jf(r, "edge_noise_strength_m", q->a[2]);
    q->a[3] = jf(r, "crumbs", q->a[3]);
    const char *set = js(r, "set");
    if (set) q->b[0] = (float)terrain_params_tex(p, set);
    q->b[1] = jf(r, "set_strength", q->b[1]);
    q->b[2] = 1.0f / fmaxf(jf(r, "set_tile_m", 1.0f / q->b[2]), 0.1f);
    q->b[3] = (float)mode_bits(jo(r, "modes"), (u32)q->b[3]);
    const char *dir = js(r, "direction");
    if (dir) q->c[0] = !strcmp(dir, "wind") ? 1.0f : (!strcmp(dir, "downhill") ? 2.0f : 0.0f);
    q->c[1] = jf(r, "direction_m", q->c[1]);
    q->c[2] = jf(r, "slope_bias_m", q->c[2]);
    q->c[3] = jf(r, "wear_bias_m", q->c[3]);
    q->d[0] = jf(r, "moisture_bias_m", q->d[0]);
    const char *rim = js(r, "rim");
    if (rim) q->d[1] = (float)terrain_params_tex(p, rim);
    q->d[2] = jf(r, "rim_width_m", q->d[2]);
    q->d[3] = jf(r, "tint_by_weight", q->d[3]);
    const cJSON *spr = jo(r, "scatter_sprites");
    if (cJSON_IsArray(spr)) {
        u32 bits = 0;
        const cJSON *e;
        cJSON_ArrayForEach(e, spr) if (cJSON_IsNumber(e) && e->valueint >= 0 && e->valueint < 24) bits |= 1u << e->valueint;
        q->e[0] = (float)bits;
    }
    q->e[1] = jf(r, "scatter_per_m2", q->e[1]);
    q->e[2] = jf(r, "puddle_bias", q->e[2]);
    q->e[3] = jf(r, "snow_bias", q->e[3]);
}

// A list of slot names ("grass" or ["grass", "meadow"]) as a bit set.
static u32 slot_set(const cJSON *v) {
    u32 bits = 0;
    if (cJSON_IsString(v)) {
        int s = slot_of(v->valuestring);
        if (s >= 0) bits |= 1u << s;
    } else if (cJSON_IsArray(v)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, v) if (cJSON_IsString(e)) {
            int s = slot_of(e->valuestring);
            if (s >= 0) bits |= 1u << s;
        }
    }
    return bits;
}

bool terrain_params_parse(TerrainParams *p, const char *global_json, TerTextFn layer_json, TerTextFn meta_json, const char *const *pair_json, int pair_count, void *user) {
    memset(p, 0, sizeof(*p));
    cJSON *g = cJSON_Parse(global_json);
    if (!g) { snprintf(p->err, sizeof(p->err), "terrain.json: parse error"); return false; }
    TerGlobalGpu *G = &p->gpu.g;
    G->tier[0] = 4;
    G->tier[3] = jf(g, "blend_radius_m", 3.0f);
    const cJSON *d = jo(g, "distances");
    G->dist[0] = jf(d, "hex_far_m", 160.0f);
    G->dist[1] = jf(d, "parallax_m", 10.0f);
    G->dist[2] = jf(d, "scatter_m", 35.0f);
    G->dist[3] = jf(d, "detail_m", 18.0f);
    const cJSON *m = jo(g, "macro");
    G->macro[0] = jf(m, "texture_strength", 1.0f);
    p->ground_macro[0] = 1.0f / fmaxf(jf(m, "scale1_m", 220.0f), 1.0f);
    p->ground_macro[1] = 1.0f / fmaxf(jf(m, "scale2_m", 65.0f), 1.0f);
    p->ground_macro[2] = jf(m, "hue_deg", 7.0f) / 57.29578f;
    p->ground_macro[3] = jf(m, "val", 0.08f);
    p->ground_macro[4] = jf(m, "sat", 0.1f);
    p->ground_macro[5] = jf(m, "strength", 1.0f);
    G->macro[1] = jf(g, "crumb_m", 0.18f);
    G->macro[2] = jf(g, "slope_tint_strength", 1.0f);
    G->macro[3] = jf(g, "curvature_strength", 1.0f);
    const cJSON *sn = jo(g, "snow");
    G->snow[0] = jf(sn, "line_m", 120.0f);
    G->snow[1] = jf(sn, "band_m", 40.0f);
    G->snow[2] = jf(sn, "noise_m", 15.0f);
    G->snow[3] = jf(sn, "per_degree_m", 25.0f);
    G->wind[0] = 1.0f;
    const cJSON *gt = jo(g, "grass_term");
    G->grass[0] = jf(gt, "start_m", 45.0f);
    G->grass[1] = jf(gt, "end_m", 70.0f);
    G->grass[2] = jf(gt, "strength", 1.0f);
    const cJSON *sc = jo(g, "scatter");
    const char *atlas = js(sc, "atlas");
    if (atlas) dg_strcpy(p->scatter_atlas, sizeof(p->scatter_atlas), atlas);
    const cJSON *grid = jo(sc, "grid");
    G->scatter[0] = cJSON_IsArray(grid) ? (float)cJSON_GetArrayItem(grid, 0)->valueint : 1.0f;
    G->scatter[1] = cJSON_IsArray(grid) ? (float)cJSON_GetArrayItem(grid, 1)->valueint : 1.0f;
    const cJSON *q = jo(g, "quality_by_level");
    for (int i = 0; i < TER_TIERS; i++) p->level_quality[i] = cJSON_IsArray(q) && i < cJSON_GetArraySize(q) ? (float)cJSON_GetArrayItem(q, i)->valuedouble : (float)i;
    for (int s = 0; s < TER_SLOTS; s++) layer_defaults(&p->gpu.layer[s], s);
    const cJSON *slots = jo(g, "slots");
    for (int s = 0; s < SLOT_NAMES; s++) {
        const char *ln = js(slots, k_slot_names[s]);
        if (!ln) continue;
        dg_strcpy(p->slot_layer[s], sizeof(p->slot_layer[s]), ln);
        const char *lj = layer_json ? layer_json(ln, user) : NULL;
        if (!lj) { log_warn("terrain: layer %s (slot %s) missing", ln, k_slot_names[s]); continue; }
        parse_layer(p, s, lj, meta_json, user);
    }
    cJSON_Delete(g);
    // transitions: pair 0 = the defaults (or _default.json's), every other file a pair over its slot sets
    p->pair_count = 1;
    pair_defaults(&p->gpu.pair[0]);
    dg_strcpy(p->pair_name[0], sizeof(p->pair_name[0]), "default");
    for (int i = 0; i < pair_count; i++) {
        cJSON *r = cJSON_Parse(pair_json[i]);
        if (!r) { log_warn("terrain: transition %d: JSON parse error", i); continue; }
        const char *name = js(r, "name");
        if (name && !strcmp(name, "default")) {
            parse_pair_fields(p, &p->gpu.pair[0], r);
            cJSON_Delete(r);
            continue;
        }
        if (p->pair_count >= TER_MAX_PAIRS) { cJSON_Delete(r); break; }
        int k = p->pair_count++;
        p->gpu.pair[k] = p->gpu.pair[0];
        parse_pair_fields(p, &p->gpu.pair[k], r);
        dg_strcpy(p->pair_name[k], sizeof(p->pair_name[k]), name ? name : "pair");
        u32 a = slot_set(jo(r, "a")), b = slot_set(jo(r, "b"));
        // later files override earlier ones; a pair applies both ways (the first material is a)
        for (int x = 0; x < TER_SLOTS; x++)
            for (int y = 0; y < TER_SLOTS; y++)
                if ((a >> x & 1u) && (b >> y & 1u) && x != y) {
                    p->gpu.pair_of[x * TER_SLOTS + y] = (u32)k;
                    p->gpu.pair_of[y * TER_SLOTS + x] = (u32)k | 0x80000000u;   // high bit: (y, x) is the pair's (b, a)
                }
        cJSON_Delete(r);
    }
    return true;
}

// ------------------------------------------------------------------ files

typedef struct DirCtx { const char *dir; char *texts[64]; int n; } DirCtx;

static char *read_text(const char *rel) {
    char path[512];
    size_t sz;
    return platform_read_file(platform_data_path(rel, path, sizeof(path)), &sz);
}

static const char *layer_text(const char *name, void *user) {
    DirCtx *c = (DirCtx *)user;
    char rel[256];
    snprintf(rel, sizeof(rel), "%s/layers/%s.json", c->dir, name);
    char *t = read_text(rel);
    if (t && c->n < 64) c->texts[c->n++] = t;
    return t;
}

static const char *meta_text(const char *name, void *user) {
    DirCtx *c = (DirCtx *)user;
    char rel[256];
    if (!strncmp(name, "surf:", 5)) return NULL;
    snprintf(rel, sizeof(rel), "%s/tex/%s.meta.json", c->dir, name);
    char *t = read_text(rel);
    if (t && c->n < 64) c->texts[c->n++] = t;
    return t;
}

static int cmp_names(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

// Sorted *.json of dir/sub (files starting with "_" first: they are the defaults).
static char **list_json(const char *dir, const char *sub, int *count) {
    char rel[256], buf[512];
    snprintf(rel, sizeof(rel), "%s/%s", dir, sub);
    char **files = SDL_GlobDirectory(platform_data_path(rel, buf, sizeof(buf)), "*.json", 0, count);
    if (files && *count > 1) qsort(files, (size_t)*count, sizeof(char *), cmp_names);
    return files;
}

bool terrain_params_load(TerrainParams *p, const char *dir) {
    char rel[256];
    snprintf(rel, sizeof(rel), "%s/terrain.json", dir);
    char *global = read_text(rel);
    if (!global) {
        memset(p, 0, sizeof(*p));
        snprintf(p->err, sizeof(p->err), "%.200s missing", rel);
        return false;
    }
    DirCtx c = { dir, { 0 }, 0 };
    int nf = 0;
    char **files = list_json(dir, "transitions", &nf);
    char *pairs[TER_MAX_PAIRS + 1];
    int np = 0;
    for (int i = 0; i < nf && np < TER_MAX_PAIRS; i++) {
        snprintf(rel, sizeof(rel), "%s/transitions/%s", dir, files[i]);
        char *t = read_text(rel);
        if (t) pairs[np++] = t;
    }
    if (files) SDL_free(files);
    bool ok = terrain_params_parse(p, global, layer_text, meta_text, (const char *const *)pairs, np, &c);
    free(global);
    for (int i = 0; i < np; i++) free(pairs[i]);
    for (int i = 0; i < c.n; i++) free(c.texts[i]);
    return ok;
}

i64 terrain_params_mtime(const char *dir) {
    i64 best = 0;
    char rel[256], buf[512];
    snprintf(rel, sizeof(rel), "%s/terrain.json", dir);
    best = platform_file_mtime(platform_data_path(rel, buf, sizeof(buf)));
    const char *subs[2] = { "layers", "transitions" };
    for (int s = 0; s < 2; s++) {
        int n = 0;
        char **files = list_json(dir, subs[s], &n);
        for (int i = 0; i < n; i++) {
            snprintf(rel, sizeof(rel), "%s/%s/%s", dir, subs[s], files[i]);
            i64 t = platform_file_mtime(platform_data_path(rel, buf, sizeof(buf)));
            if (t > best) best = t;
        }
        if (files) SDL_free(files);
    }
    return best;
}
