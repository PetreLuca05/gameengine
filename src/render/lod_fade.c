// LOD ranges and cross-fades (docs/LOD.md): see lod_fade.h.
#include "lod_fade.h"
#include "render/fidelity/fidelity.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_cat_id[LODC_COUNT] = { "characters", "static", "impostor", "foliage", "terrain", "shadow", "vfx" };
static const char *k_era_id[4] = { "modern", "2020", "2015", "2010" };
static const char *k_tier_id[5] = { "off", "low", "medium", "high", "modern" };

void lod_table_default(LodTable *t) {
    memset(t, 0, sizeof(*t));
    t->fade_time = 0.35f;
    t->band = 0.25f;
    const float cat[LODC_COUNT] = { 1.0f, 2.0f, 2.0f, 1.5f, 1.5f, 1.5f, 1.5f };
    memcpy(t->cat, cat, sizeof(cat));
    const float tier[5] = { 0.3f, 0.4f, 0.6f, 0.85f, 1.0f };
    memcpy(t->tier, tier, sizeof(tier));
    for (int e = 0; e < 4; e++)
        for (int c = 0; c < LODC_COUNT; c++) t->era[e][c] = e == 2 ? 0.85f : (e == 3 ? 0.7f : 1.0f);
    t->char_px[0] = 140.0f;
    t->char_px[1] = 60.0f;
    t->char_px[2] = 25.0f;
    t->char_hysteresis = 0.15f;
    t->char_cull_px = 3.0f;
    t->char_shadow_px = 20.0f;
    const int up[LOD_CHAR_COUNT] = { 1, 2, 4, 8 };
    memcpy(t->char_update, up, sizeof(up));
    t->char_ramp_s = 1.5f;
    t->char_ik_band = 0.3f;
    t->imp_band = 0.35f;
    t->imp_gain = 1.2f;
    t->fol_band = 1.5f;
    t->fol_instance = 0.3f;
    t->fol_shrink = 0.35f;
    t->ter_detail = 1.5f;
    t->ter_morph = 0.5f;
    t->shadow_band = 0.15f;
    t->prefetch = 0.5f;
}

static float jnum(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static bool jarr(const cJSON *o, const char *k, float *out, int n) {
    const cJSON *a = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (!cJSON_IsArray(a)) return true;
    if (cJSON_GetArraySize(a) != n) return false;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v)) return false;
        out[i] = (float)v->valuedouble;
    }
    return true;
}

bool lod_table_parse(LodTable *t, const char *json, char *err, int n) {
    lod_table_default(t);
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { snprintf(err, (size_t)n, "lod.json: parse error"); return false; }
    bool ok = true;
    const cJSON *fade = cJSON_GetObjectItemCaseSensitive(root, "fade");
    t->fade_time = jnum(fade, "time", t->fade_time);
    t->band = jnum(fade, "band", t->band);
    const cJSON *dist = cJSON_GetObjectItemCaseSensitive(root, "distance");
    for (int c = 0; c < LODC_COUNT; c++) t->cat[c] = jnum(dist, k_cat_id[c], t->cat[c]);
    const cJSON *tiers = cJSON_GetObjectItemCaseSensitive(root, "tiers");
    for (int l = 0; l < 5; l++) t->tier[l] = jnum(tiers, k_tier_id[l], t->tier[l]);
    const cJSON *eras = cJSON_GetObjectItemCaseSensitive(root, "eras");
    for (int e = 0; e < 4; e++) {
        const cJSON *eo = eras ? cJSON_GetObjectItemCaseSensitive(eras, k_era_id[e]) : NULL;
        for (int c = 0; c < LODC_COUNT; c++) t->era[e][c] = jnum(eo, k_cat_id[c], jnum(eo, "all", t->era[e][c]));
    }
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "characters");
    ok &= jarr(ch, "px", t->char_px, LOD_CHAR_COUNT - 1);
    float up[LOD_CHAR_COUNT];
    for (int i = 0; i < LOD_CHAR_COUNT; i++) up[i] = (float)t->char_update[i];
    ok &= jarr(ch, "update_every", up, LOD_CHAR_COUNT);
    for (int i = 0; i < LOD_CHAR_COUNT; i++) t->char_update[i] = DG_MAX(1, (int)(up[i] + 0.5f));
    t->char_hysteresis = jnum(ch, "hysteresis", t->char_hysteresis);
    t->char_cull_px = jnum(ch, "cull_px", t->char_cull_px);
    t->char_shadow_px = jnum(ch, "shadow_px", t->char_shadow_px);
    t->char_ramp_s = jnum(ch, "update_ramp_s", t->char_ramp_s);
    t->char_ik_band = jnum(ch, "ik_band", t->char_ik_band);
    const cJSON *im = cJSON_GetObjectItemCaseSensitive(root, "impostor");
    t->imp_band = jnum(im, "band", t->imp_band);
    t->imp_gain = jnum(im, "gain", t->imp_gain);
    const cJSON *fo = cJSON_GetObjectItemCaseSensitive(root, "foliage");
    t->fol_band = jnum(fo, "band", t->fol_band);
    t->fol_instance = jnum(fo, "instance_fade", t->fol_instance);
    t->fol_shrink = jnum(fo, "shrink", t->fol_shrink);
    const cJSON *te = cJSON_GetObjectItemCaseSensitive(root, "terrain");
    t->ter_detail = jnum(te, "detail_range", t->ter_detail);
    t->ter_morph = jnum(te, "morph", t->ter_morph);
    const cJSON *sh = cJSON_GetObjectItemCaseSensitive(root, "shadow");
    t->shadow_band = jnum(sh, "band", t->shadow_band);
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "streaming");
    t->prefetch = jnum(st, "prefetch", t->prefetch);
    cJSON_Delete(root);
    if (!ok) { snprintf(err, (size_t)n, "lod.json: an array has the wrong length or type"); return false; }
    // sanity: thresholds strictly decreasing, positive multipliers
    for (int i = 0; i + 1 < LOD_CHAR_COUNT - 1; i++)
        if (!(t->char_px[i] > t->char_px[i + 1]) || t->char_px[i + 1] <= 0) { snprintf(err, (size_t)n, "lod.json: characters.px must decrease"); return false; }
    for (int c = 0; c < LODC_COUNT; c++)
        if (!(t->cat[c] > 0)) { snprintf(err, (size_t)n, "lod.json: distance.%s must be > 0", k_cat_id[c]); return false; }
    for (int l = 0; l < 5; l++)
        if (!(t->tier[l] > 0) || (l > 0 && t->tier[l] < t->tier[l - 1])) { snprintf(err, (size_t)n, "lod.json: tiers must be > 0 and not decrease (%s)", k_tier_id[l]); return false; }
    if (t->fade_time < 0 || t->band < 0 || t->fol_shrink < 0 || t->fol_shrink > 1) { snprintf(err, (size_t)n, "lod.json: fade values out of range"); return false; }
    return true;
}

float lod_scale_of(const LodTable *t, LodCat c, int level, float user_bias, int era) {
    era = DG_CLAMP(era, 0, 3);
    level = DG_CLAMP(level, 0, 4);
    return t->tier[level] * (user_bias > 0 ? user_bias : 1.0f) * t->cat[c] * t->era[era][c];
}

int lod_char_select(const LodTable *t, float scale, int current, float pixels) {
    float p = pixels * (scale > 0 ? scale : 1.0f);
    int target = LOD_CHAR_COUNT - 1;
    for (int i = 0; i < LOD_CHAR_COUNT - 1; i++)
        if (p >= t->char_px[i]) { target = i; break; }
    if (current < 0 || current >= LOD_CHAR_COUNT) return target;
    float h = t->char_hysteresis;
    while (current < target && p < t->char_px[current] * (1.0f - h)) current++;
    while (current > target && p > t->char_px[current - 1] * (1.0f + h)) current--;
    return current;
}

// ---------------------------------------------------------------- time-based

void lod_fade_reset(LodFade *f, int lod) {
    f->cur = f->from = (i8)lod;
    f->t = 1.0f;
}

bool lod_fade_active(const LodFade *f) { return f->from != f->cur && f->t < 1.0f; }

void lod_fade_step(LodFade *f, int want, float dt, float fade_time) {
    if (f->cur < 0) { lod_fade_reset(f, want); return; }
    if (lod_fade_active(f)) {   // finish the running fade, whatever the selector says now
        f->t = fade_time > 0 ? f->t + dt / fade_time : 1.0f;
        if (f->t >= 1.0f) { f->t = 1.0f; f->from = f->cur; }
        return;
    }
    if (want == f->cur) return;
    f->from = f->cur;
    f->cur = (i8)want;
    f->t = fade_time > 0 ? fminf(dt / fade_time, 1.0f) : 1.0f;
    if (f->t >= 1.0f) f->from = f->cur;
}

float lod_fade_in(float t) { return DG_CLAMP(t, 1.0f / 512.0f, 1.0f - 1.0f / 512.0f); }
float lod_fade_out(float t) { return -(1.0f - lod_fade_in(t)); }

int lod_fade_draws(const LodFade *f, int lods[2], float fades[2]) {
    if (!lod_fade_active(f)) {
        lods[0] = f->cur;
        fades[0] = 0.0f;
        return 1;
    }
    lods[0] = f->from;
    fades[0] = lod_fade_out(f->t);
    lods[1] = f->cur;
    fades[1] = lod_fade_in(f->t);
    return 2;
}

float lod_coverage(float f) { return f == 0.0f ? 1.0f : (f > 0.0f ? f : -f); }

bool lod_kept(float f, float n) { return f == 0.0f || (f > 0.0f ? n < f : n >= 1.0f + f); }

float lod_iv_pack(float lo, float hi) {
    int a = DG_CLAMP((int)floorf(lo * 256.0f + 0.5f), 0, 256), b = DG_CLAMP((int)floorf(hi * 256.0f + 0.5f), 0, 256);
    return (float)a + (float)b * 512.0f;
}

bool lod_iv_kept(float packed, float n) {
    if (packed == 0.0f) return true;   // 0: no interval (whole coverage)
    int p = (int)packed, a = p % 512, b = p / 512;
    float lo = (float)a / 256.0f, hi = (float)b / 256.0f;
    return n >= lo && n < hi;
}

int lod_resident_pick(int want, u32 ready, int count) {
    if (want < 0 || want >= count) return -1;
    if (ready & (1u << want)) return want;
    for (int l = want + 1; l < count; l++)
        if (ready & (1u << l)) return l;
    for (int l = want - 1; l >= 0; l--)
        if (ready & (1u << l)) return l;
    return -1;
}

// ---------------------------------------------------------------- terrain

int lod_terrain_level(float near_dist, float lod_d) {
    if (near_dist < lod_d || lod_d <= 0) return 0;
    int l = (int)floorf(log2f(near_dist / lod_d));
    return DG_CLAMP(l, 0, 3);
}

float lod_terrain_morph(float dist, int level, float lod_d, float band, float diag) {
    if (level >= 3 || lod_d <= 0) return 0.0f;
    float L = log2f(fmaxf(dist, 1e-3f) / lod_d) - (float)level;   // 0..1 across this level's octave (and beyond)
    float a = 1.0f - DG_CLAMP(band, 0.02f, 1.0f);
    // a chunk switching to this level from level - 1 has vertices up to diag metres further than its nearest
    // point: they must not be morphing yet (no pop at the switch)
    if (level > 0) a = fmaxf(a, log2f(1.0f + diag / (lod_d * (float)(1 << level))));
    if (a >= 1.0f) return L >= 1.0f ? 1.0f : 0.0f;
    float m = (L - a) / (1.0f - a);
    return m <= 0 ? 0.0f : (m >= 1 ? 1.0f : m);
}

// ---------------------------------------------------------------- distance band

float lod_band_weight(float y, float band) {
    if (band <= 0) return y >= 0 ? 1.0f : 0.0f;
    float w = y / band;
    return w <= 0 ? 0.0f : (w >= 1 ? 1.0f : w);
}

int lod_pick_band(const float *err, int count, int first, float k, float px, float band, float *coarse_w) {
    *coarse_w = 0;
    int lod = DG_CLAMP(first, 0, count - 1);
    while (lod + 1 < count && err[lod + 1] * k <= px) lod++;
    if (lod > first && band > 0) {
        // octaves the switch to `lod` is past its threshold: within the band, the finer LOD fades out
        float e = err[lod] * k;
        float y = e > 1e-12f ? log2f(px / e) : band;
        if (y < band) {
            *coarse_w = lod_band_weight(y, band);
            return lod - 1;
        }
    }
    return lod;
}

// ---------------------------------------------------------------- runtime

static struct {
    bool init;
    LodTable t;
    Cvar *fade_time, *band, *fade_debug, *legacy, *bias, *era, *debug, *scope;
} L;

const LodTable *lod_table(void) {
    if (!L.init) lod_table_default(&L.t);
    return &L.t;
}

bool lod_load(const char *path) {
    char *text = platform_read_file(path, NULL);
    if (!text) { log_warn("lod: %s missing, built-in ranges", path); lod_table_default(&L.t); return false; }
    char err[160];
    LodTable t;
    bool ok = lod_table_parse(&t, text, err, sizeof(err));
    free(text);
    if (!ok) { log_error("%s (built-in ranges kept)", err); return false; }
    L.t = t;
    log_info("lod: %s (fade %.2f s, band %.2f oct; distance x characters %.2f static %.2f impostor %.2f foliage %.2f terrain %.2f shadow %.2f)", path, (double)t.fade_time,
             (double)t.band, (double)t.cat[LODC_CHARACTER], (double)t.cat[LODC_STATIC], (double)t.cat[LODC_IMPOSTOR], (double)t.cat[LODC_FOLIAGE], (double)t.cat[LODC_TERRAIN],
             (double)t.cat[LODC_SHADOW]);
    return true;
}

static void load_shipped(void) {
    char path[512];
    platform_data_path("assets/render/lod.json", path, sizeof(path));
    lod_load(path);
}

static void cmd_lod_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load_shipped();
}

void lod_init(void) {
    if (L.init) return;
    L.init = true;
    lod_table_default(&L.t);
    load_shipped();
    L.fade_time = cvar_reg_float("r_lod_fade_time", L.t.fade_time, 0.0f, 3.0f, CVAR_ARCHIVE, "LOD cross-fade time of characters and their shadows (s, 0 = cut; docs/LOD.md)");
    L.band = cvar_reg_float("r_lod_fade_band", L.t.band, 0.0f, 2.0f, CVAR_ARCHIVE,
                            "distance band of the stateless LOD / impostor cross-fades (octaves of projected size, 0 = cut; docs/LOD.md)");
    L.fade_debug = cvar_reg_int("r_lod_fade_debug", 0, 0, 1, 0, "tint fading LODs (green: fading in, red: fading out)");
    L.legacy = cvar_reg_bool("r_lod_legacy", false, 0, "the LOD ranges and transitions from before docs/LOD.md (comparisons)");
    L.scope = cvar_reg_bool("r_lod_scope", true, 0, "scope picture-in-picture LOD (docs/LOD.md): its own classic culling / LOD pick, characters at their magnified size (0: before)");
    cmd_register("r_lod_reload", cmd_lod_reload, "reload assets/render/lod.json (LOD ranges, docs/LOD.md)");
}

bool lod_active(void) {
    if (!L.init) return false;
    return !(L.legacy && L.legacy->i);
}

static int cur_era(void) {
    if (!L.era) L.era = cvar_find("r_era");
    int era = L.era ? L.era->i : 0;
    return era > 3 ? 0 : era;   // custom: the modern table
}

static int cur_level(void) { return fid_loaded() ? fid_level() : FID_MODERN; }

float lod_user_bias(void) {
    if (!L.bias) L.bias = cvar_find("r_lod_bias");
    if (!L.bias) return 1.0f;
    float def = fid_loaded() ? fid_bounds(cur_level(), fid_era_from_cvar(cur_era()), FF_LOD_BIAS).def : 1.0f;
    return def > 0 ? L.bias->f / def : L.bias->f;
}

float lod_scale(LodCat c) {
    if (!lod_active()) return 1.0f;
    return lod_scale_of(&L.t, c, cur_level(), lod_user_bias(), cur_era());
}

float lod_fade_time(void) { return L.fade_time ? L.fade_time->f : lod_table()->fade_time; }
float lod_band(void) { return L.band ? L.band->f : lod_table()->band; }
int lod_fade_debug(void) { return L.fade_debug ? L.fade_debug->i : 0; }
bool lod_scope(void) { return !L.scope || L.scope->i != 0; }
int lod_debug(void) {
    if (!L.debug) L.debug = cvar_find("r_lod_debug");
    return L.debug ? L.debug->i : 0;
}
