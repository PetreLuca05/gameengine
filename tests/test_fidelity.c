// Engine Fidelity unit tests (src/render/fidelity, docs/ENGINE_FIDELITY.md), on the real tables
// (assets/render/fidelity) and the real cvar system:
//   * the tables load, are complete, defaults sit inside their bounds, higher levels never take a
//     feature away or lower a ceiling (every era);
//   * applying a level, then reading every cvar, matches the table (every level x era, any order);
//   * every control's options are allowed / greyed exactly per level, the "requires" level is the
//     lowest one allowing the option, and a set outside the bounds is clamped (console / config /
//     presets all go through cvar.bound);
//   * lowering fidelity clamps higher settings, raising restores the level defaults, values the user
//     set below a default are kept;
//   * era matrix: what an era lacks is unavailable, era-owned features scale the era's own values;
//   * locked features (docs/SETTINGS_AUDIT.md): one value (assets/render/consistency.json) at every
//     level and era, never written by fid_apply; visibility floors (Hard shadows, water refraction,
//     rain / snow) at every level;
//   * every level runs post-processing (Off: the minimal pass); frame graph pass classification;
//   * frame graph validation (fg_check): dead passes, unused resources, undefined reads, history
//     ping-pong;
//   * malformed tables are rejected with a message.
#include "render/fidelity/fidelity.h"
#include "render/graph/fg_check.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/consistency.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Engine g_engine;   // the core library logs through it
static int g_fail, g_checks;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        g_checks++;                                             \
        if (!(cond)) {                                          \
            g_fail++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

// Era preset values (vk_vgeo.c k_eras): vgeo, shadow error px (the LOD thresholds and the foliage
// density are locked, docs/SETTINGS_AUDIT.md).
#define ERA_OWNED 2
static const float k_era_vals[FID_ERAS][ERA_OWNED] = { { 1, 2 }, { 1, 4 }, { 0, 2 }, { 0, 2 } };
static const int k_owned[ERA_OWNED] = { FF_VGEO, FF_VGEO_SHADOW_ERROR_PX };
static Cvar *g_era;

static void register_cvars(void) {
    for (int e = 0; e < FID_ERAS; e++)
        for (int k = 0; k < ERA_OWNED; k++) fid_set_era_value(e, k_owned[k], k_era_vals[e][k]);
    // Every feature cvar with its engine range and the Modern default (= the engine default); a
    // locked one with the value of the consistency set.
    for (int f = 0; f < fid_feature_count(); f++) {
        const FidFeatureDef *d = fid_feature(f);
        FidBounds b = fid_bounds(FID_MODERN, FID_ERA_MODERN, f);
        u32 flags = CVAR_ARCHIVE | CVAR_TIER;
        if (fid_feature_locked(f)) {
            const char *v = consistency_find(d->cvar);
            CHECK(v != NULL, "locked feature %s (%s) is in assets/render/consistency.json", d->id, d->cvar);
            b.def = v ? (float)atof(v) : 0.0f;
            flags = CVAR_LOCKED;
        }
        if (d->kind == FID_BOOL) cvar_reg_bool(d->cvar, b.def != 0, flags, d->label);
        else if (d->kind == FID_INT) cvar_reg_int(d->cvar, (int)b.def, (int)d->lo, (int)d->hi, flags, d->label);
        else cvar_reg_float(d->cvar, b.def, d->lo, d->hi, flags, d->label);
    }
    g_era = cvar_reg_int("r_era", 0, 0, 4, CVAR_ARCHIVE, "era");
    cvar_reg_string("r_fidelity_custom", "", CVAR_ARCHIVE, "user choices");
}

static Cvar *cv(int f) { return cvar_find(fid_feature(f)->cvar); }
static float val(int f) { return fid_cvar_value(f); }
static void set(int f, float v) {
    char b[32];
    snprintf(b, sizeof(b), "%g", v);
    cvar_set_cv(cv(f), b);
}
static void set_era(int e) {
    cvar_set_int(g_era, e);
    // The era preset sets its own values (vk_vgeo.c era_apply), then the bounds clamp everything.
    for (int k = 0; k < ERA_OWNED; k++) set(k_owned[k], k_era_vals[e][k]);
    fid_enforce_all();
}

static void test_tables(void) {
    char err[256] = "";
    bool ok = fid_load_dir("assets/render/fidelity", err, sizeof(err));
    CHECK(ok, "tables load: %s", err);
    if (!ok) { printf("cannot continue without the tables\n"); exit(1); }
    CHECK(fid_tables_complete(err, sizeof(err)), "complete: %s", err);
    for (int l = 0; l < FID_LEVELS; l++) {
        CHECK(fid_level_desc(l)[0] != 0, "%s has a description", fid_level_id(l));
        CHECK(fid_level_budget_ms(l) > 0, "%s has a budget", fid_level_id(l));
        CHECK(fid_level_parse(fid_level_id(l)) == l && fid_level_parse(fid_level_name(l)) == l, "parse %s", fid_level_id(l));
    }
    const float budgets[FID_LEVELS] = { 3, 5, 8, 12, 16 };
    for (int l = 0; l < FID_LEVELS; l++) CHECK(fabsf(fid_level_budget_ms(l) - budgets[l]) < 1e-4f, "%s budget %.1f", fid_level_id(l), fid_level_budget_ms(l));
    // Every level runs post-processing: Off's minimal pass keeps auto exposure, fog and the lens
    // droplets (the same visibility as every level, docs/SETTINGS_AUDIT.md V10 / V11).
    for (int l = 0; l < FID_LEVELS; l++) CHECK(fid_level_post(l), "%s runs the post pass", fid_level_id(l));
}

static void test_apply_matches_table(void) {
    for (int e = 0; e < FID_ERAS; e++) {
        set_era(e);
        // Every level from every other level (and twice in a row).
        for (int from = 0; from < FID_LEVELS; from++)
            for (int to = 0; to < FID_LEVELS; to++) {
                fid_apply(FID_MODERN);
                fid_apply(from);
                char rep[1024];
                CHECK(fid_check_table(from, rep, sizeof(rep)) == 0, "era %s: %s -> table: %s", fid_era_id(e), fid_level_id(from), rep);
                fid_apply(to);
                CHECK(fid_check_table(to, rep, sizeof(rep)) == 0, "era %s: %s -> %s: %s", fid_era_id(e), fid_level_id(from), fid_level_id(to), rep);
                CHECK(g_era->i == e, "the era survives the level change (%d)", g_era->i);
            }
    }
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
}

// Every option of every control, per level: allowed exactly when the bounds say so; a set outside
// them stores an allowed value no costlier than asked; the "requires" level is the lowest allowing it.
static void test_options(void) {
    for (int e = 0; e < FID_ERAS; e++) {
        set_era(e);
        for (int l = 0; l < FID_LEVELS; l++) {
            fid_apply(l);
            for (int f = 0; f < fid_feature_count(); f++) {
                const FidFeatureDef *d = fid_feature(f);
                if (fid_feature_locked(f)) continue;   // test_locked
                FidBounds b = fid_bounds(l, e, f);
                int steps = d->kind == FID_FLOAT ? 20 : (int)(d->hi - d->lo);
                if (f == FF_DECAL_MAX) steps = 64;
                for (int s = 0; s <= steps; s++) {
                    float v = d->lo + (d->hi - d->lo) * (float)s / (float)DG_MAX(steps, 1);
                    if (d->kind != FID_FLOAT) v = roundf(v);
                    bool allowed = fid_value_allowed(l, e, f, v);
                    int req = fid_min_level_for(e, f, v);
                    if (allowed) CHECK(req >= 0 && req <= l, "%s=%g allowed at %s but requires %d", d->id, v, fid_level_id(l), req);
                    else CHECK(req < 0 || req > l, "%s=%g greyed at %s but requires %d", d->id, v, fid_level_id(l), req);
                    // Monotonic: allowed here -> allowed one level up (every option keeps working).
                    if (allowed && l + 1 < FID_LEVELS && f != FF_UPSCALER && f != FF_TONEMAPPER && f != FF_PRECIP_QUALITY && f != FF_LENS_DISTORTION)
                        CHECK(fid_value_allowed(l + 1, e, f, v), "%s=%g allowed at %s but not at %s (era %s)", d->id, v, fid_level_id(l), fid_level_id(l + 1), fid_era_id(e));
                    set(f, v);
                    float got = val(f);
                    if (allowed) CHECK(fabsf(got - v) < 1e-3f, "%s=%g allowed at %s but stored %g", d->id, v, fid_level_id(l), got);
                    else {
                        CHECK(fid_allowed(&b, d, got) || (b.avail == FID_UNAVAILABLE && !d->has_off), "%s=%g clamped to %g (not allowed) at %s", d->id, v, got, fid_level_id(l));
                        bool choice = f == FF_UPSCALER || f == FF_TONEMAPPER || f == FF_PRECIP_QUALITY;
                        if (!choice && b.avail != FID_UNAVAILABLE) CHECK(fid_cost_cmp(d, got, v) <= 0 || fid_cost_cmp(d, got, b.lo) == 0 || d->cheaper_high, "%s=%g clamped up to %g at %s", d->id, v, got, fid_level_id(l));
                    }
                }
                set(f, b.def);
            }
        }
    }
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
}

static void test_lower_raise(void) {
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
    // Push some settings above the defaults (allowed at Modern) ...
    set(FF_SSR, 2);
    set(FF_MOTION_BLUR, 2);
    set(FF_VOLUMETRICS, 3);
    set(FF_RENDER_SCALE, 1.75f);
    set(FF_SSGI, 1);
    CHECK(val(FF_VOLUMETRICS) == 3 && val(FF_SSGI) == 1 && fabsf(val(FF_RENDER_SCALE) - 1.75f) < 1e-4f, "Modern allows ultra volumetrics, SSGI, 1.75x");
    // ... lowering clamps them,
    fid_apply(FID_LOW);
    CHECK(val(FF_SSR) == 0 && val(FF_MOTION_BLUR) == 0 && val(FF_VOLUMETRICS) == 0 && val(FF_SSGI) == 0, "Low: screen-space / camera effects off");
    CHECK(val(FF_RENDER_SCALE) <= 1.0f + 1e-4f, "Low: render scale <= 1 (%g)", val(FF_RENDER_SCALE));
    CHECK(val(FF_AA) <= 1, "Low: FXAA at most");
    // the console / config can't push past the bounds,
    cvar_set("r_ssr", "2");
    cvar_set("r_aa", "3");
    cvar_set("r_render_scale", "2");
    CHECK(val(FF_SSR) == 0 && val(FF_AA) == 1 && val(FF_RENDER_SCALE) <= 1.0f, "Low: sets clamp (ssr %g aa %g scale %g)", val(FF_SSR), val(FF_AA), val(FF_RENDER_SCALE));
    // raising restores the level defaults.
    fid_apply(FID_MODERN);
    char rep[1024];
    CHECK(fid_check_table(FID_MODERN, rep, sizeof(rep)) == 0, "raising to Modern restores its defaults: %s", rep);
    // Explicitly lower values survive both ways.
    set(FF_MOTION_BLUR, 0);
    set(FF_FILM_GRAIN, 0);
    set(FF_SHADOW_RESOLUTION, 1);
    fid_apply(FID_HIGH);
    CHECK(val(FF_MOTION_BLUR) == 0 && val(FF_FILM_GRAIN) == 0 && val(FF_SHADOW_RESOLUTION) == 1, "High keeps the explicitly lower values");
    CHECK(val(FF_SSR) == 1, "High: SSR half res (default) %g", val(FF_SSR));
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_MOTION_BLUR) == 0 && val(FF_SHADOW_RESOLUTION) == 1, "Medium keeps them");
    fid_apply(FID_MODERN);
    CHECK(val(FF_MOTION_BLUR) == 0 && val(FF_FILM_GRAIN) == 0 && val(FF_SHADOW_RESOLUTION) == 1, "back to Modern: still lower");
    CHECK(fid_custom_count() == 3, "three custom values (%d)", fid_custom_count());
    // Higher than the default at a level is not "explicitly lower": the next level's default wins.
    set(FF_VOLUMETRICS, 3);
    fid_apply(FID_HIGH);
    CHECK(val(FF_VOLUMETRICS) == 2, "High: volumetrics back to its default (%g)", val(FF_VOLUMETRICS));
    // An explicit choice (unordered) is kept where allowed.
    fid_apply(FID_MODERN);
    set(FF_UPSCALER, 2);
    fid_apply(FID_LOW);
    CHECK(val(FF_UPSCALER) == 2, "Low keeps the chosen FSR 1 (allowed)");
    set(FF_UPSCALER, 1);
    CHECK(val(FF_UPSCALER) != 1, "Low: TAA-U greyed");
    set(FF_UPSCALER, 0);
    // The user's choices persist (r_fidelity_custom) and come back after a restart.
    fid_apply(FID_MODERN);
    set(FF_SSR, 1);
    Cvar *custom = cvar_find("r_fidelity_custom");
    CHECK(custom && strstr(custom->str, "ssr=1"), "persisted: %s", custom ? custom->str : "-");
    char saved[CVAR_STR_MAX];
    dg_strcpy(saved, sizeof(saved), custom->str);
    fid_custom_load("");
    fid_apply(FID_MEDIUM);
    fid_apply(FID_MODERN);
    CHECK(val(FF_SSR) == 2, "choices forgotten: Modern default SSR");
    fid_custom_load(saved);
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_SSR) == 0, "Medium: SSR unavailable");
    fid_apply(FID_MODERN);
    CHECK(val(FF_SSR) == 1 && val(FF_MOTION_BLUR) == 0, "reloaded choices: half-res SSR and no motion blur come back at Modern");
    // Restore: no user choices, Modern defaults.
    fid_custom_load("");
    fid_apply(FID_MODERN);
}

static void test_levels(void) {
    set_era(FID_ERA_MODERN);
    fid_apply(FID_OFF);
    const int post[] = { FF_AA, FF_UPSCALER, FF_SHARPEN, FF_AO, FF_CONTACT_SHADOWS, FF_SSR, FF_SSGI, FF_VOLUMETRICS, FF_MOTION_BLUR, FF_DOF, FF_ADS_DOF,
                         FF_BLOOM, FF_CHROMATIC, FF_VIGNETTE, FF_FILM_GRAIN, FF_HDR_OUTPUT };
    for (int i = 0; i < ARRAY_COUNT(post); i++) {
        CHECK(val(post[i]) == 0, "Off: %s off (%g)", fid_feature(post[i])->id, val(post[i]));
        CHECK(fid_bounds(FID_OFF, 0, post[i]).avail == FID_UNAVAILABLE, "Off: %s locked", fid_feature(post[i])->id);
    }
    CHECK(val(FF_SHADOWS) == 1 && val(FF_SHADOW_CASCADES) == 2 && val(FF_RENDER_SCALE) >= 0.5f && val(FF_RENDER_SCALE) <= 0.66f + 1e-4f, "Off: Hard shadows, two cascades over the locked distance, 0.5-0.66 scale");
    CHECK(val(FF_LENS_DROPS) == 1 && val(FF_AUTO_EXPOSURE) == 1 && val(FF_LIGHT_SHADOWS) == 1 && val(FF_CLOUD_SHADOWS) == 1, "Off: droplets, auto exposure, lamp and cloud shadows (locked on)");
    // Internal consistency: TAA (and TAA-U) only where the level has TAA; HDR output only High / Modern.
    for (int e = 0; e < FID_ERAS; e++)
        for (int l = 0; l < FID_LEVELS; l++) {
            bool taa = fid_value_allowed(l, e, FF_AA, 3);
            CHECK(!fid_value_allowed(l, e, FF_UPSCALER, 1) || taa, "%s/%s: TAA-U without TAA", fid_level_id(l), fid_era_id(e));
            CHECK(fid_value_allowed(l, e, FF_HDR_OUTPUT, 1) == ((l >= FID_HIGH) && e != FID_ERA_2010), "%s/%s: HDR output", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_PUDDLE_REFLECTIONS, 1) || fid_value_allowed(l, e, FF_SSR, 1), "%s/%s: puddles need SSR", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_ADS_DOF, 1) || fid_value_allowed(l, e, FF_DOF, 1), "%s/%s: ADS DoF needs DoF", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_LENS_DIRT, 1) || fid_value_allowed(l, e, FF_BLOOM, 1), "%s/%s: lens dirt needs bloom", fid_level_id(l), fid_era_id(e));
        }
    // The spec's per-level points.
    fid_apply(FID_LOW);
    CHECK(val(FF_AA) == 1 && val(FF_SHADOW_CASCADES) == 4 && val(FF_SHADOWS) == 1 && val(FF_SHADOW_RESOLUTION) == 0 && fabsf(val(FF_SHADOW_DISTANCE) - 150.0f) < 1e-3f && fabsf(val(FF_RENDER_SCALE) - 0.66f) < 1e-3f, "Low: FXAA, Hard Only, Low resolution, 4 cascades, 150 m (locked), 0.66");
    CHECK(fabsf(val(FF_FOLIAGE_DENSITY) - 1.0f) < 1e-4f && fabsf(val(FF_VGEO_ERROR_PX) - 1.0f) < 1e-4f && val(FF_TEXTURE_QUALITY) == 0, "Low: full foliage, vgeo 1 px (locked), mips -2");
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_SSR) == 0 && val(FF_MOTION_BLUR) == 1 && val(FF_DOF) == 1 && val(FF_UPSCALER) == 2 && fabsf(val(FF_UPSCALE_RATIO) - 0.75f) < 1e-4f, "Medium: no SSR, camera MB, ADS DoF, FSR 1 0.75");
    CHECK(val(FF_VOL_LOCAL_LIGHTS) == 0 && val(FF_VOLUMETRICS) == 1 && val(FF_VFX_LIGHTING) == 0 && val(FF_DECAL_MAX) == 256, "Medium: fog-only, unlit particles, the locked decal budget");
    CHECK(!fid_value_allowed(FID_MEDIUM, 0, FF_CHROMATIC, 0.5f), "Medium: CA greyed");
    fid_apply(FID_HIGH);
    CHECK(val(FF_SSR) == 1 && val(FF_SSGI) == 0 && !fid_value_allowed(FID_HIGH, 0, FF_SSGI, 1) && val(FF_BLOOM_QUALITY) == 1 && val(FF_DOF_QUALITY) == 1, "High: SSR half, SSGI greyed, fewer bloom mips, hybrid DoF");
    CHECK(fabsf(val(FF_SHADOW_DISTANCE) - 150.0f) < 1e-3f && val(FF_SHADOW_CASCADES) == 4 && val(FF_SOFT_SHADOW_QUALITY) == 1 && fabsf(val(FF_FOLIAGE_DENSITY) - 1.0f) < 1e-4f, "High: 150 m (locked), 4 cascades, soft Medium, full foliage");
    fid_apply(FID_MODERN);
    for (int f = 0; f < fid_feature_count(); f++)
        if (f != FF_HDR_OUTPUT) CHECK(fid_bounds(FID_MODERN, 0, f).avail != FID_UNAVAILABLE, "Modern: %s unlocked", fid_feature(f)->id);
}

static void test_eras(void) {
    set_era(FID_ERA_2010);
    fid_apply(FID_MODERN);
    CHECK(val(FF_SSR) == 0 && val(FF_SSGI) == 0 && val(FF_VOLUMETRICS) == 0 && val(FF_VGEO) == 0, "2010: no SSR / SSGI / froxels / vgeo");
    CHECK(fid_bounds(FID_MODERN, FID_ERA_2010, FF_SSR).era_lacks && fid_min_level_for(FID_ERA_2010, FF_SSR, 1) < 0, "2010 lacks SSR at every level");
    CHECK(val(FF_AA) == 2, "2010 Modern: SMAA (no TAA in the era): %g", val(FF_AA));
    cvar_set("r_aa", "3");
    CHECK(val(FF_AA) != 3, "2010: TAA greyed");
    fid_apply(FID_LOW);
    CHECK(val(FF_AA) == 2 && val(FF_SHADOWS) == 2, "2010-era Low differs from modern-era Low (SMAA, soft shadows)");
    CHECK(fabsf(val(FF_VGEO_LOD_PX) - 2.0f) < 1e-3f, "2010-era Low: discrete LOD error locked at 2 px (was 12 x4): %g", val(FF_VGEO_LOD_PX));
    CHECK(fabsf(val(FF_FOLIAGE_DENSITY) - 1.0f) < 1e-4f, "2010-era Low: full foliage (locked; was 0.2): %g", val(FF_FOLIAGE_DENSITY));
    set_era(FID_ERA_2020);
    fid_apply(FID_LOW);
    CHECK(fabsf(val(FF_VGEO_ERROR_PX) - 1.0f) < 1e-3f, "2020-era Low: vgeo error locked at 1 px (was 2 x3): %g", val(FF_VGEO_ERROR_PX));
    CHECK(fabsf(val(FF_VGEO_SHADOW_ERROR_PX) - 12.0f) < 1e-3f, "2020-era Low: shadow-view error 4 px x3 = %g (a tier)", val(FF_VGEO_SHADOW_ERROR_PX));
    fid_apply(FID_MEDIUM);
    CHECK(fabsf(val(FF_FOLIAGE_DENSITY) - 1.0f) < 1e-4f && val(FF_SSR) == 0, "2020-era Medium: full foliage (locked)");
    // Custom era (4) behaves as modern.
    cvar_set_int(g_era, 4);
    CHECK(fid_era() == FID_ERA_MODERN, "custom era = modern capabilities");
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
}

// Locked features: the same value at every level x era, whatever was applied before, equal to the
// consistency set; their bounds are that one value; console sets of the cvar don't go through the
// fidelity bounds. Visibility floors of the tier features hold at every level x era.
static void test_locked(void) {
    float ref[FF_COUNT];
    bool have = false;
    for (int e = 0; e < FID_ERAS; e++) {
        set_era(e);
        for (int l = 0; l < FID_LEVELS; l++) {
            fid_apply((l + 2) % FID_LEVELS);
            fid_apply(l);
            for (int f = 0; f < fid_feature_count(); f++) {
                if (!fid_feature_locked(f)) continue;
                const FidFeatureDef *d = fid_feature(f);
                float v = val(f);
                const char *cs = consistency_find(d->cvar);
                CHECK(cs && fabsf(v - (float)atof(cs)) < 1e-4f, "%s/%s: %s = %g, the consistency set says %s", fid_era_id(e), fid_level_id(l), d->cvar, v, cs ? cs : "-");
                if (have) CHECK(fabsf(v - ref[f]) < 1e-4f, "%s/%s: %s = %g differs from %g at modern/off", fid_era_id(e), fid_level_id(l), d->cvar, v, ref[f]);
                ref[f] = v;
                FidBounds b = fid_bounds(l, e, f);
                CHECK(b.avail == FID_DEFAULT_ON && fabsf(b.lo - b.hi) < 1e-5f && fabsf(b.def - v) < 1e-4f, "%s/%s: %s bounds are the one locked value", fid_era_id(e), fid_level_id(l), d->id);
            }
            have = true;
            CHECK(val(FF_SHADOWS) >= 1, "%s/%s: at least Hard shadows (%g)", fid_era_id(e), fid_level_id(l), val(FF_SHADOWS));
            CHECK(!fid_value_allowed(l, e, FF_SHADOWS, 0), "%s/%s: shadows can't be disabled", fid_era_id(e), fid_level_id(l));
            CHECK(val(FF_WATER) >= 1 && !fid_value_allowed(l, e, FF_WATER, 0), "%s/%s: water keeps its refraction (%g)", fid_era_id(e), fid_level_id(l), val(FF_WATER));
            CHECK(val(FF_PRECIP_QUALITY) >= 1 && !fid_value_allowed(l, e, FF_PRECIP_QUALITY, 0) && !fid_value_allowed(l, e, FF_PRECIP_QUALITY, -1), "%s/%s: rain / snow never off (%g)",
                  fid_era_id(e), fid_level_id(l), val(FF_PRECIP_QUALITY));
        }
    }
    // A tier table listing a locked feature is rejected.
    char err[256] = "";
    char *text = NULL;
    {
        FILE *f = fopen("assets/render/fidelity/low.json", "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            text = (char *)calloc(1, (size_t)n + 64);
            if (fread(text, 1, (size_t)n, f) != (size_t)n) text[0] = 0;
            fclose(f);
        }
    }
    CHECK(text && text[0], "low.json readable");
    if (text) {
        char *at = strstr(text, "\"features\": {");
        if (at) {
            at += strlen("\"features\": {");
            size_t rest = strlen(at);
            const char *ins = "\"foliage_density\": {\"avail\": \"on\", \"max\": 0.25},";
            char *mod = (char *)calloc(1, strlen(text) + strlen(ins) + 8);
            memcpy(mod, text, (size_t)(at - text));
            strcat(mod, ins);
            memcpy(mod + strlen(mod), at, rest + 1);
            CHECK(fid_load_level_text(FID_LOW, mod, err, sizeof(err)), "low + foliage_density parses");
            CHECK(!fid_tables_complete(err, sizeof(err)) && strstr(err, "locked"), "a tier table setting a locked feature is rejected: %s", err);
            free(mod);
        }
        free(text);
    }
    char ok_err[256];
    CHECK(fid_load_dir("assets/render/fidelity", ok_err, sizeof(ok_err)), "reload: %s", ok_err);
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
}

static void test_passes(void) {
    const char *post[] = { "hiz", "contact", "gtao", "gtao_temporal", "gtao_upsample", "ssgi", "ssgi_temporal", "ssr", "ssr_temporal", "vol_inject",
                           "vol_integrate", "color_pyramid", "depth_history", "taa", "taau", "fxaa", "smaa_prep", "smaa_blend", "fsr_easu", "mb_tiles",
                           "mb_blur", "dof_focus", "dof_gather", "lens_drops", "lens_apply", "post_buffers_init", "exposure", "bloom", "flare", "tonemap" };
    const char *scene[] = { "prepass", "vgeo_cull", "vgeo_raster", "vgeo_hiz", "opaque", "decals", "opaque_dynamic", "decals_dynamic", "sky", "sky_luts",
                            "sky_view", "fog_apply", "scene_copy", "transparent", "vm_prepass", "viewmodel", "scope_pip", "scope_pip_fx", "sight_scope",
                            "sight_reddot", "phone_post0", "ui_blur", "ui_views", "final", "exr_capture" };
    for (int i = 0; i < ARRAY_COUNT(post); i++) CHECK(fid_pass_is_post(post[i]), "%s is a post pass", post[i]);
    for (int i = 0; i < ARRAY_COUNT(scene); i++) CHECK(!fid_pass_is_post(scene[i]), "%s is a scene pass", scene[i]);
}

// Frame graph check on hand-built graphs (the renderer runs the same analysis on every frame).
static void test_graph_check(void) {
    enum { HDR, DEPTH, SWAP, NORMAL, VEL, AO_HALF, AO_HA, AO_HB, AO, DEPTH_PREV, LDR, UNUSED, RES_N };
    FgcRes res[RES_N] = { { "hdr", FGC_IMPORT }, { "depth", FGC_IMPORT }, { "swapchain", FGC_IMPORT }, { "gb_normal", FGC_TRANSIENT },
                          { "gb_velocity", FGC_TRANSIENT }, { "ao_half", FGC_TRANSIENT }, { "ao_hist_a", FGC_HISTORY }, { "ao_hist_b", FGC_HISTORY },
                          { "ao", FGC_TRANSIENT }, { "depth_prev", FGC_HISTORY }, { "ldr", FGC_TRANSIENT }, { "unused", FGC_TRANSIENT } };
    // Off-style graph: scene + direct output.
    FgcUse pre[] = { { DEPTH, true }, { NORMAL, true } }, opq[] = { { HDR, true }, { DEPTH, true } }, fin[] = { { HDR, false }, { SWAP, true } };
    FgcPass off[] = { { "prepass", pre, 2 }, { "opaque", opq, 2 }, { "final", fin, 2 } };
    FgcReport r;
    fgc_analyze(off, 3, res, NORMAL + 1, &r);
    CHECK(r.dead_passes == 0 && r.unused_res == 0 && r.undefined_reads == 0, "off graph clean (%d dead, %d unused, %d undefined)", r.dead_passes, r.unused_res, r.undefined_reads);
    int post = 0;
    for (int i = 0; i < 3; i++) post += fid_pass_is_post(off[i].name);
    CHECK(post == 0, "off graph: 0 post passes");
    // gb_normal written but never read -> the prepass still writes depth (import): live.
    // AO chain with ping-pong history and a depth history nobody reads -> depth_history is dead.
    FgcUse gtao[] = { { DEPTH, false }, { NORMAL, false }, { AO_HALF, true } };
    FgcUse tmp[] = { { AO_HALF, false }, { AO_HA, false }, { DEPTH_PREV, false }, { AO_HB, true } };
    FgcUse up[] = { { AO_HB, false }, { AO, true } };
    FgcUse opq2[] = { { HDR, true }, { AO, false } };
    FgcUse dh[] = { { DEPTH, false }, { DEPTH_PREV, true } };
    FgcUse tm[] = { { HDR, false }, { LDR, true } };
    FgcUse fin2[] = { { LDR, false }, { SWAP, true } };
    FgcPass full[] = { { "prepass", pre, 2 }, { "gtao", gtao, 3 }, { "gtao_temporal", tmp, 4 }, { "gtao_upsample", up, 2 }, { "opaque", opq2, 2 },
                       { "depth_history", dh, 2 }, { "tonemap", tm, 2 }, { "final", fin2, 2 } };
    fgc_analyze(full, 8, res, RES_N, &r);
    CHECK(r.dead_passes == 0, "temporal AO + depth history consumed: %d dead (%s)", r.dead_passes, r.dead_passes ? r.dead[0] : "");
    CHECK(r.unused_res == 2, "gb_velocity + 'unused' declared, never used: %d", r.unused_res);
    // Without the AO chain, the depth history has no reader: dead.
    FgcPass noao[] = { { "prepass", pre, 2 }, { "opaque", opq, 2 }, { "depth_history", dh, 2 }, { "tonemap", tm, 2 }, { "final", fin2, 2 } };
    fgc_analyze(noao, 5, res, RES_N, &r);
    CHECK(r.dead_passes == 1 && !strcmp(r.dead[0], "depth_history"), "depth history without a reader is dead (%d)", r.dead_passes);
    // A transient only written (tonemap output not read) -> dead; a read before any write -> undefined.
    FgcUse rd[] = { { LDR, false }, { SWAP, true } };
    FgcPass bad[] = { { "final", rd, 2 }, { "tonemap", tm, 2 } };
    fgc_analyze(bad, 2, res, RES_N, &r);
    CHECK(r.dead_passes == 1 && r.undefined_reads == 1, "write-only tonemap dead (%d), ldr read before written (%d)", r.dead_passes, r.undefined_reads);
    CHECK(fgc_history_partner("taa_hist_a", "taa_hist_b") && fgc_history_partner("depth_prev", "depth_prev") && !fgc_history_partner("ao_hist_a", "ssr_hist_b"), "history partners");
}

// Settings > Graphics > Normal qualities (fid_group_*): at every level x era, the level's own tier
// reads back after fid_apply; tiers up to the level apply and read back as themselves (or a tier
// with the same values), tiers above it are greyed with the level they need; a group change makes
// the level read Custom; re-applying the level clears it.
static void test_groups(void) {
    for (int e = 0; e < FID_ERAS; e++) {
        set_era(e);
        for (int l = 0; l < FID_LEVELS; l++) {
            fid_custom_load("");
            fid_apply(l);
            for (int g = 0; g < FG_COUNT; g++) {
                const int *fs;
                CHECK(fid_group_features(g, &fs) > 0 && fs, "group %s has features", fid_group_id(g));
                CHECK(fid_group_detect(g) == l, "era %s %s: group %s reads %d", fid_era_id(e), fid_level_id(l), fid_group_id(g), fid_group_detect(g));
                for (int t = 0; t < FID_LEVELS; t++) {
                    int req = -2;
                    bool ok = fid_group_tier_allowed(g, t, &req);
                    if (t <= l) CHECK(ok && req <= l, "era %s %s: %s tier %d allowed (req %d)", fid_era_id(e), fid_level_id(l), fid_group_id(g), t, req);
                    else CHECK(ok == (req >= 0 && req <= l), "era %s %s: %s tier %d greyed with req %d", fid_era_id(e), fid_level_id(l), fid_group_id(g), t, req);
                    if (!ok) continue;
                    fid_group_apply(g, t);
                    int got = fid_group_detect(g);
                    CHECK(got >= 0, "era %s %s: %s tier %d reads back (got %d)", fid_era_id(e), fid_level_id(l), fid_group_id(g), t, got);
                    if (got >= 0 && got != t) {   // equal values: the same tier under another name
                        int n = fid_group_features(g, &fs);
                        for (int i = 0; i < n; i++)
                            CHECK(fabsf(fid_bounds(got, e, fs[i]).def - fid_bounds(t, e, fs[i]).def) < 1e-3f || fid_bounds(l, e, fs[i]).avail == FID_UNAVAILABLE,
                                  "%s tier %d read as %d: same %s", fid_group_id(g), t, got, fid_feature(fs[i])->id);
                    }
                    CHECK(g_era->i == e, "era %s survives %s tier %d (r_era %d)", fid_era_id(e), fid_group_id(g), t, g_era->i);
                }
                fid_custom_load("");
                fid_apply(l);
            }
        }
    }
    set_era(FID_ERA_MODERN);
    fid_custom_load("");
    fid_apply(FID_MODERN);
    CHECK(fid_custom_count() == 0, "Modern: no custom values");
    fid_group_apply(FG_SHADOWS, FID_LOW);
    CHECK(fid_custom_count() > 0 && fid_group_detect(FG_SHADOWS) == FID_LOW, "Low shadows on Modern: Custom (%d), shadows Low", fid_custom_count());
    CHECK(val(FF_SHADOW_RESOLUTION) == fid_bounds(FID_LOW, FID_ERA_MODERN, FF_SHADOW_RESOLUTION).def, "shadow resolution at Low's default");
    CHECK(fid_group_detect(FG_TEXTURES) == FID_MODERN, "textures untouched");
    fid_custom_load("");
    fid_apply(FID_MODERN);
    CHECK(fid_custom_count() == 0 && fid_group_detect(FG_SHADOWS) == FID_MODERN, "re-applying Modern clears it");
    fid_apply(FID_MEDIUM);
    int req = -1;
    CHECK(!fid_group_tier_allowed(FG_POST, FID_MODERN, &req) && req > FID_MEDIUM, "Medium: Ultra post greyed (requires %d)", req);
    fid_apply(FID_MODERN);
}

static void test_malformed(void) {
    char err[256];
    CHECK(!fid_load_level_text(FID_LOW, "{\"level\":\"low\",\"features\":{\"ssr2\":{\"avail\":\"no\"}}}", err, sizeof(err)) && strstr(err, "ssr2"), "unknown feature: %s", err);
    CHECK(!fid_load_level_text(FID_LOW, "{\"level\":\"low\",\"features\":{\"ssr\":{\"avail\":\"maybe\"}}}", err, sizeof(err)), "bad avail: %s", err);
    CHECK(!fid_load_level_text(FID_LOW, "{\"level\":\"high\",\"features\":{}}", err, sizeof(err)), "level id mismatch: %s", err);
    CHECK(!fid_load_level_text(FID_LOW, "{\"level\":\"low\",\"features\":{\"ssr\":{\"avail\":\"no\",\"maximum\":1}}}", err, sizeof(err)), "unknown key: %s", err);
    // An incomplete level (only one feature) fails the completeness check.
    CHECK(fid_load_level_text(FID_LOW, "{\"level\":\"low\",\"features\":{\"ssr\":{\"avail\":\"no\"}}}", err, sizeof(err)), "partial level parses");
    CHECK(!fid_tables_complete(err, sizeof(err)) && strstr(err, "no entry"), "incomplete: %s", err);
    CHECK(!fid_load_eras_text("{\"eras\":{\"1999\":{}}}", err, sizeof(err)), "unknown era: %s", err);
    char ok_err[256];
    CHECK(fid_load_dir("assets/render/fidelity", ok_err, sizeof(ok_err)), "reload: %s", ok_err);
}

int main(void) {
    cvar_system_init();
    CHECK(consistency_load(CONSISTENCY_PATH), "%s loads", CONSISTENCY_PATH);
    char err[256] = "";
    if (!fid_load_dir("assets/render/fidelity", err, sizeof(err))) { printf("FAIL tables: %s\n", err); return 1; }
    register_cvars();
    fid_attach_cvars();
    fid_set_level(FID_MODERN);
    test_tables();
    test_apply_matches_table();
    test_options();
    test_lower_raise();
    test_levels();
    test_eras();
    test_locked();
    test_groups();
    test_passes();
    test_graph_check();
    test_malformed();
    printf("test_fidelity: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
