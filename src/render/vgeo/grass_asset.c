#include "grass_asset.h"
#include "foliage_wind.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"   // MAT_COVER
#include "render/shadow_casters.h"
#include "cJSON.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f

// Terrain material names (MgMaterial order, mapgen.h): the density table's keys.
static const char *const k_mat_names[] = { "grass", "meadow", "forest", "dirt", "rock", "sand", "gravel", "asphalt",
                                           "dirt_road", "concrete", "field", "crop", "plowed", "mud" };
static const char *const k_tier_names[GRASS_TIERS] = { "off", "low", "medium", "high", "modern" };

static void defaults(GrassAsset *a) {
    memset(a, 0, sizeof(*a));
    a->cols = a->rows = 1;
    a->shadow_class = SCC_GROUND;
    a->variants = 3;
    a->cards = 3;
    a->segments = 3;
    a->cell_count = 1;
    a->height_m[0] = 0.3f, a->height_m[1] = 0.45f;
    a->width_m[0] = 0.35f, a->width_m[1] = 0.5f;
    a->fan_deg = 18.0f;
    a->lean = 0.1f;
    a->bend_m = 0.06f;
    a->base_jitter_m = 0.05f;
    a->normal_dome = 0.5f;
    a->mirror = 0.5f;
    a->clump_scale_m = 6.0f;
    a->clump_amount = 0.5f;
    a->bare = 0.3f;
    a->scale[0] = 0.8f, a->scale[1] = 1.2f;
    a->slope_max = 1.0f;
    a->roughness = 0.65f;
    a->specular = 0.35f;
    a->sss = 0.4f;
    a->sss_wrap = 0.5f;
    a->alpha_cutoff = 0.45f;
    a->root_dark = 0.78f;
    a->root_extent = 0.25f;
    a->ground_match = 0.3f;
    a->ground_extent = 0.35f;
    a->var_hue = 4.0f, a->var_sat = 0.08f, a->var_val = 0.07f;
    a->macro = 1.0f;
    a->ao_root = 0.55f;
    a->ao_strength = MATERIAL_AO_FOLIAGE;
    a->ao_combine = MAT_AO_COMBINE_MIN;
    a->top_color[0] = 0.16f, a->top_color[1] = 0.17f, a->top_color[2] = 0.06f;
    a->coverage = 0.5f;
    static const GrassTier t[GRASS_TIERS] = { { 8, 18, 0, 0.35f }, { 12, 30, 0, 0.5f }, { 20, 45, 10, 0.75f }, { 25, 60, 20, 0.9f }, { 30, 70, 20, 1.0f } };
    memcpy(a->tier, t, sizeof(t));
    a->wind_main = a->wind_detail = a->wind_flutter = 1.0f;
}

static float jf(const cJSON *o, const char *k, float def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}
static void jrange(const cJSON *o, const char *k, float out[2]) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsArray(v) && cJSON_GetArraySize(v) == 2) {
        out[0] = (float)cJSON_GetArrayItem(v, 0)->valuedouble;
        out[1] = (float)cJSON_GetArrayItem(v, 1)->valuedouble;
    }
}
static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static bool jcolor(const cJSON *o, const char *k, float out[3]) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(v) || strlen(v->valuestring) != 7 || v->valuestring[0] != '#') return false;
    unsigned c = (unsigned)strtoul(v->valuestring + 1, NULL, 16);
    for (int i = 0; i < 3; i++) out[i] = srgb_to_lin((float)((c >> (16 - 8 * i)) & 255u) / 255.0f);
    return true;
}

bool grass_asset_parse(GrassAsset *a, const char *json, const char *name) {
    defaults(a);
    dg_strcpy(a->name, sizeof(a->name), name);
    cJSON *root = cJSON_Parse(json);
    if (!root) { log_error("grass: %s: JSON parse error", name); return false; }
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(root, "atlas");
    if (!cJSON_IsString(at)) { log_error("grass: %s: no \"atlas\"", name); cJSON_Delete(root); return false; }
    dg_strcpy(a->atlas, sizeof(a->atlas), at->valuestring);
    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "shadow_class");
    if (cJSON_IsString(sc)) {
        int c = scc_class_from_name(sc->valuestring);
        if (c < 0) log_warn("grass: %s: unknown shadow_class \"%s\" (none / ground / small / tree)", name, sc->valuestring);
        else a->shadow_class = c;
    }
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "grid");
    if (cJSON_IsArray(g) && cJSON_GetArraySize(g) == 2) {
        a->cols = DG_CLAMP(cJSON_GetArrayItem(g, 0)->valueint, 1, 8);
        a->rows = DG_CLAMP(cJSON_GetArrayItem(g, 1)->valueint, 1, 8);
    }
    a->variants = DG_CLAMP((int)jf(root, "variants", (float)a->variants), 1, 8);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "clump");
    if (c) {
        a->cards = DG_CLAMP((int)jf(c, "cards", (float)a->cards), 1, 8);
        a->segments = DG_CLAMP((int)jf(c, "segments", (float)a->segments), 1, 6);
        jrange(c, "height_m", a->height_m);
        jrange(c, "width_m", a->width_m);
        a->fan_deg = jf(c, "fan_deg", a->fan_deg);
        a->lean = jf(c, "lean", a->lean);
        a->bend_m = jf(c, "bend_m", a->bend_m);
        a->base_jitter_m = jf(c, "base_jitter_m", a->base_jitter_m);
        a->normal_dome = jf(c, "normal_dome", a->normal_dome);
        a->mirror = jf(c, "mirror", a->mirror);
        const cJSON *cells = cJSON_GetObjectItemCaseSensitive(c, "cells");
        if (cJSON_IsArray(cells)) {
            a->cell_count = 0;
            const cJSON *e;
            cJSON_ArrayForEach(e, cells) if (a->cell_count < GRASS_MAX_CELLS && cJSON_IsNumber(e)) a->cells[a->cell_count++] = DG_CLAMP(e->valueint, 0, a->cols * a->rows - 1);
        }
    }
    if (!c || !cJSON_GetObjectItemCaseSensitive(c, "cells")) {
        a->cell_count = DG_MIN(a->cols * a->rows, GRASS_MAX_CELLS);
        for (int i = 0; i < a->cell_count; i++) a->cells[i] = i;
    }
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "placement");
    if (p) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(p, "density");
        for (int i = 0; i < (int)(sizeof(k_mat_names) / sizeof(k_mat_names[0])); i++) a->density[i] = jf(d, k_mat_names[i], 0.0f);
        a->clump_scale_m = jf(p, "clump_scale_m", a->clump_scale_m);
        a->clump_amount = jf(p, "clump_amount", a->clump_amount);
        a->bare = jf(p, "bare", a->bare);
        jrange(p, "scale", a->scale);
        a->slope_max = jf(p, "slope_max", a->slope_max);
    }
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "shading");
    if (s) {
        a->roughness = jf(s, "roughness", a->roughness);
        a->specular = jf(s, "specular", a->specular);
        a->sss = jf(s, "sss", a->sss);
        a->sss_wrap = jf(s, "sss_wrap", a->sss_wrap);
        a->alpha_cutoff = jf(s, "alpha_cutoff", a->alpha_cutoff);
        a->root_dark = jf(s, "root_dark", a->root_dark);
        a->root_extent = jf(s, "root_extent", a->root_extent);
        a->ground_match = jf(s, "ground_match", a->ground_match);
        a->ground_extent = jf(s, "ground_extent", a->ground_extent);
        a->var_hue = jf(s, "var_hue_deg", a->var_hue);
        a->var_sat = jf(s, "var_sat", a->var_sat);
        a->var_val = jf(s, "var_val", a->var_val);
        a->macro = jf(s, "macro", a->macro);
        a->ao_root = jf(s, "ao_root", a->ao_root);
        a->ao_strength = jf(s, "ao_strength", a->ao_strength);
        if (a->ao_strength <= 0.0f) a->ao_strength = MATERIAL_AO_OFF;
        const cJSON *aoc = cJSON_GetObjectItemCaseSensitive(s, "ao_combine");
        if (cJSON_IsString(aoc)) a->ao_combine = material_ao_combine_parse(aoc->valuestring);
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "terrain");
    if (t) {
        jcolor(t, "top_color", a->top_color);
        a->coverage = jf(t, "coverage", a->coverage);
    }
    const cJSON *tiers = cJSON_GetObjectItemCaseSensitive(root, "tiers");
    for (int i = 0; tiers && i < GRASS_TIERS; i++) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(tiers, k_tier_names[i]);
        if (!e) continue;
        a->tier[i].full_m = jf(e, "full_m", a->tier[i].full_m);
        a->tier[i].end_m = jf(e, "end_m", a->tier[i].end_m);
        a->tier[i].shadow_m = jf(e, "shadow_m", a->tier[i].shadow_m);
        a->tier[i].density = jf(e, "density", a->tier[i].density);
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "wind");
    if (w) {
        a->wind_main = jf(w, "main", a->wind_main);
        a->wind_detail = jf(w, "detail", a->wind_detail);
        a->wind_flutter = jf(w, "flutter", a->wind_flutter);
    }
    cJSON_Delete(root);
    for (int i = 0; i < GRASS_TIERS; i++) a->tier[i].end_m = fmaxf(a->tier[i].end_m, a->tier[i].full_m + 1.0f);
    return true;
}

bool grass_asset_load(GrassAsset *a, const char *path, const char *name) {
    size_t sz;
    char *text = platform_read_file(path, &sz);
    if (!text) { log_error("grass: cannot read %s", path); return false; }
    bool ok = grass_asset_parse(a, text, name);
    free(text);
    return ok;
}

static int cmp_names(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

int grass_assets_load_dir(GrassAsset *out, int max) {
    char buf[512];
    const char *dir = platform_data_path(GRASS_DIR, buf, sizeof(buf));
    int n = 0, count = 0;
    char **files = SDL_GlobDirectory(dir, "*.json", 0, &count);
    if (!files) return 0;
    if (count > 1) qsort(files, (size_t)count, sizeof(char *), cmp_names);
    for (int i = 0; i < count && n < max; i++) {
        char path[640], name[48];
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        dg_strcpy(name, sizeof(name), files[i]);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        if (name[0] == '_') continue;
        if (grass_asset_load(&out[n], path, name)) n++;
    }
    SDL_free(files);
    return n;
}

// ------------------------------------------------------------------ mesh

static u32 g_rng;
static float rnd(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}
static u32 rgba(float r, float g, float b, float a) {
    float v[4] = { r, g, b, a };
    u32 out = 0;
    for (int k = 0; k < 4; k++) out |= (u32)lrintf(DG_CLAMP(v[k], 0.0f, 1.0f) * 255.0f) << (8 * k);
    return out;
}

static u32 push_vertex(VgFoliageMesh *m, const float p[3], const float n[3], float u, float v, u32 col) {
    if (m->vertex_count == m->vcap) {
        m->vcap = m->vcap ? m->vcap * 2 : 64;
        m->pos = (float *)realloc(m->pos, sizeof(float) * 3 * m->vcap);
        m->normal = (float *)realloc(m->normal, sizeof(float) * 3 * m->vcap);
        m->uv = (float *)realloc(m->uv, sizeof(float) * 2 * m->vcap);
        m->color = (u32 *)realloc(m->color, sizeof(u32) * m->vcap);
    }
    u32 i = m->vertex_count++;
    memcpy(&m->pos[i * 3], p, 12);
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int k = 0; k < 3; k++) m->normal[i * 3 + k] = l > 0 ? n[k] / l : (float)(k == 1);
    m->uv[i * 2] = u;
    m->uv[i * 2 + 1] = v;
    m->color[i] = col;
    return i;
}
static void push_tri(VgFoliageMesh *m, u32 a, u32 b, u32 c) {
    if (m->index_count + 3 > m->icap) {
        m->icap = m->icap ? m->icap * 2 : 192;
        m->indices = (u32 *)realloc(m->indices, sizeof(u32) * m->icap);
        m->tri_material = (u16 *)realloc(m->tri_material, sizeof(u16) * (m->icap / 3));
    }
    m->tri_material[m->index_count / 3] = 0;
    m->indices[m->index_count++] = a;
    m->indices[m->index_count++] = b;
    m->indices[m->index_count++] = c;
}

static float pos_phase(const float p[3]) {
    u32 h = (u32)(i32)floorf(p[0] * 7.0f) * 0x8DA6B343u ^ (u32)(i32)floorf(p[2] * 7.0f) * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (float)(h & 0xFFFFu) / 65536.0f;
}

void grass_mesh_extent(const GrassAsset *a, float *height, float *radius) {
    *height = a->height_m[1] * (1.0f + a->lean);
    *radius = a->width_m[1] * 0.5f + a->base_jitter_m + a->height_m[1] * (tanf(a->fan_deg * PI / 180.0f) + a->lean) + a->bend_m;
}

// Cards fanned around the clump centre: card k faces angle k pi / cards (+ jitter), leans outward by
// fan_deg along its normal (alternating sides), bends quadratically by up to bend_m at the tip; one quad
// column per card, `segments` rows (6 triangles at 3 rows). Uvs: the card's atlas cell (mirrored at
// random), v = 1 at the root (the cells' blades rise from their bottom edge).
void grass_gen_mesh(VgFoliageMesh *m, const GrassAsset *a, u32 variant) {
    memset(m, 0, sizeof(*m));
    g_rng = (u32)variant * 2654435761u + 0x9E37u;
    for (const char *c = a->name; *c; c++) g_rng = g_rng * 31u + (u8)*c;
    const float inset = 0.5f / 512.0f;   // keeps bilinear taps inside the cell
    for (int k = 0; k < a->cards; k++) {
        float ang = (float)k * PI / (float)a->cards + (rnd() - 0.5f) * 0.6f;
        float right[3] = { cosf(ang), 0.0f, sinf(ang) }, fwd[3] = { -sinf(ang), 0.0f, cosf(ang) };
        float side = (k & 1) ? 1.0f : -1.0f;
        float fan = tanf(a->fan_deg * PI / 180.0f) * (0.4f + 0.6f * rnd()) * side + (rnd() - 0.5f) * 2.0f * a->lean;
        float h = a->height_m[0] + (a->height_m[1] - a->height_m[0]) * rnd();
        float w = a->width_m[0] + (a->width_m[1] - a->width_m[0]) * rnd();
        float base[3] = { (rnd() - 0.5f) * 2.0f * a->base_jitter_m, 0.0f, (rnd() - 0.5f) * 2.0f * a->base_jitter_m };
        float bend = (rnd() - 0.5f) * 2.0f * a->bend_m;
        int cell = a->cells[(int)(rnd() * (float)a->cell_count) % a->cell_count];
        int cx = cell % a->cols, cy = cell / a->cols;
        float u0 = ((float)cx + inset) / (float)a->cols, u1 = ((float)cx + 1.0f - inset) / (float)a->cols;
        float v0 = ((float)cy + inset) / (float)a->rows, v1 = ((float)cy + 1.0f - inset) / (float)a->rows;   // v0 = cell top
        if (rnd() < a->mirror) { float t = u0; u0 = u1; u1 = t; }
        float card_hash = rnd();
        float phase = pos_phase(base) * 0.5f + rnd() * 0.5f;
        u32 first = m->vertex_count;
        int rows = a->segments;
        for (int j = 0; j <= rows; j++) {
            float t = (float)j / (float)rows;
            // the card leans along its normal (fan) and bends further toward the tip (quadratic)
            float off = fan * h * t + bend * t * t;
            for (int i = 0; i <= 1; i++) {
                float s = (float)i - 0.5f;
                float p[3];
                for (int q = 0; q < 3; q++) p[q] = base[q] + right[q] * s * w + fwd[q] * off;
                p[1] = h * t / sqrtf(1.0f + fan * fan);   // leaning keeps the card's length, not its height
                // normal: the card's (right x lean direction = fwd - fan up), blended to the clump dome
                float cn[3] = { fwd[0], -fan, fwd[2] };
                float dome[3] = { p[0] * 1.5f, 1.0f, p[2] * 1.5f };
                float dl = sqrtf(dome[0] * dome[0] + dome[1] * dome[1] + dome[2] * dome[2]);
                float n[3];
                for (int q = 0; q < 3; q++) n[q] = cn[q] * (1.0f - a->normal_dome) + dome[q] / dl * a->normal_dome;
                float u = i ? u1 : u0, v = v1 + (v0 - v1) * t;
                push_vertex(m, p, n, u, v, rgba(t, phase, 0.0f, card_hash));
            }
        }
        for (int j = 0; j < rows; j++) {
            u32 b0 = first + (u32)(j * 2), b1 = b0 + 1, t0 = b0 + 2, t1 = b0 + 3;
            push_tri(m, b0, b1, t1);
            push_tri(m, b0, t1, t0);
        }
    }
    m->material_count = 1;
}

// ------------------------------------------------------------------ material / shading parameters

void grass_material(const GrassAsset *a, int tier, u32 cover_id, VgMaterial *out) {
    tier = DG_CLAMP(tier, 0, GRASS_TIERS - 1);
    memset(out, 0, sizeof(*out));
    out->color[0] = out->color[1] = out->color[2] = out->color[3] = 1.0f;
    out->tex_scale = 1.0f;
    out->roughness = a->roughness;
    out->flags = VGM_FOLIAGE | VGM_MASKED | VGM_TWO_SIDED | VGM_DITHER_FADE | MAT_TEXTURED | MAT_COVER | ((cover_id & 7u) << MAT_COVER_ID_SHIFT);
    out->alpha_cutoff = a->alpha_cutoff;
    out->wpo = WPO_GRASS | WPO_DISPLACE;
    VgWindParams w;
    vgeo_wind_defaults(&w, VGWP_GRASS, a->height_m[1]);
    w.trunk_stiffness *= a->wind_main;
    w.branch_stiffness *= a->wind_detail;
    w.leaf_stiffness *= a->wind_flutter;
    vgeo_wind_pack(&w, out->wpo_params, out->wind_params);
    const GrassTier *t = &a->tier[tier];
    out->fade_start = t->full_m;
    out->fade_end = t->end_m;
    // shadow_max_dist 0 means "always": a level without grass shadows gets a distance nothing is under
    out->shadow_max_dist = t->shadow_m > 0 ? t->shadow_m : 0.01f;
    out->shadow_class = (u32)a->shadow_class;
    vg_material_set_ao(out, a->ao_strength, a->ao_combine, false);
}

void grass_cover_params(const GrassAsset *a, float out[12]) {
    float v[12] = { a->root_dark, a->root_extent, a->ground_match, a->ground_extent, a->var_hue * PI / 180.0f, a->var_sat, a->var_val, a->macro,
                    a->sss, a->sss_wrap, a->specular, a->ao_root };
    memcpy(out, v, sizeof(v));
}

static u32 hash2(i32 x, i32 z, u32 seed) {
    u32 h = (u32)x * 73856093u ^ (u32)z * 19349663u ^ seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}
static float vnoise(float x, float z, u32 seed) {
    float fx = floorf(x), fz = floorf(z), tx = x - fx, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
    float v[4];
    for (int k = 0; k < 4; k++) v[k] = (float)(hash2((i32)fx + (k & 1), (i32)fz + (k >> 1), seed) & 0xFFFFu) / 65535.0f;
    return (v[0] * (1 - tx) + v[1] * tx) * (1 - tz) + (v[2] * (1 - tx) + v[3] * tx) * tz;
}

// Clumps vs bare ground: two octaves of value noise at clump_scale_m; below `bare` nothing grows, above it
// the keep rate rises to 1 (amount 0: uniform).
float grass_clump_keep(const GrassAsset *a, float x, float z, u32 seed) {
    float s = 1.0f / fmaxf(a->clump_scale_m, 0.1f);
    float n = 0.65f * vnoise(x * s, z * s, seed) + 0.35f * vnoise(x * s * 2.7f + 19.1f, z * s * 2.7f - 7.3f, seed + 1u);
    float t = DG_CLAMP((n - a->bare) / fmaxf(1.0f - a->bare, 1e-3f), 0.0f, 1.0f);
    float keep = t * t * (3 - 2 * t);
    return 1.0f - a->clump_amount + a->clump_amount * keep;
}

float grass_expected_density(const GrassAsset *a, int mat, int tier) {
    if (mat < 0 || mat >= GRASS_MAX_MATERIALS) return 0;
    // mean keep rate over a large area (numerical: the noise is stationary)
    double acc = 0;
    int n = 0;
    for (int z = 0; z < 64; z++)
        for (int x = 0; x < 64; x++, n++) acc += grass_clump_keep(a, (float)x * 1.37f, (float)z * 1.37f, 7u);
    return a->density[mat] * a->tier[DG_CLAMP(tier, 0, GRASS_TIERS - 1)].density * (float)(acc / n);
}
