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
//   * fair play (docs/SETTINGS_AUDIT.md): no level disables sun shadows or rain, and no visibility
//     parameter (foliage density, LOD / impostor ranges, shadow distance, fog, lens) is a feature;
//   * Off: no post-processing; frame graph pass classification;
//   * frame graph validation (fg_check): dead passes, unused resources, undefined reads, history
//     ping-pong;
//   * malformed tables are rejected with a message.
#include "render/fidelity/fidelity.h"
#include "render/graph/fg_check.h"
#include "core/engine.h"
#include "core/cvar.h"
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

// Era preset values (vk_vgeo.c k_eras): shadow error px.
static const float k_era_vals[FID_ERAS][1] = { { 2 }, { 4 }, { 2 }, { 3 } };
static Cvar *g_era;

static void register_cvars(void) {
    const int owned[1] = { FF_VGEO_SHADOW_ERROR_PX };
    for (int e = 0; e < FID_ERAS; e++)
        for (int k = 0; k < 1; k++) fid_set_era_value(e, owned[k], k_era_vals[e][k]);
    // Every feature cvar with its engine range and the Modern default (= the engine default).
    for (int f = 0; f < fid_feature_count(); f++) {
        const FidFeatureDef *d = fid_feature(f);
        FidBounds b = fid_bounds(FID_MODERN, FID_ERA_MODERN, f);
        if (d->kind == FID_BOOL) cvar_reg_bool(d->cvar, b.def != 0, CVAR_ARCHIVE, d->label);
        else if (d->kind == FID_INT) cvar_reg_int(d->cvar, (int)b.def, (int)d->lo, (int)d->hi, CVAR_ARCHIVE, d->label);
        else cvar_reg_float(d->cvar, b.def, d->lo, d->hi, CVAR_ARCHIVE, d->label);
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
    const int owned[1] = { FF_VGEO_SHADOW_ERROR_PX };
    for (int k = 0; k < 1; k++) set(owned[k], k_era_vals[e][k]);
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
    CHECK(!fid_level_post(FID_OFF) && fid_level_post(FID_LOW) && fid_level_post(FID_MODERN), "only Off has no post-processing");
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
                FidBounds b = fid_bounds(l, e, f);
                int steps = d->kind == FID_FLOAT ? 20 : (int)(d->hi - d->lo);
                for (int s = 0; s <= steps; s++) {
                    float v = d->lo + (d->hi - d->lo) * (float)s / (float)DG_MAX(steps, 1);
                    if (d->kind != FID_FLOAT) v = roundf(v);
                    bool allowed = fid_value_allowed(l, e, f, v);
                    int req = fid_min_level_for(e, f, v);
                    if (allowed) CHECK(req >= 0 && req <= l, "%s=%g allowed at %s but requires %d", d->id, v, fid_level_id(l), req);
                    else CHECK(req < 0 || req > l, "%s=%g greyed at %s but requires %d", d->id, v, fid_level_id(l), req);
                    // Monotonic: allowed here -> allowed one level up (every option keeps working).
                    if (allowed && l + 1 < FID_LEVELS && f != FF_UPSCALER && f != FF_PRECIP_QUALITY)
                        CHECK(fid_value_allowed(l + 1, e, f, v), "%s=%g allowed at %s but not at %s (era %s)", d->id, v, fid_level_id(l), fid_level_id(l + 1), fid_era_id(e));
                    set(f, v);
                    float got = val(f);
                    if (allowed) CHECK(fabsf(got - v) < 1e-3f, "%s=%g allowed at %s but stored %g", d->id, v, fid_level_id(l), got);
                    else {
                        CHECK(fid_allowed(&b, d, got) || (b.avail == FID_UNAVAILABLE && !d->has_off), "%s=%g clamped to %g (not allowed) at %s", d->id, v, got, fid_level_id(l));
                        bool choice = f == FF_UPSCALER || f == FF_PRECIP_QUALITY;
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
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_UPSCALER) == 2, "Medium keeps the chosen FSR 1 (allowed)");
    set(FF_UPSCALER, 3);
    CHECK(val(FF_UPSCALER) != 3, "Medium: the FSR 2 slot greyed");
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
    CHECK(val(FF_SHADOWS) == 1 && val(FF_SHADOW_CASCADES) == 1 && val(FF_RENDER_SCALE) >= 0.5f && val(FF_RENDER_SCALE) <= 0.66f + 1e-4f, "Off: hard shadows (one cascade), 0.5-0.66 scale");
    CHECK(fabsf(val(FF_VGEO_SHADOW_ERROR_PX) - 8.0f) < 1e-4f, "Off: shadow geometry error x4 (%g)", val(FF_VGEO_SHADOW_ERROR_PX));
    // Fair play (docs/SETTINGS_AUDIT.md): every level and era keeps sun shadows and the rain.
    for (int e = 0; e < FID_ERAS; e++)
        for (int l = 0; l < FID_LEVELS; l++) {
            CHECK(!fid_value_allowed(l, e, FF_SHADOWS, 0) && fid_bounds(l, e, FF_SHADOWS).def >= 1, "%s/%s: sun shadows cannot be disabled", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_PRECIP_QUALITY, 0) && fid_bounds(l, e, FF_PRECIP_QUALITY).def != 0, "%s/%s: rain / snow cannot be switched off", fid_level_id(l), fid_era_id(e));
        }
    // No visibility parameter is a fidelity feature: the consistency set owns them.
    static const char *const locked[] = { "r_foliage_density", "r_vgeo_impostor_px", "r_classic_impostor_px", "r_vgeo_error_px", "r_vgeo_lod_px", "r_lod_bias", "r_char_quality",
                                          "r_shadow_max_distance", "r_cloud_shadows", "r_light_shadows", "r_moon_shadows", "r_fog_scale", "r_auto_exposure", "r_exposure", "r_tonemapper",
                                          "r_lens_flare", "r_lens_dirt", "r_lens_distortion", "r_lens_drops", "vfx_decal_max", "r_bloom_intensity", "r_gamma" };
    for (int i = 0; i < ARRAY_COUNT(locked); i++) CHECK(fid_feature_by_cvar(locked[i]) < 0, "%s is a visibility parameter, not a fidelity feature", locked[i]);
    // Internal consistency: TAA (and TAA-U) only where the level has TAA; HDR output only High / Modern.
    for (int e = 0; e < FID_ERAS; e++)
        for (int l = 0; l < FID_LEVELS; l++) {
            bool taa = fid_value_allowed(l, e, FF_AA, 3);
            CHECK(!fid_value_allowed(l, e, FF_UPSCALER, 1) || taa, "%s/%s: TAA-U without TAA", fid_level_id(l), fid_era_id(e));
            CHECK(fid_value_allowed(l, e, FF_HDR_OUTPUT, 1) == ((l >= FID_HIGH) && e != FID_ERA_2010), "%s/%s: HDR output", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_PUDDLE_REFLECTIONS, 1) || fid_value_allowed(l, e, FF_SSR, 1), "%s/%s: puddles need SSR", fid_level_id(l), fid_era_id(e));
            CHECK(!fid_value_allowed(l, e, FF_ADS_DOF, 1) || fid_value_allowed(l, e, FF_DOF, 1), "%s/%s: ADS DoF needs DoF", fid_level_id(l), fid_era_id(e));
        }
    // The spec's per-level points.
    fid_apply(FID_LOW);
    CHECK(val(FF_AA) == 1 && val(FF_SHADOW_CASCADES) == 2 && val(FF_SHADOWS) == 1 && val(FF_SHADOW_RESOLUTION) == 1 && fabsf(val(FF_RENDER_SCALE) - 0.66f) < 1e-3f, "Low: FXAA, Hard Only, 1024 px (the locked 150 m), 2 cascades, 0.66");
    CHECK(fabsf(val(FF_VGEO_SHADOW_ERROR_PX) - 6.0f) < 1e-4f && val(FF_TEXTURE_QUALITY) == 0, "Low: shadow geometry x3, mips -2");
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_SSR) == 0 && val(FF_MOTION_BLUR) == 1 && val(FF_DOF) == 1 && val(FF_UPSCALER) == 1 && fabsf(val(FF_UPSCALE_RATIO) - 0.75f) < 1e-4f, "Medium: no SSR, camera MB, ADS DoF, TAA-U 0.75 (PF-0029)");
    CHECK(val(FF_VOL_LOCAL_LIGHTS) == 0 && val(FF_VOLUMETRICS) == 1 && val(FF_VFX_LIGHTING) == 0, "Medium: fog-only, unlit particles");
    CHECK(!fid_value_allowed(FID_MEDIUM, 0, FF_CHROMATIC, 0.5f), "Medium: CA greyed");
    fid_apply(FID_HIGH);
    CHECK(val(FF_UPSCALER) == 1 && fabsf(val(FF_UPSCALE_RATIO) - 0.75f) < 1e-4f, "High: TAA-U 0.75 (PF-0029)");
    CHECK(val(FF_SSR) == 1 && val(FF_SSGI) == 0 && !fid_value_allowed(FID_HIGH, 0, FF_SSGI, 1) && val(FF_BLOOM_QUALITY) == 1 && val(FF_DOF_QUALITY) == 1, "High: SSR half, SSGI greyed, fewer bloom mips, hybrid DoF");
    CHECK(val(FF_SHADOW_CASCADES) == 4 && val(FF_SOFT_SHADOW_QUALITY) == 1, "High: 4 cascades, soft Medium");
    fid_apply(FID_MODERN);
    for (int f = 0; f < fid_feature_count(); f++)
        if (f != FF_HDR_OUTPUT) CHECK(fid_bounds(FID_MODERN, 0, f).avail != FID_UNAVAILABLE, "Modern: %s unlocked", fid_feature(f)->id);
}

static void test_eras(void) {
    set_era(FID_ERA_2010);
    fid_apply(FID_MODERN);
    CHECK(val(FF_SSR) == 0 && val(FF_SSGI) == 0 && val(FF_VOLUMETRICS) == 0, "2010: no SSR / SSGI / froxels");
    CHECK(fid_bounds(FID_MODERN, FID_ERA_2010, FF_SSR).era_lacks && fid_min_level_for(FID_ERA_2010, FF_SSR, 1) < 0, "2010 lacks SSR at every level");
    CHECK(val(FF_AA) == 2, "2010 Modern: SMAA (no TAA in the era): %g", val(FF_AA));
    cvar_set("r_aa", "3");
    CHECK(val(FF_AA) != 3, "2010: TAA greyed");
    fid_apply(FID_LOW);
    CHECK(val(FF_AA) == 2 && val(FF_SHADOWS) == 2, "2010-era Low differs from modern-era Low (SMAA, soft shadows)");
    set_era(FID_ERA_2020);
    fid_apply(FID_LOW);
    CHECK(fabsf(val(FF_VGEO_SHADOW_ERROR_PX) - 12.0f) < 1e-3f, "2020-era Low: shadow geometry 4 px x3 = %g", val(FF_VGEO_SHADOW_ERROR_PX));
    fid_apply(FID_MEDIUM);
    CHECK(val(FF_SSR) == 0, "2020-era Medium: no SSR");
    // Custom era (4) behaves as modern.
    cvar_set_int(g_era, 4);
    CHECK(fid_era() == FID_ERA_MODERN, "custom era = modern capabilities");
    set_era(FID_ERA_MODERN);
    fid_apply(FID_MODERN);
}

static void test_passes(void) {
    const char *post[] = { "hiz", "contact", "gtao", "gtao_temporal", "gtao_upsample", "ssgi", "ssgi_temporal", "ssr", "ssr_temporal", "vol_inject",
                           "vol_integrate", "color_pyramid", "depth_history", "taa", "taau", "fxaa", "smaa_prep", "smaa_blend", "fsr_easu", "mb_tiles",
                           "mb_blur", "dof_focus", "dof_gather", "post_buffers_init", "exposure", "bloom", "flare", "tonemap" };
    const char *scene[] = { "prepass", "opaque", "decals", "opaque_dynamic", "decals_dynamic", "sky", "sky_luts",
                            "sky_view", "fog_apply", "scene_copy", "transparent", "vm_prepass", "viewmodel", "scope_pip", "scope_pip_fx", "sight_scope",
                            "sight_reddot", "phone_post0", "ui_blur", "ui_views", "final", "exr_capture", "lens_drops", "lens_apply" };
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

// The player's quality settings (fid_group_*, docs/ENGINE_FIDELITY.md "Player settings"): as in the
// game (cvars bound, no level bounds), at every era x level x group x tier the group's features take
// the tier's values whatever the level, the tier reads back as itself (or a tier with the same
// values), nothing outside the group moves; anti-aliasing steps FXAA, SMAA, TAA, TAA.
static void test_groups(void) {
    fid_detach_cvars();
    fid_bind_cvars();
    int seen[FF_COUNT] = { 0 };
    for (int g = 0; g < FG_COUNT; g++) {
        const int *fs;
        int n = fid_group_features(g, &fs);
        CHECK(n > 0 && fs, "group %s has features", fid_group_id(g));
        for (int i = 0; i < n; i++) seen[fs[i]]++;
    }
    for (int f = 0; f < FF_COUNT; f++) CHECK(seen[f] <= 1, "%s in at most one group (%d)", fid_feature(f)->id, seen[f]);
    for (int e = 0; e < FID_ERAS; e++) {
        set_era(e);
        for (int l = 0; l < FID_LEVELS; l++) {
            fid_init_level(l);
            for (int g = 0; g < FG_COUNT; g++) {
                const int *fs;
                int n = fid_group_features(g, &fs);
                for (int t = FID_LOW; t < FID_LEVELS; t++) {
                    float before[FF_COUNT];
                    for (int f = 0; f < FF_COUNT; f++) before[f] = fid_cvar_value(f);
                    fid_group_apply(g, t);
                    for (int i = 0; i < n; i++) {
                        float want;
                        if (!fid_group_value(g, t, fs[i], &want)) continue;
                        CHECK(fabsf(val(fs[i]) - want) < 1e-3f, "era %s level %s: %s tier %d sets %s to %g (has %g)", fid_era_id(e), fid_level_id(l), fid_group_id(g), t,
                              fid_feature(fs[i])->id, want, val(fs[i]));
                    }
                    int got = fid_group_detect(g);
                    CHECK(got >= t, "era %s level %s: %s tier %d reads back (got %d)", fid_era_id(e), fid_level_id(l), fid_group_id(g), t, got);
                    for (int f = 0; f < FF_COUNT; f++) {
                        bool in = false;
                        for (int i = 0; i < n; i++) in |= fs[i] == f;
                        if (!in && !isnan(before[f])) CHECK(fabsf(fid_cvar_value(f) - before[f]) < 1e-5f, "%s tier %d leaves %s alone", fid_group_id(g), t, fid_feature(f)->id);
                    }
                    CHECK(g_era->i == e, "era %s survives %s tier %d (r_era %d)", fid_era_id(e), fid_group_id(g), t, g_era->i);
                }
            }
        }
    }
    set_era(FID_ERA_MODERN);
    fid_init_level(FID_MODERN);
    static const float aa[FID_LEVELS] = { 0, 1, 2, 3, 3 };
    for (int t = FID_LOW; t < FID_LEVELS; t++) {
        fid_group_apply(FG_AA, t);
        CHECK(val(FF_AA) == aa[t], "anti-aliasing tier %d = %g (has %g)", t, aa[t], val(FF_AA));
    }
    // No level bound: Ultra shadows on the Low tables' level stay Ultra.
    fid_init_level(FID_LOW);
    fid_group_apply(FG_SHADOWS, FID_MODERN);
    CHECK(val(FF_SHADOW_RESOLUTION) == fid_bounds(FID_MODERN, FID_ERA_MODERN, FF_SHADOW_RESOLUTION).def, "Ultra shadows on Low: Ultra's resolution");
    fid_init_level(FID_MODERN);
    fid_detach_cvars();
    fid_attach_cvars();
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

// GPU families of the auto-detect cap (docs/PLATFORM_MACOS.md): MoltenVK device names / vendor ids.
static void test_gpu_families(void) {
    CHECK(fid_gpu_family(0x106B, "Apple M1", true) == FID_GPU_APPLE, "M1");
    CHECK(fid_gpu_family(0x106B, "Apple M3 Max", true) == FID_GPU_APPLE_PRO, "M3 Max");
    CHECK(fid_gpu_family(0x106B, "Apple M2 Ultra", true) == FID_GPU_APPLE_PRO, "M2 Ultra");
    CHECK(fid_gpu_family(0x106B, "Apple M4 Pro", true) == FID_GPU_APPLE_PRO, "M4 Pro");
    CHECK(fid_gpu_family(0x8086, "Intel(R) UHD Graphics 630", true) == FID_GPU_MAC_INTEL, "Intel Mac");
    CHECK(fid_gpu_family(0x1002, "AMD Radeon Pro 5500M", true) == FID_GPU_MAC_AMD, "AMD Mac");
    CHECK(fid_gpu_family(0x106B, "Apple Paravirtual device", true) == FID_GPU_APPLE_VM && fid_gpu_level_cap(FID_GPU_APPLE_VM) == FID_LOW, "macOS VM");
    CHECK(fid_gpu_family(0x8086, "Intel(R) Arc(TM) A770", false) == FID_GPU_OTHER, "Intel on Windows / Linux");
    CHECK(fid_gpu_family(0x10DE, "NVIDIA GeForce RTX 3060 Ti", false) == FID_GPU_OTHER, "NVIDIA");
    CHECK(fid_gpu_level_cap(FID_GPU_MAC_INTEL) == FID_MEDIUM && fid_gpu_level_cap(FID_GPU_APPLE) == FID_HIGH, "caps");
    CHECK(fid_gpu_level_cap(FID_GPU_OTHER) == FID_MODERN && fid_gpu_level_cap(FID_GPU_APPLE_PRO) == FID_MODERN, "no cap elsewhere");
}

int main(void) {
    test_gpu_families();
    cvar_system_init();
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
    test_groups();
    test_passes();
    test_graph_check();
    test_malformed();
    printf("test_fidelity: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
