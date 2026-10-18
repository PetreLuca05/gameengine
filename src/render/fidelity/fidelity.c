#include "fidelity.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ registry

#define B FID_BOOL
#define I FID_INT
#define F FID_FLOAT
// id, cvar, kind, has_off, off, lo, hi, cheaper_high, era_owned, label
static const FidFeatureDef k_features[FF_COUNT] = {
    [FF_AA] = { "aa", "r_aa", I, true, 0, 0, 3, false, false, "Anti-aliasing" },
    [FF_UPSCALER] = { "upscaler", "r_upscaler", I, true, 0, 0, 4, false, false, "Upscaler" },
    [FF_UPSCALE_RATIO] = { "upscale_ratio", "r_upscale_ratio", F, false, 0, 0.5f, 1.0f, false, false, "Upscaler resolution" },
    [FF_RENDER_SCALE] = { "render_scale", "r_render_scale", F, false, 0, 0.25f, 2.0f, false, false, "Render scale" },
    [FF_SHARPEN] = { "sharpen", "r_sharpen", F, true, 0, 0, 1, false, false, "Sharpening" },
    [FF_AO] = { "ao", "r_ao", I, true, 0, 0, 2, false, false, "Ambient occlusion" },
    [FF_CONTACT_SHADOWS] = { "contact_shadows", "r_contact_shadows", I, true, 0, 0, 2, false, false, "Contact shadows" },
    [FF_SSR] = { "ssr", "r_ssr", I, true, 0, 0, 2, false, false, "Screen-space reflections" },
    [FF_SSGI] = { "ssgi", "r_ssgi", B, true, 0, 0, 1, false, false, "Screen-space global illumination" },
    [FF_PUDDLE_REFLECTIONS] = { "puddle_reflections", "r_puddle_reflections", B, true, 0, 0, 1, false, false, "Puddle reflections" },
    [FF_VOLUMETRICS] = { "volumetrics", "r_volumetrics", I, true, 0, 0, 3, false, false, "Volumetric fog" },
    [FF_VOL_LOCAL_LIGHTS] = { "vol_local_lights", "r_vol_local_lights", B, true, 0, 0, 1, false, false, "Light scattering (local lights)" },
    [FF_MOTION_BLUR] = { "motion_blur", "r_motion_blur", I, true, 0, 0, 2, false, false, "Motion blur" },
    [FF_DOF] = { "dof", "r_dof", I, true, 0, 0, 2, false, false, "Depth of field" },
    [FF_DOF_QUALITY] = { "dof_quality", "r_dof_quality", I, false, 0, 0, 2, false, false, "Depth of field quality" },
    [FF_ADS_DOF] = { "ads_dof", "ads_dof", B, true, 0, 0, 1, false, false, "Depth of field while aiming" },
    [FF_BLOOM] = { "bloom", "r_bloom", B, true, 0, 0, 1, false, false, "Bloom" },
    [FF_BLOOM_QUALITY] = { "bloom_quality", "r_bloom_quality", I, false, 0, 0, 2, false, false, "Bloom quality" },
    [FF_LENS_FLARE] = { "lens_flare", "r_lens_flare", F, true, 0, 0, 2, false, false, "Lens flare" },
    [FF_LENS_DIRT] = { "lens_dirt", "r_lens_dirt", F, true, 0, 0, 2, false, false, "Lens dirt" },
    [FF_CHROMATIC] = { "chromatic", "r_chromatic", F, true, 0, 0, 2, false, false, "Chromatic aberration" },
    [FF_LENS_DISTORTION] = { "lens_distortion", "r_lens_distortion", F, true, 0, -1, 1, false, false, "Lens distortion" },
    [FF_LENS_DROPS] = { "lens_drops", "r_lens_drops", B, true, 0, 0, 1, false, false, "Lens droplets" },
    [FF_VIGNETTE] = { "vignette", "r_vignette", F, true, 0, 0, 1, false, false, "Vignette" },
    [FF_FILM_GRAIN] = { "film_grain", "r_film_grain", F, true, 0, 0, 1, false, false, "Film grain" },
    [FF_AUTO_EXPOSURE] = { "auto_exposure", "r_auto_exposure", B, true, 0, 0, 1, false, false, "Auto exposure" },
    [FF_TONEMAPPER] = { "tonemapper", "r_tonemapper", I, false, 0, 0, 2, false, false, "Tonemapper" },
    [FF_HDR_OUTPUT] = { "hdr_output", "r_hdr_output", B, true, 0, 0, 1, false, false, "HDR10 output" },
    [FF_SHADOWS] = { "shadows", "r_shadows", I, true, 0, 0, 2, false, false, "Shadows" },
    [FF_SHADOW_RESOLUTION] = { "shadow_resolution", "r_shadow_resolution", I, false, 0, 0, 3, false, false, "Shadow resolution" },
    [FF_SHADOW_PROJECTION] = { "shadow_projection", "r_shadow_projection", I, false, 0, 0, 1, false, false, "Shadow projection" },
    [FF_SHADOW_CASCADES] = { "shadow_cascades", "r_shadow_cascades", I, false, 0, 1, 4, false, false, "Shadow cascades" },
    [FF_SHADOW_DISTANCE] = { "shadow_distance", "r_shadow_max_distance", F, false, 0, 10.0f, 1000.0f, false, false, "Shadow distance" },
    [FF_SOFT_SHADOW_QUALITY] = { "soft_shadow_quality", "r_soft_shadow_quality", I, false, 0, 0, 3, false, false, "Soft shadow quality" },
    [FF_SHADOW_MIN_RADIUS] = { "shadow_min_radius", "r_shadow_min_radius", F, false, 0, 0, 4, true, false, "Small object shadows" },
    [FF_LIGHT_SHADOWS] = { "light_shadows", "r_light_shadows", B, true, 0, 0, 1, false, false, "Point / spot light shadows" },
    [FF_LIGHT_SHADOW_QUALITY] = { "light_shadow_quality", "r_light_shadow_quality", I, false, 0, 1, 2, false, false, "Light shadow quality" },
    [FF_CLOUD_SHADOWS] = { "cloud_shadows", "r_cloud_shadows", B, true, 0, 0, 1, false, false, "Cloud shadows" },
    [FF_MOON_SHADOWS] = { "moon_shadows", "r_moon_shadows", B, true, 0, 0, 1, false, false, "Moon shadows at twilight" },
    [FF_TEXTURE_QUALITY] = { "texture_quality", "r_texture_quality", I, false, 0, 0, 2, false, false, "Textures" },
    [FF_ANISO] = { "aniso", "r_aniso", I, false, 0, 1, 16, false, false, "Anisotropic filtering" },
    [FF_VGEO] = { "vgeo", "r_vgeo", B, true, 0, 0, 1, false, true, "Virtualized geometry" },
    [FF_VGEO_ERROR_PX] = { "vgeo_error_px", "r_vgeo_error_px", F, false, 0, 0.25f, 16, true, true, "Geometry detail (vgeo error)" },
    [FF_VGEO_SHADOW_ERROR_PX] = { "vgeo_shadow_error_px", "r_vgeo_shadow_error_px", F, false, 0, 0.25f, 32, true, true, "Shadow geometry detail" },
    [FF_VGEO_LOD_PX] = { "vgeo_lod_px", "r_vgeo_lod_px", F, false, 0, 0.25f, 64, true, true, "Discrete LOD error" },
    [FF_FOLIAGE_DENSITY] = { "foliage_density", "r_foliage_density", F, false, 0, 0, 1, false, true, "Foliage density" },
    [FF_FOLIAGE_WIND] = { "foliage_wind", "r_foliage_wind", I, true, 0, 0, 3, false, false, "Foliage wind" },
    [FF_IMPOSTOR_PX] = { "impostor_px", "r_vgeo_impostor_px", F, false, 0, 0, 256, true, false, "Impostor distance" },
    [FF_TERRAIN_QUALITY] = { "terrain_quality", "r_terrain_quality", I, false, 0, 0, 4, false, false, "Terrain detail" },
    [FF_LOD_BIAS] = { "lod_bias", "r_lod_bias", F, false, 0, 0.25f, 4, false, false, "Character LOD detail" },
    [FF_CHAR_QUALITY] = { "char_quality", "r_char_quality", I, false, 0, 0, 3, false, false, "Character detail" },
    [FF_SCOPE_PIP_SCALE] = { "scope_pip_scale", "r_scope_pip_scale", F, false, 0, 0.2f, 2, false, false, "Scope picture resolution" },
    [FF_VFX_QUALITY] = { "vfx_quality", "vfx_quality", I, false, 0, 0, 3, false, false, "Effects (particles)" },
    [FF_VFX_LIGHTING] = { "vfx_lighting", "vfx_lighting", B, true, 0, 0, 1, false, false, "Lit particles" },
    [FF_DECAL_MAX] = { "decal_max", "vfx_decal_max", I, false, 0, 0, 4096, false, false, "Impact decals" },
    [FF_PRECIP_QUALITY] = { "precip_quality", "r_precip_quality", I, false, 0, -1, 3, false, false, "Rain / snow quality" },
    [FF_WATER] = { "water", "r_water", I, false, 0, 0, 3, false, false, "Water quality" },
};
#undef B
#undef I
#undef F

// Locked features (docs/SETTINGS_AUDIT.md): their cvars are CVAR_LOCKED, one value for every level
// and era (assets/render/consistency.json). The tables must not list them (fid_tables_complete);
// their bounds are that single value, and nothing here writes them.
static const int k_locked[] = { FF_FOLIAGE_DENSITY, FF_IMPOSTOR_PX, FF_VGEO_ERROR_PX, FF_VGEO_LOD_PX, FF_LOD_BIAS, FF_CHAR_QUALITY, FF_SHADOW_DISTANCE,
                                FF_CLOUD_SHADOWS, FF_LIGHT_SHADOWS, FF_MOON_SHADOWS, FF_DECAL_MAX, FF_LENS_DROPS, FF_AUTO_EXPOSURE, FF_LENS_FLARE,
                                FF_LENS_DIRT, FF_LENS_DISTORTION, FF_TONEMAPPER };
bool fid_feature_locked(int f) {
    for (int i = 0; i < ARRAY_COUNT(k_locked); i++) if (k_locked[i] == f) return true;
    return false;
}

// Features whose values have no cost order (a choice, not a tier): an explicit user choice is kept
// when the new level allows it.
static bool unordered(int f) { return f == FF_UPSCALER || f == FF_TONEMAPPER || f == FF_PRECIP_QUALITY || f == FF_LENS_DISTORTION; }

static const char *k_level_ids[FID_LEVELS] = { "off", "low", "medium", "high", "modern" };
static const char *k_level_names[FID_LEVELS] = { "Off", "Low", "Medium", "High", "Modern" };
static const char *k_era_ids[FID_ERAS] = { "modern", "2020", "2015", "2010" };

int fid_feature_count(void) { return FF_COUNT; }
const FidFeatureDef *fid_feature(int f) { return f >= 0 && f < FF_COUNT ? &k_features[f] : NULL; }
int fid_feature_find(const char *id) {
    for (int f = 0; f < FF_COUNT; f++) if (!strcmp(k_features[f].id, id)) return f;
    return -1;
}
int fid_feature_by_cvar(const char *cvar) {
    for (int f = 0; f < FF_COUNT; f++) if (!strcmp(k_features[f].cvar, cvar)) return f;
    return -1;
}
const char *fid_level_id(int l) { return l >= 0 && l < FID_LEVELS ? k_level_ids[l] : "?"; }
const char *fid_level_name(int l) { return l >= 0 && l < FID_LEVELS ? k_level_names[l] : "?"; }
const char *fid_era_id(int e) { return e >= 0 && e < FID_ERAS ? k_era_ids[e] : "?"; }
int fid_era_from_cvar(int r_era) { return r_era >= 0 && r_era < FID_ERAS ? r_era : FID_ERA_MODERN; }

int fid_level_parse(const char *s) {
    if (!s || !s[0]) return -1;
    for (int l = 0; l < FID_LEVELS; l++)
        if (!strcmp(s, k_level_ids[l]) || !strcmp(s, k_level_names[l])) return l;
    if (s[0] >= '0' && s[0] <= '4' && !s[1]) return s[0] - '0';
    return -1;
}

// ------------------------------------------------------------------ tables

enum { E_AVAIL = 1, E_DEF = 2, E_MIN = 4, E_MAX = 8, E_ALLOWED = 16, E_SCALE = 32 };
typedef struct FidEntry { u32 set; FidAvail avail; float def, min, max, scale; u32 allowed; } FidEntry;

typedef struct FidLevelData {
    bool loaded;
    char name[24], desc[200];
    float budget_ms;
    bool post;
    FidEntry base[FF_COUNT];
    FidEntry era[FID_ERAS][FF_COUNT];   // per-era overrides (partial)
} FidLevelData;

static struct {
    FidLevelData lv[FID_LEVELS];
    bool eras_loaded;
    bool lacks[FID_ERAS][FF_COUNT];
    FidEntry limit[FID_ERAS][FF_COUNT];   // era capability limits (min / max / allowed only)
    float era_value[FID_ERAS][FF_COUNT];
    bool era_value_set[FID_ERAS][FF_COUNT];
    // runtime
    int level;
    bool era_locked;
    int era_lock;
    bool attached;
    Cvar *cv[FF_COUNT];
    Cvar *r_era;
    // Values the user set (menu, console, presets) away from the level default: re-applied by
    // fid_apply where they are below the new level's default. Persisted in r_fidelity_custom.
    bool tracking, internal;
    bool user_set[FF_COUNT];
    float user_val[FF_COUNT];
} S = { .level = FID_MODERN, .tracking = true };

void fid_reset_tables(void) {
    memset(S.lv, 0, sizeof(S.lv));
    memset(S.lacks, 0, sizeof(S.lacks));
    memset(S.limit, 0, sizeof(S.limit));
    S.eras_loaded = false;
}

const char *fid_level_desc(int l) { return l >= 0 && l < FID_LEVELS ? S.lv[l].desc : ""; }
float fid_level_budget_ms(int l) { return l >= 0 && l < FID_LEVELS ? S.lv[l].budget_ms : 0.0f; }
bool fid_level_post(int l) { return l >= 0 && l < FID_LEVELS ? S.lv[l].post : true; }
bool fid_loaded(void) {
    for (int l = 0; l < FID_LEVELS; l++) if (!S.lv[l].loaded) return false;
    return S.eras_loaded;
}

void fid_set_era_value(int era, int f, float v) {
    if (era < 0 || era >= FID_ERAS || f < 0 || f >= FF_COUNT) return;
    S.era_value[era][f] = v;
    S.era_value_set[era][f] = true;
}
float fid_era_value(int era, int f) {
    if (era < 0 || era >= FID_ERAS || f < 0 || f >= FF_COUNT) return NAN;
    if (S.era_value_set[era][f]) return S.era_value[era][f];
    // Unregistered: the cvar's registered default (modern behaviour), else the top of the range.
    if (S.cv[f]) return (float)atof(S.cv[f]->default_str);
    return k_features[f].hi;
}

static void errf(char *err, int n, const char *fmt, const char *a, const char *b) {
    if (err && n > 0) snprintf(err, (size_t)n, fmt, a, b);
}

static bool parse_entry(const cJSON *o, FidEntry *e, const char *where, const char *fid, char *err, int n) {
    if (!cJSON_IsObject(o)) { errf(err, n, "%s: '%s' must be an object", where, fid); return false; }
    const cJSON *it;
    cJSON_ArrayForEach(it, o) {
        const char *k = it->string;
        if (!strcmp(k, "note")) continue;
        if (!strcmp(k, "avail")) {
            const char *v = cJSON_IsString(it) ? it->valuestring : "";
            if (!strcmp(v, "no")) e->avail = FID_UNAVAILABLE;
            else if (!strcmp(v, "off")) e->avail = FID_DEFAULT_OFF;
            else if (!strcmp(v, "on")) e->avail = FID_DEFAULT_ON;
            else { errf(err, n, "%s: '%s' avail must be no / off / on", where, fid); return false; }
            e->set |= E_AVAIL;
        } else if (!strcmp(k, "default") || !strcmp(k, "min") || !strcmp(k, "max") || !strcmp(k, "scale")) {
            if (!cJSON_IsNumber(it)) { errf(err, n, "%s: '%s' needs numbers", where, fid); return false; }
            float v = (float)it->valuedouble;
            if (k[0] == 'd') { e->def = v; e->set |= E_DEF; }
            else if (!strcmp(k, "min")) { e->min = v; e->set |= E_MIN; }
            else if (!strcmp(k, "max")) { e->max = v; e->set |= E_MAX; }
            else { e->scale = v; e->set |= E_SCALE; }
        } else if (!strcmp(k, "allowed")) {
            if (!cJSON_IsArray(it)) { errf(err, n, "%s: '%s' allowed must be an array", where, fid); return false; }
            e->allowed = 0;
            const cJSON *a;
            cJSON_ArrayForEach(a, it) {
                int v = cJSON_IsNumber(a) ? (int)a->valuedouble : -100;
                if (v < FID_INT_BASE || v > 30) { errf(err, n, "%s: '%s' allowed values must be -1 .. 30", where, fid); return false; }
                e->allowed |= 1u << (v - FID_INT_BASE);
            }
            e->set |= E_ALLOWED;
        } else {
            char what[96];
            snprintf(what, sizeof(what), "%s.%s", fid, k);
            errf(err, n, "%s: unknown key '%s'", where, what);
            return false;
        }
    }
    return true;
}

static bool parse_features(const cJSON *obj, FidEntry *out, const char *where, char *err, int n) {
    const cJSON *it;
    cJSON_ArrayForEach(it, obj) {
        int f = fid_feature_find(it->string);
        if (f < 0) { errf(err, n, "%s: unknown feature '%s'", where, it->string); return false; }
        if (!parse_entry(it, &out[f], where, it->string, err, n)) return false;
    }
    return true;
}

static int era_index(const char *s) {
    for (int e = 0; e < FID_ERAS; e++) if (!strcmp(s, k_era_ids[e])) return e;
    return -1;
}

bool fid_load_level_text(int level, const char *json, char *err, int n) {
    if (level < 0 || level >= FID_LEVELS) return false;
    const char *where = k_level_ids[level];
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { errf(err, n, "%s: JSON parse error%s", where, ""); return false; }
    FidLevelData *L = &S.lv[level];
    memset(L, 0, sizeof(*L));
    bool ok = true;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "level");
    if (!cJSON_IsString(id) || strcmp(id->valuestring, where)) { errf(err, n, "%s: \"level\" must be \"%s\"", where, where); ok = false; }
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
    dg_strcpy(L->name, sizeof(L->name), cJSON_IsString(nm) ? nm->valuestring : k_level_names[level]);
    const cJSON *ds = cJSON_GetObjectItemCaseSensitive(root, "description");
    dg_strcpy(L->desc, sizeof(L->desc), cJSON_IsString(ds) ? ds->valuestring : "");
    const cJSON *bm = cJSON_GetObjectItemCaseSensitive(root, "budget_ms");
    L->budget_ms = cJSON_IsNumber(bm) ? (float)bm->valuedouble : 0.0f;
    const cJSON *po = cJSON_GetObjectItemCaseSensitive(root, "post");
    L->post = !cJSON_IsBool(po) || cJSON_IsTrue(po);
    const cJSON *fe = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (ok && !cJSON_IsObject(fe)) { errf(err, n, "%s: missing \"features\"%s", where, ""); ok = false; }
    if (ok) ok = parse_features(fe, L->base, where, err, n);
    const cJSON *er = cJSON_GetObjectItemCaseSensitive(root, "eras");
    const cJSON *it;
    if (ok && er) cJSON_ArrayForEach(it, er) {
        int e = era_index(it->string);
        if (e < 0) { errf(err, n, "%s: unknown era '%s'", where, it->string); ok = false; break; }
        char w[48];
        snprintf(w, sizeof(w), "%s (era %s)", where, it->string);
        if (!parse_features(it, L->era[e], w, err, n)) { ok = false; break; }
    }
    cJSON_Delete(root);
    L->loaded = ok;
    return ok;
}

bool fid_load_eras_text(const char *json, char *err, int n) {
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { errf(err, n, "eras: JSON parse error%s%s", "", ""); return false; }
    memset(S.lacks, 0, sizeof(S.lacks));
    memset(S.limit, 0, sizeof(S.limit));
    bool ok = true;
    const cJSON *eras = cJSON_GetObjectItemCaseSensitive(root, "eras"), *it;
    if (!cJSON_IsObject(eras)) { errf(err, n, "eras: missing \"eras\"%s%s", "", ""); ok = false; }
    if (ok) cJSON_ArrayForEach(it, eras) {
        int e = era_index(it->string);
        if (e < 0) { errf(err, n, "eras: unknown era '%s'%s", it->string, ""); ok = false; break; }
        const cJSON *l, *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(it, "lacks")) {
            int f = cJSON_IsString(a) ? fid_feature_find(a->valuestring) : -1;
            if (f < 0) { errf(err, n, "eras: %s lacks an unknown feature '%s'", it->string, cJSON_IsString(a) ? a->valuestring : "?"); ok = false; break; }
            S.lacks[e][f] = true;
        }
        l = cJSON_GetObjectItemCaseSensitive(it, "limits");
        if (ok && l) ok = parse_features(l, S.limit[e], it->string, err, n);
        if (!ok) break;
    }
    cJSON_Delete(root);
    S.eras_loaded = ok;
    return ok;
}

bool fid_load_dir(const char *dir, char *err, int n) {
    char path[512];
    bool ok = true;
    for (int l = 0; l < FID_LEVELS && ok; l++) {
        snprintf(path, sizeof(path), "%s/%s.json", dir, k_level_ids[l]);
        char *text = platform_read_file(path, NULL);
        if (!text) { errf(err, n, "cannot read %s%s", path, ""); return false; }
        ok = fid_load_level_text(l, text, err, n);
        free(text);
    }
    if (!ok) return false;
    snprintf(path, sizeof(path), "%s/eras.json", dir);
    char *text = platform_read_file(path, NULL);
    if (!text) { errf(err, n, "cannot read %s%s", path, ""); return false; }
    ok = fid_load_eras_text(text, err, n);
    free(text);
    return ok && fid_tables_complete(err, n);
}

// ------------------------------------------------------------------ bounds

static u32 bit(float v) {
    int i = (int)lroundf(v);
    return (i < FID_INT_BASE || i > 30) ? 0u : 1u << (i - FID_INT_BASE);
}

static u32 mask_range(float lo, float hi) {
    u32 m = 0;
    for (int v = FID_INT_BASE; v <= 30; v++) if ((float)v >= lo - 1e-4f && (float)v <= hi + 1e-4f) m |= 1u << (v - FID_INT_BASE);
    return m;
}

static bool int_kind(const FidFeatureDef *d) { return d->kind != FID_FLOAT; }
static bool masked(const FidFeatureDef *d) { return int_kind(d) && d->hi <= 30; }

static void merge(FidEntry *dst, const FidEntry *src) {
    if (src->set & E_AVAIL) dst->avail = src->avail;
    if (src->set & E_DEF) dst->def = src->def;
    if (src->set & E_MIN) dst->min = src->min;
    if (src->set & E_MAX) dst->max = src->max;
    if (src->set & E_ALLOWED) dst->allowed = src->allowed;
    if (src->set & E_SCALE) dst->scale = src->scale;
    dst->set |= src->set;
}

bool fid_allowed(const FidBounds *b, const FidFeatureDef *d, float v) {
    if (v < b->lo - 1e-4f || v > b->hi + 1e-4f) return false;
    if (!int_kind(d)) return true;
    if (fabsf(v - roundf(v)) > 1e-4f) return false;
    return !masked(d) || (b->allowed & bit(v)) != 0;
}

FidBounds fid_bounds(int level, int era, int f) {
    FidBounds b;
    memset(&b, 0, sizeof(b));
    if (f < 0 || f >= FF_COUNT) return b;
    const FidFeatureDef *d = &k_features[f];
    if (fid_feature_locked(f)) {
        // The locked value (its cvar's default once the consistency set is applied).
        Cvar *c = S.cv[f] ? S.cv[f] : cvar_find(d->cvar);
        float v = c ? (float)atof(c->default_str) : d->hi;
        b.avail = FID_DEFAULT_ON;
        b.lo = b.hi = b.def = v;
        b.allowed = masked(d) ? bit(v) : ~0u;
        return b;
    }
    level = DG_CLAMP(level, 0, FID_LEVELS - 1);
    era = DG_CLAMP(era, 0, FID_ERAS - 1);
    FidEntry e = S.lv[level].base[f];
    merge(&e, &S.lv[level].era[era][f]);
    b.avail = (e.set & E_AVAIL) ? e.avail : FID_DEFAULT_ON;
    b.lo = d->lo;
    b.hi = d->hi;
    if (e.set & E_MIN) b.lo = fmaxf(b.lo, e.min);
    if (e.set & E_MAX) b.hi = fminf(b.hi, e.max);
    // The era's capability limits narrow every level.
    const FidEntry *lim = &S.limit[era][f];
    if (lim->set & E_MIN) b.lo = fmaxf(b.lo, lim->min);
    if (lim->set & E_MAX) b.hi = fminf(b.hi, lim->max);
    b.allowed = masked(d) ? mask_range(b.lo, b.hi) : ~0u;
    if (masked(d) && (e.set & E_ALLOWED)) b.allowed &= e.allowed;
    if (masked(d) && (lim->set & E_ALLOWED)) b.allowed &= lim->allowed;
    if (S.lacks[era][f]) { b.avail = FID_UNAVAILABLE; b.era_lacks = true; }
    // Default.
    float def;
    if (d->era_owned) {
        float ev = fid_era_value(era, f);
        float scale = (e.set & E_SCALE) ? e.scale : 1.0f;
        def = (e.set & E_DEF) ? e.def : ev * scale;
        if ((e.set & E_SCALE) && d->cheaper_high) b.lo = fmaxf(b.lo, ev * scale);   // "x3 the era's error threshold" is a floor
    } else {
        def = (e.set & E_DEF) ? e.def : (d->kind == FID_BOOL && b.avail == FID_DEFAULT_ON ? 1.0f : (d->has_off ? d->off : b.hi));
    }
    if (b.avail == FID_DEFAULT_OFF && d->has_off) def = d->off;
    if (b.avail == FID_UNAVAILABLE && d->has_off) {
        b.lo = b.hi = def = d->off;
        b.allowed = masked(d) ? bit(d->off) : ~0u;
    } else if (b.avail == FID_DEFAULT_OFF && d->has_off) {
        // Off must stay selectable.
        b.lo = fminf(b.lo, d->off);
        if (masked(d)) b.allowed |= bit(d->off);
    }
    if (b.lo > b.hi) {
        if (b.lo > d->hi) b.lo = d->hi;   // a scaled floor past the cvar's range: its top
        b.hi = b.lo;
    }
    b.def = def;
    b.def = fid_clamp(&b, d, def);
    return b;
}

int fid_cost_cmp(const FidFeatureDef *d, float a, float b) {
    if (d == &k_features[FF_LENS_DISTORTION]) { a = fabsf(a); b = fabsf(b); }
    if (fabsf(a - b) < 1e-4f) return 0;
    bool lt = d->cheaper_high ? a > b : a < b;
    return lt ? -1 : 1;
}

float fid_clamp(const FidBounds *b, const FidFeatureDef *d, float v) {
    if (isnan(v)) return b->def;
    float c = DG_CLAMP(v, b->lo, b->hi);
    if (!int_kind(d)) return c;
    float r = roundf(c);
    if (!masked(d) || (b->allowed & bit(r))) return r;
    int f = (int)(d - k_features);
    if (f >= 0 && f < FF_COUNT && unordered(f)) {
        if (b->allowed & bit(b->def)) return b->def;
        for (int i = FID_INT_BASE; i <= 30; i++) if (b->allowed & bit((float)i)) return (float)i;   // a choice: the first allowed one
    }
    // The nearest allowed value below (cheaper), else the nearest above.
    for (int i = (int)r; i >= FID_INT_BASE; i--) if (b->allowed & bit((float)i)) return (float)i;
    for (int i = (int)r; i <= 30; i++) if (b->allowed & bit((float)i)) return (float)i;
    return b->def;
}

bool fid_value_allowed(int level, int era, int f, float v) {
    FidBounds b = fid_bounds(level, era, f);
    if (b.avail == FID_UNAVAILABLE && !k_features[f].has_off) return false;   // locked
    return fid_allowed(&b, &k_features[f], v);
}

int fid_min_level_for(int era, int f, float v) {
    for (int l = 0; l < FID_LEVELS; l++) if (fid_value_allowed(l, era, f, v)) return l;
    return -1;
}

bool fid_tables_complete(char *err, int n) {
    if (!fid_loaded()) { errf(err, n, "fidelity tables not loaded%s%s", "", ""); return false; }
    for (int l = 0; l < FID_LEVELS; l++)
        for (int f = 0; f < FF_COUNT; f++) {
            const FidEntry *e = &S.lv[l].base[f];
            const FidFeatureDef *d = &k_features[f];
            bool in_era = false;
            for (int era = 0; era < FID_ERAS; era++) in_era |= S.lv[l].era[era][f].set != 0 || S.limit[era][f].set != 0 || S.lacks[era][f];
            if (fid_feature_locked(f)) {
                if (e->set || in_era) { errf(err, n, "%s: '%s' is locked (assets/render/consistency.json): a tier table must not set it", k_level_ids[l], d->id); return false; }
                continue;
            }
            if (!e->set) { errf(err, n, "%s: no entry for '%s'", k_level_ids[l], d->id); return false; }
            bool avail = !(e->set & E_AVAIL) || e->avail != FID_UNAVAILABLE;
            if (avail && e->avail != FID_DEFAULT_OFF && !d->era_owned && d->kind != FID_BOOL && !(e->set & E_DEF)) {
                errf(err, n, "%s: '%s' is available but has no default", k_level_ids[l], d->id);
                return false;
            }
        }
    for (int era = 0; era < FID_ERAS; era++)
        for (int f = 0; f < FF_COUNT; f++) {
            const FidFeatureDef *d = &k_features[f];
            if (fid_feature_locked(f)) continue;
            for (int l = 0; l < FID_LEVELS; l++) {
                FidBounds b = fid_bounds(l, era, f);
                const FidEntry *e = &S.lv[l].base[f];
                // An explicit default must lie inside the level's own bounds (era-owned: derived).
                FidEntry m = *e;
                merge(&m, &S.lv[l].era[era][f]);
                // (Era capability limits clamp the modern defaults silently: checked for the modern era
                // and for defaults an era block sets itself.)
                bool own = era == FID_ERA_MODERN || (S.lv[l].era[era][f].set & E_DEF);
                if (own && (m.set & E_DEF) && b.avail == FID_DEFAULT_ON && !b.era_lacks && !fid_allowed(&b, d, m.def)) {
                    char w[80];
                    snprintf(w, sizeof(w), "%s/%s", k_level_ids[l], k_era_ids[era]);
                    errf(err, n, "%s: default of '%s' outside its bounds", w, d->id);
                    return false;
                }
                if (l == 0) continue;
                // Monotonic: a higher level never takes a feature away or lowers its ceiling.
                FidBounds p = fid_bounds(l - 1, era, f);
                char w[80];
                snprintf(w, sizeof(w), "%s/%s", k_level_ids[l], k_era_ids[era]);
                if (p.avail != FID_UNAVAILABLE && b.avail == FID_UNAVAILABLE) { errf(err, n, "%s: '%s' unavailable but available one level lower", w, d->id); return false; }
                if (!unordered(f) && b.avail != FID_UNAVAILABLE && p.avail != FID_UNAVAILABLE) {
                    bool worse = d->cheaper_high ? b.lo > p.lo + 1e-4f : b.hi < p.hi - 1e-4f;
                    if (worse) { errf(err, n, "%s: '%s' bound tighter than one level lower", w, d->id); return false; }
                }
            }
        }
    return true;
}

// ------------------------------------------------------------------ runtime

int fid_level(void) { return S.level; }
int fid_era(void) {
    if (S.era_locked) return S.era_lock;
    if (!S.r_era) S.r_era = cvar_find("r_era");
    return S.r_era ? fid_era_from_cvar(S.r_era->i) : FID_ERA_MODERN;
}

static void fmt_value(const FidFeatureDef *d, float v, char *buf, size_t n) {
    if (int_kind(d)) snprintf(buf, n, "%d", (int)lroundf(v));
    else snprintf(buf, n, "%g", v);
}

float fid_cvar_value(int f) {
    Cvar *c = f >= 0 && f < FF_COUNT ? S.cv[f] : NULL;
    if (!c && f >= 0 && f < FF_COUNT) c = cvar_find(k_features[f].cvar);
    if (!c) return NAN;
    return k_features[f].kind == FID_FLOAT ? c->f : (float)c->i;
}

float fid_clamp_current(int f, float v) {
    if (!fid_loaded() || f < 0 || f >= FF_COUNT) return v;
    FidBounds b = fid_bounds(S.level, fid_era(), f);
    return fid_clamp(&b, &k_features[f], v);
}

static void write_custom(void) {
    Cvar *c = cvar_find("r_fidelity_custom");
    if (!c) return;
    char buf[CVAR_STR_MAX];
    fid_custom_string(buf, sizeof(buf));
    if (strcmp(buf, c->str)) {
        bool in = S.internal;
        S.internal = true;
        cvar_set_cv(c, buf);
        S.internal = in;
    }
}

static void bound_hook(Cvar *cv) {
    if (!fid_loaded()) return;
    int f = -1;
    for (int i = 0; i < FF_COUNT; i++) if (S.cv[i] == cv) { f = i; break; }
    if (f < 0 || fid_feature_locked(f)) return;
    const FidFeatureDef *d = &k_features[f];
    float v = d->kind == FID_FLOAT ? cv->f : (float)cv->i;
    FidBounds b = fid_bounds(S.level, fid_era(), f);
    float c = fid_clamp(&b, d, v);
    if (fabsf(c - v) > 1e-5f) {
        char buf[32];
        fmt_value(d, c, buf, sizeof(buf));
        cvar_store(cv, buf);
    }
    // A user change (not fid_apply / enforce, not the era preset's own features).
    if (S.tracking && !S.internal && !d->era_owned && b.avail != FID_UNAVAILABLE) {
        bool was = S.user_set[f];
        float old = S.user_val[f];
        S.user_set[f] = fabsf(c - b.def) > 1e-4f;
        S.user_val[f] = c;
        if (was != S.user_set[f] || (S.user_set[f] && fabsf(old - c) > 1e-5f)) write_custom();
    }
}

void fid_set_tracking(bool on) { S.tracking = on; }

void fid_custom_string(char *out, int size) {
    int n = 0;
    out[0] = 0;
    for (int f = 0; f < FF_COUNT && n < size; f++) {
        if (!S.user_set[f]) continue;
        char v[32];
        fmt_value(&k_features[f], S.user_val[f], v, sizeof(v));
        int w = snprintf(out + n, (size_t)(size - n), "%s%s=%s", n ? ";" : "", k_features[f].id, v);
        if (w < 0 || n + w >= size) { out[n] = 0; break; }
        n += w;
    }
}

void fid_custom_load(const char *s) {
    memset(S.user_set, 0, sizeof(S.user_set));
    while (s && *s) {
        char id[48];
        int k = 0;
        while (*s && *s != '=' && *s != ';' && k < 47) id[k++] = *s++;
        id[k] = 0;
        float v = 0;
        bool have = false;
        if (*s == '=') { v = (float)atof(++s); have = true; }
        while (*s && *s != ';') s++;
        if (*s == ';') s++;
        int f = fid_feature_find(id);
        if (f >= 0 && have && !k_features[f].era_owned && !fid_feature_locked(f)) { S.user_set[f] = true; S.user_val[f] = v; }
    }
}

bool fid_user_value(int f, float *v) {
    if (f < 0 || f >= FF_COUNT || !S.user_set[f]) return false;
    if (v) *v = S.user_val[f];
    return true;
}

void fid_attach_cvars(void) {
    S.r_era = cvar_find("r_era");
    for (int f = 0; f < FF_COUNT; f++) {
        S.cv[f] = cvar_find(k_features[f].cvar);
        if (S.cv[f] && !fid_feature_locked(f)) S.cv[f]->bound = bound_hook;
    }
    S.attached = true;
}

void fid_detach_cvars(void) {
    for (int f = 0; f < FF_COUNT; f++) {
        if (S.cv[f] && S.cv[f]->bound == bound_hook) S.cv[f]->bound = NULL;
        S.cv[f] = NULL;
    }
    S.r_era = NULL;
    S.attached = false;
}

static void set_value(int f, float v) {
    if (!S.cv[f] || fid_feature_locked(f)) return;
    char buf[32];
    fmt_value(&k_features[f], v, buf, sizeof(buf));
    bool in = S.internal;
    S.internal = true;
    CvarSource src = cvar_source_push(CVAR_SRC_FIDELITY);   // tier cvars (docs/SETTINGS_AUDIT.md)
    cvar_set_cv(S.cv[f], buf);
    cvar_source_pop(src);
    S.internal = in;
}

void fid_set_level(int level) {
    S.level = DG_CLAMP(level, 0, FID_LEVELS - 1);
    fid_enforce_all();
}

void fid_enforce_all(void) {
    if (!fid_loaded()) return;
    for (int f = 0; f < FF_COUNT; f++) {
        float v = fid_cvar_value(f);
        if (isnan(v) || fid_feature_locked(f)) continue;
        float c = fid_clamp_current(f, v);
        if (fabsf(c - v) > 1e-5f) set_value(f, c);
    }
}

void fid_apply(int level) {
    level = DG_CLAMP(level, 0, FID_LEVELS - 1);
    if (!fid_loaded()) { S.level = level; return; }
    int old = S.level, era = fid_era();
    // The era stays fixed while the values change (the era preset's own detection may flip r_era
    // to custom for a moment).
    S.era_locked = true;
    S.era_lock = era;
    S.level = level;
    for (int f = 0; f < FF_COUNT; f++) {
        const FidFeatureDef *d = &k_features[f];
        float v = fid_cvar_value(f);
        if (isnan(v) || fid_feature_locked(f)) continue;
        FidBounds nb = fid_bounds(level, era, f), ob = fid_bounds(old, era, f);
        (void)ob;
        float target;
        if (nb.avail == FID_UNAVAILABLE) {
            target = d->has_off ? d->off : fid_clamp(&nb, d, v);   // no off state: locked as it is
        } else {
            float u = S.user_val[f];
            bool keep = S.user_set[f] && (unordered(f) ? fid_allowed(&nb, d, u) : fid_cost_cmp(d, u, nb.def) < 0);
            target = keep ? fid_clamp(&nb, d, u) : nb.def;
        }
        if (fabsf(target - v) > 1e-5f) set_value(f, target);
    }
    S.era_locked = false;
    fid_enforce_all();
}

int fid_check_table(int level, char *report, int size) {
    int bad = 0, era = fid_era();
    if (report && size > 0) report[0] = 0;
    for (int f = 0; f < FF_COUNT; f++) {
        float v = fid_cvar_value(f);
        if (isnan(v) || fid_feature_locked(f)) continue;
        FidBounds b = fid_bounds(level, era, f);
        if (fabsf(v - b.def) < 1e-3f) continue;
        if (b.avail == FID_UNAVAILABLE && !k_features[f].has_off) continue;   // locked, value irrelevant
        bad++;
        if (report && size > 0) {
            size_t len = strlen(report);
            if (len + 64 < (size_t)size) snprintf(report + len, (size_t)size - len, "%s%s=%g (table %g)", len ? ", " : "", k_features[f].cvar, v, b.def);
        }
    }
    return bad;
}

int fid_custom_count(void) { return fid_check_table(S.level, NULL, 0); }

// ------------------------------------------------------------------ quality groups

// Anti-aliasing, render scale, sharpening and motion blur have their own Normal rows; the lens
// finish (flare, dirt, chromatic aberration, vignette, grain), exposure, the tonemapper, the
// renderer switch, character / scope detail stay with the overall level (and the Debug tab).
static const int k_g_post[] = { FF_AO, FF_CONTACT_SHADOWS, FF_SSR, FF_SSGI, FF_PUDDLE_REFLECTIONS, FF_VOLUMETRICS, FF_VOL_LOCAL_LIGHTS,
                                FF_DOF, FF_DOF_QUALITY, FF_ADS_DOF, FF_BLOOM, FF_BLOOM_QUALITY };
static const int k_g_shadows[] = { FF_SHADOWS, FF_SHADOW_RESOLUTION, FF_SHADOW_CASCADES, FF_SHADOW_DISTANCE, FF_SOFT_SHADOW_QUALITY,
                                   FF_SHADOW_MIN_RADIUS, FF_LIGHT_SHADOWS, FF_LIGHT_SHADOW_QUALITY, FF_CLOUD_SHADOWS, FF_MOON_SHADOWS };
static const int k_g_textures[] = { FF_TEXTURE_QUALITY, FF_ANISO, FF_TERRAIN_QUALITY };
static const int k_g_effects[] = { FF_VFX_QUALITY, FF_VFX_LIGHTING, FF_DECAL_MAX, FF_PRECIP_QUALITY, FF_WATER, FF_LENS_DROPS };
static const int k_g_foliage[] = { FF_FOLIAGE_DENSITY, FF_FOLIAGE_WIND };
static const int k_g_view[] = { FF_LOD_BIAS, FF_IMPOSTOR_PX, FF_VGEO_ERROR_PX, FF_VGEO_SHADOW_ERROR_PX, FF_VGEO_LOD_PX };
static const struct { const char *id; const int *f; int n; } k_groups[FG_COUNT] = {
    [FG_POST] = { "post", k_g_post, ARRAY_COUNT(k_g_post) },
    [FG_SHADOWS] = { "shadows", k_g_shadows, ARRAY_COUNT(k_g_shadows) },
    [FG_TEXTURES] = { "textures", k_g_textures, ARRAY_COUNT(k_g_textures) },
    [FG_EFFECTS] = { "effects", k_g_effects, ARRAY_COUNT(k_g_effects) },
    [FG_FOLIAGE] = { "foliage", k_g_foliage, ARRAY_COUNT(k_g_foliage) },
    [FG_VIEW_DISTANCE] = { "view_distance", k_g_view, ARRAY_COUNT(k_g_view) },
};

int fid_group_features(int g, const int **f) {
    if (g < 0 || g >= FG_COUNT) { if (f) *f = NULL; return 0; }
    if (f) *f = k_groups[g].f;
    return k_groups[g].n;
}
const char *fid_group_id(int g) { return g >= 0 && g < FG_COUNT ? k_groups[g].id : "?"; }

// A feature the current level locks without an off state (render scale style) takes no part.
static bool group_locked(int f) {
    if (fid_feature_locked(f)) return true;
    FidBounds b = fid_bounds(S.level, fid_era(), f);
    return b.avail == FID_UNAVAILABLE && !k_features[f].has_off;
}

bool fid_group_tier_allowed(int g, int tier, int *req) {
    if (req) *req = -1;
    if (g < 0 || g >= FG_COUNT || tier < 0 || tier >= FID_LEVELS) return false;
    int era = fid_era(), need = 0;
    for (int i = 0; i < k_groups[g].n; i++) {
        int f = k_groups[g].f[i];
        if (group_locked(f)) continue;
        int l = fid_min_level_for(era, f, fid_bounds(tier, era, f).def);
        if (l < 0) return false;
        need = DG_MAX(need, l);
    }
    if (req) *req = need;
    return need <= S.level;
}

// A user change (tracked as the user's choice, clamped by the bound hook like a menu set).
void fid_group_apply(int g, int tier) {
    if (!fid_loaded() || g < 0 || g >= FG_COUNT || tier < 0 || tier >= FID_LEVELS) return;
    int era = fid_era();
    for (int i = 0; i < k_groups[g].n; i++) {
        int f = k_groups[g].f[i];
        if (!S.cv[f] || group_locked(f)) continue;
        float v = fid_bounds(tier, era, f).def;
        if (fabsf(v - fid_cvar_value(f)) < 1e-5f) continue;
        char buf[32];
        fmt_value(&k_features[f], v, buf, sizeof(buf));
        CvarSource src = cvar_source_push(CVAR_SRC_FIDELITY);
        cvar_set_cv(S.cv[f], buf);
        cvar_source_pop(src);
    }
}

static bool group_matches(int g, int tier) {
    int era = fid_era();
    for (int i = 0; i < k_groups[g].n; i++) {
        int f = k_groups[g].f[i];
        float v = fid_cvar_value(f);
        if (isnan(v) || group_locked(f)) continue;
        if (fabsf(v - fid_clamp_current(f, fid_bounds(tier, era, f).def)) > 1e-3f) return false;
    }
    return true;
}

int fid_group_detect(int g) {
    if (!fid_loaded() || g < 0 || g >= FG_COUNT) return -1;
    if (group_matches(g, S.level)) return S.level;
    for (int t = FID_LEVELS - 1; t >= 0; t--)
        if (fid_group_tier_allowed(g, t, NULL) && group_matches(g, t)) return t;
    return -1;
}

// ------------------------------------------------------------------ passes

bool fid_pass_is_post(const char *n) {
    static const char *exact[] = { "hiz", "contact", "color_pyramid", "depth_history", "taa", "taau", "fxaa", "fsr_easu", "post_buffers_init",
                                   "exposure", "bloom", "flare", "tonemap", "debug_view" };
    static const char *prefix[] = { "gtao", "ssgi", "ssr", "vol_", "smaa_", "mb_", "dof_", "lens_" };
    for (int i = 0; i < ARRAY_COUNT(exact); i++) if (!strcmp(n, exact[i])) return true;
    for (int i = 0; i < ARRAY_COUNT(prefix); i++) if (!strncmp(n, prefix[i], strlen(prefix[i]))) return true;
    return false;
}
