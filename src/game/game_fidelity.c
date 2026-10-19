// Graphics quality, game side (docs/ENGINE_FIDELITY.md "Player settings"; tables: src/render/fidelity):
//   * the player's quality settings (Settings > Graphics > Quality) are the only thing that sets the
//     quality features: r_quality_aa / _post / _shadows / _textures / _effects / _foliage /
//     _view_distance (0 Low .. 3 Ultra, archived), each its group's features at that tier
//     (fid_group_apply); Overall sets all seven. No level bounds, auto-detect, presets or remembered
//     overrides: the frame graph follows live (no restart), the pool drops what it no longer uses;
//   * r_fidelity (locked Modern): the tables' level, switched only by the dev tools below (which put
//     the player's values back when done);
//   * bench_fidelity <level|all> [frames]: the fixed devroom flythrough, per pass GPU ms logged;
//   * fidelity_check / fidelity_test: frame graph validation (no dead passes, Off = zero post
//     passes), cvars against the tables, 50 level switches without leaking GPU memory;
//   * r_perf_overlay: FPS / frame time overlay.
#include "game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
#include "ui/ui.h"
#include "ui/screens/screens.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RUN_NONE, RUN_BENCH, RUN_TEST } RunKind;

#define BENCH_MAX_PASSES 96
#define HIST 180

static struct {
    Cvar *level, *overlay;
    Cvar *quality[FG_COUNT];           // the player's tier per group (0 Low .. 3 Ultra)
    bool ready, loading, quiet;
    int trim_in;                       // frames until the pool trim after a quality / level switch
    // bench / test runs (one at a time, a frame-driven state machine)
    RunKind run;
    int step, frame, levels[FID_LEVELS + 1], level_count, frames_per;
    bool quit_after;
    char saved_values[FF_COUNT][CVAR_STR_MAX];
    int saved_level;
    // bench
    char pass_names[BENCH_MAX_PASSES][32];
    double pass_ms[BENCH_MAX_PASSES];
    int pass_count;
    double gpu_sum, cpu_sum;
    int measured;
    float bench_result[FID_LEVELS];
    // test
    int fails;
    u64 mem_before;
    // overlay history (CPU frame ms)
    float hist[HIST];
    int hist_pos;
} F;

// ------------------------------------------------------------------ the player's quality settings

int game_fidelity_level(void) { return fid_level(); }
bool game_fidelity_busy(void) { return F.run != RUN_NONE; }

int game_quality_tier(int group) {
    if (group < 0 || group >= FG_COUNT || !F.quality[group]) return 3;
    return DG_CLAMP(F.quality[group]->i, 0, 3);
}

void game_quality_set(int group, int tier) {
    if (group >= 0 && group < FG_COUNT && F.quality[group]) cvar_set_int(F.quality[group], DG_CLAMP(tier, 0, 3));
}

int game_quality_overall(void) {
    int t = game_quality_tier(0);
    for (int g = 1; g < FG_COUNT; g++) if (game_quality_tier(g) != t) return -1;
    return t;
}

void game_quality_set_overall(int tier) {
    for (int g = 0; g < FG_COUNT; g++) game_quality_set(g, tier);
}

// Tier t of the settings (0 Low .. 3 Ultra) = the tables' level t + 1 (Low .. Modern).
static void apply_group(int g) {
    fid_group_apply(g, game_quality_tier(g) + FID_LOW);
    F.trim_in = 3;   // once the new graph has run: drop the pool images it no longer uses
}

void game_quality_apply(void) {
    if (!F.ready) return;
    for (int g = 0; g < FG_COUNT; g++) apply_group(g);
}

static void on_quality(Cvar *c) {
    if (!F.ready || F.loading || F.run != RUN_NONE) return;
    for (int g = 0; g < FG_COUNT; g++)
        if (F.quality[g] == c) {
            apply_group(g);
            log_info("quality: %s %s", fid_group_id(g), (const char *[]){ "low", "medium", "high", "ultra" }[game_quality_tier(g)]);
        }
}

static void after_level_change(int level) {
    (void)level;
    F.trim_in = 3;   // once the new graph has run: drop the pool images it no longer uses
}

// r_fidelity: only the dev tools switch it (a level's table values on every feature).
static void on_level(Cvar *c) {
    if (!F.ready) return;
    int l = DG_CLAMP(c->i, 0, FID_LEVELS - 1);
    if (F.loading || F.quiet) {   // config load / restore: the level the tables read, nothing applied
        fid_init_level(l);
        return;
    }
    fid_apply(l);
    after_level_change(l);
    log_info("fidelity: %s (%s): every feature at its table value (dev; the quality settings re-apply on their next change)", fid_level_name(l), fid_level_desc(l));
}

void game_fidelity_set(int level) { cvar_set_int(F.level, DG_CLAMP(level, 0, FID_LEVELS - 1)); }

// ------------------------------------------------------------------ snapshot (bench / test restore)

static void snapshot(void) {
    for (int f = 0; f < FF_COUNT; f++) {
        Cvar *c = cvar_find(fid_feature(f)->cvar);
        dg_strcpy(F.saved_values[f], CVAR_STR_MAX, c ? c->str : "");
    }
    F.saved_level = F.level->i;
}

static void restore(void) {
    F.quiet = true;
    cvar_set_int(F.level, F.saved_level);
    fid_init_level(F.saved_level);
    CvarSource src = cvar_source_push(CVAR_SRC_FIDELITY);
    for (int f = 0; f < FF_COUNT; f++) {
        Cvar *c = cvar_find(fid_feature(f)->cvar);
        if (c && F.saved_values[f][0]) cvar_set_cv(c, F.saved_values[f]);
    }
    cvar_source_pop(src);
    F.quiet = false;
    after_level_change(F.saved_level);
}

// A level for a measurement: its table values on every feature.
static void measure_level(int l) {
    F.quiet = true;
    cvar_set_int(F.level, l);
    F.quiet = false;
    fid_apply(l);
    after_level_change(l);
}

// ------------------------------------------------------------------ bench_fidelity (fixed flythrough)

// Devroom: the hall, the post-process test corner (SW), back over the firing line to the horde yard.
// x, y, z, yaw (deg, 0 = -z), pitch (deg).
static const float k_fly[][5] = {
    { 0, 1.8f, 20, 0, -3 },      { 0, 2.0f, 2, -30, -4 },  { -12, 1.8f, -14, -57, -5 }, { -22, 1.8f, -24, -90, -3 },
    { -18, 3.0f, -5, -180, -6 }, { -10, 4, 30, -200, -10 }, { 0, 6, 50, -180, -15 },    { 15, 3, 25, -90, -5 }, { 0, 1.8f, 20, -360, -3 },
};

static float catmull(float p0, float p1, float p2, float p3, float t) {
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
}

static bool fly_camera(RenderCamera *cam) {
    if (F.run != RUN_BENCH) return false;
    int n = ARRAY_COUNT(k_fly);
    float u = (float)F.frame / (float)DG_MAX(F.frames_per, 1) * (float)(n - 1);
    int i = DG_CLAMP((int)u, 0, n - 2);
    float t = fminf(u - (float)i, 1.0f);
    float v[5];
    for (int k = 0; k < 5; k++) v[k] = catmull(k_fly[i > 0 ? i - 1 : 0][k], k_fly[i][k], k_fly[i + 1][k], k_fly[i + 2 < n ? i + 2 : n - 1][k], t);
    cam->pos = v3(v[0], v[1], v[2]);
    cam->yaw = -v[3] * DG_DEG2RAD;
    cam->pitch = v[4] * DG_DEG2RAD;
    cam->roll = 0;
    return true;
}

static void bench_start_level(void) {
    measure_level(F.levels[F.step]);
    F.frame = 0;
    F.pass_count = 0;
    F.gpu_sum = F.cpu_sum = 0;
    F.measured = 0;
    memset(F.pass_ms, 0, sizeof(F.pass_ms));
}

static void cmd_bench(int argc, const char **argv) {
    if (F.run != RUN_NONE || !fid_loaded()) { log_warn("bench_fidelity: busy or no tables"); return; }
    const char *what = argc > 1 ? argv[1] : "all";
    F.level_count = 0;
    if (!strcmp(what, "all")) for (int l = 0; l < FID_LEVELS; l++) F.levels[F.level_count++] = l;
    else {
        int l = fid_level_parse(what);
        if (l < 0) { log_warn("bench_fidelity: unknown level '%s' (off, low, medium, high, modern, all)", what); return; }
        F.levels[F.level_count++] = l;
    }
    F.frames_per = argc > 2 ? DG_CLAMP(atoi(argv[2]), 20, 100000) : 300;
    F.quit_after = argc > 3 && !strcmp(argv[3], "quit");
    for (int l = 0; l < FID_LEVELS; l++) F.bench_result[l] = -1.0f;
    snapshot();
    F.run = RUN_BENCH;
    F.step = 0;
    bench_start_level();
    log_info("bench_fidelity: %d level(s), %d frames each over the devroom flythrough", F.level_count, F.frames_per);
}

static void bench_finish_level(void) {
    int l = F.levels[F.step];
    int n = DG_MAX(F.measured, 1);
    u32 rw = 0, rh = 0;
    render_render_size(&rw, &rh);
    double post = 0, all = 0;
    for (int k = 0; k < F.pass_count; k++) {
        all += F.pass_ms[k] / n;
        if (fid_pass_is_post(F.pass_names[k])) post += F.pass_ms[k] / n;
    }
    float gpu = (float)(F.gpu_sum / n), cpu = (float)(F.cpu_sum / n), budget = fid_level_budget_ms(l);
    RenderGraphReport rep;
    render_graph_report(&rep);
    log_info("bench_fidelity %s: %d frames, output %ux%u, render %ux%u, GPU frame %.3f ms (budget %.1f ms at 1080p: %s), CPU frame %.3f ms, frame graph %.3f ms "
             "(post %.3f ms, %d post passes), memory %.1f MB",
             fid_level_id(l), F.measured, (unsigned)render_width(), (unsigned)render_height(), rw, rh, gpu, budget, gpu <= budget ? "within" : "OVER", cpu, all,
             post, rep.post_passes, (double)render_gpu_memory_bytes() / (1024.0 * 1024.0));
    for (int k = 0; k < F.pass_count; k++)
        log_info("  %-18s %8.3f ms%s", F.pass_names[k], F.pass_ms[k] / n, fid_pass_is_post(F.pass_names[k]) ? "  post" : "");
    F.bench_result[l] = gpu;
}

static void bench_frame(void) {
    // Timings arrive two frames late: skip the first frames of every level.
    if (F.frame >= 10) {
        F.measured++;
        F.gpu_sum += render_gpu_frame_ms();
        F.cpu_sum += g_engine.frame_dt * 1000.0;
        int n = render_pass_count();
        for (int i = 0; i < n; i++) {
            const char *nm = render_pass_name(i);
            int k = 0;
            while (k < F.pass_count && strcmp(F.pass_names[k], nm)) k++;
            if (k == F.pass_count && F.pass_count < BENCH_MAX_PASSES) dg_strcpy(F.pass_names[F.pass_count++], 32, nm);
            if (k < BENCH_MAX_PASSES) F.pass_ms[k] += render_pass_ms(i);
        }
    }
    if (++F.frame < F.frames_per) return;
    bench_finish_level();
    if (++F.step < F.level_count) { bench_start_level(); return; }
    F.run = RUN_NONE;
    restore();
    log_info("bench_fidelity: done (GPU frame ms: off %.2f, low %.2f, medium %.2f, high %.2f, modern %.2f; -1 = not run)", F.bench_result[0], F.bench_result[1],
             F.bench_result[2], F.bench_result[3], F.bench_result[4]);
    if (F.quit_after) g_engine.quit = true;
}

// ------------------------------------------------------------------ graph / table checks

// Validates this frame's graph against the current level: no dead passes, no declared-but-unused
// resources, no undefined reads; Off schedules no post-processing pass. Returns failures.
static int check_graph(bool verbose) {
    RenderGraphReport r;
    render_graph_report(&r);
    int lvl = fid_level(), bad = 0;
    bool off = !fid_level_post(lvl);
    if (r.dead_passes) { bad++; for (int i = 0; i < r.dead_passes && i < 12; i++) log_warn("fidelity_check: dead pass %s", r.dead[i]); }
    if (r.unused_res) { bad++; for (int i = 0; i < r.unused_res && i < 12; i++) log_warn("fidelity_check: unused resource %s", r.unused[i]); }
    if (r.undefined_reads) { bad++; for (int i = 0; i < r.undefined_reads && i < 12; i++) log_warn("fidelity_check: transient %s read before any pass wrote it", r.undefined[i]); }
    if (off && r.post_passes) { bad++; for (int i = 0; i < r.post_passes && i < 32; i++) log_warn("fidelity_check: Off schedules post pass %s", r.post[i]); }
    if (verbose) {
        char list[2048] = "";
        for (int i = 0; i < r.passes && i < 96; i++) snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s%s", i ? " " : "", r.names[i], fid_pass_is_post(r.names[i]) ? "*" : "");
        log_info("fidelity_check %s: %d passes (%d post, * below): %s", fid_level_id(lvl), r.passes, r.post_passes, list);
        log_info("fidelity_check %s: %d dead, %d unused, %d undefined, pool %d images %.1f MB (%d stale %.1f MB), GPU memory %.1f MB -> %s", fid_level_id(lvl), r.dead_passes,
                 r.unused_res, r.undefined_reads, r.pool_images, (double)r.pool_bytes / 1048576.0, r.stale_images, (double)r.stale_bytes / 1048576.0,
                 (double)render_gpu_memory_bytes() / 1048576.0, bad ? "FAIL" : "ok");
    }
    return bad;
}

static void cmd_check(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    char rep[2048];
    int t = fid_check_table(fid_level(), rep, sizeof(rep));
    check_graph(true);
    log_info("fidelity_check %s: exposure: auto (histogram) EV %.2f, Off metered EV %.2f", fid_level_id(fid_level()), render_exposure_ev(), render_direct_exposure_ev());
    log_info("fidelity_check %s: cvars vs table: %d differ%s%s", fid_level_id(fid_level()), t, t ? ": " : "", t ? rep : "");
}

// fidelity_test [quit]: every level applied fresh (graph valid, Off without post passes, every cvar
// = the table), then 50 level switches and back: GPU memory must return to where it was.
static void cmd_test(int argc, const char **argv) {
    if (F.run != RUN_NONE || !fid_loaded()) { log_warn("fidelity_test: busy or no tables"); return; }
    snapshot();
    F.run = RUN_TEST;
    F.step = 0;
    F.frame = 0;
    F.fails = 0;
    F.quit_after = argc > 1 && !strcmp(argv[1], "quit");
    measure_level(FID_OFF);
    log_info("fidelity_test: start");
}

#define TEST_SWITCHES 50
static void test_frame(void) {
    F.frame++;
    if (F.step < FID_LEVELS) {
        // Level F.step: let the graph settle (render targets resized, histories), trim, check.
        if (F.frame == 4) render_graph_trim();
        if (F.frame < 8) return;
        int l = F.step;
        char rep[2048];
        int t = fid_check_table(l, rep, sizeof(rep));
        int g = check_graph(true);
        RenderGraphReport r;
        render_graph_report(&r);
        if (r.stale_images) { g++; log_warn("fidelity_test %s: %d pool images allocated but unscheduled after the trim", fid_level_id(l), r.stale_images); }
        if (t) log_warn("fidelity_test %s: cvars differ from the table: %s", fid_level_id(l), rep);
        log_info("fidelity_test %s: table %s, graph %s", fid_level_id(l), t ? "FAIL" : "ok", g ? "FAIL" : "ok");
        F.fails += (t ? 1 : 0) + (g ? 1 : 0);
        F.frame = 0;
        if (++F.step < FID_LEVELS) measure_level(F.step);
        else F.mem_before = render_gpu_memory_bytes();   // Modern, trimmed
        return;
    }
    int k = F.step - FID_LEVELS;
    if (k < TEST_SWITCHES) {
        if (F.frame < 2) return;
        measure_level((k * 3 + 1) % FID_LEVELS);   // every level, in a mixed order
        F.frame = 0;
        F.step++;
        return;
    }
    if (k == TEST_SWITCHES) {   // back to Modern, settle, trim, settle
        measure_level(FID_MODERN);
        F.frame = 0;
        F.step++;
        return;
    }
    if (F.frame == 4) render_graph_trim();
    if (F.frame < 10) return;
    u64 after = render_gpu_memory_bytes();
    double d = ((double)after - (double)F.mem_before) / 1048576.0;
    bool leak = d > 1.0;
    log_info("fidelity_test: %d level switches: GPU memory %.1f MB -> %.1f MB (%+.2f MB) -> %s", TEST_SWITCHES, (double)F.mem_before / 1048576.0, (double)after / 1048576.0, d,
             leak ? "LEAK" : "ok");
    F.fails += leak;
    F.run = RUN_NONE;
    restore();
    log_info("fidelity_test: %s (%d failure(s))", F.fails ? "FAIL" : "PASS", F.fails);
    if (F.quit_after) g_engine.quit = true;
}

static void cmd_status(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    int l = fid_level();
    static const char *const tn[4] = { "low", "medium", "high", "ultra" };
    log_info("fidelity: tables %s, era %s, post %s; quality: aa %s, post %s, shadows %s, textures %s, effects %s, foliage %s, view distance %s", fid_level_name(l),
             fid_era_id(fid_era()), fid_level_post(l) ? "on" : "OFF (direct output)", tn[game_quality_tier(FG_AA)], tn[game_quality_tier(FG_POST)],
             tn[game_quality_tier(FG_SHADOWS)], tn[game_quality_tier(FG_TEXTURES)], tn[game_quality_tier(FG_EFFECTS)], tn[game_quality_tier(FG_FOLIAGE)],
             tn[game_quality_tier(FG_VIEW_DISTANCE)]);
    for (int f = 0; f < FF_COUNT; f++) {
        FidBounds b = fid_bounds(l, fid_era(), f);
        const FidFeatureDef *d = fid_feature(f);
        log_info("  %-22s %-24s %-13s value %-6g default %-6g range %g .. %g", d->id, d->cvar,
                 b.avail == FID_UNAVAILABLE ? (b.era_lacks ? "era lacks" : "unavailable") : (b.avail == FID_DEFAULT_OFF ? "default off" : "default on"), fid_cvar_value(f), b.def,
                 b.lo, b.hi);
    }
}

// ------------------------------------------------------------------ init / frame

void game_fidelity_init(void) {
    memset(&F, 0, sizeof(F));
    const u32 A = CVAR_ARCHIVE;
    F.level = cvar_reg_int("r_fidelity", FID_MODERN, 0, FID_LEVELS - 1, CVAR_LOCKED,
                           "Engine Fidelity tables' level (dev tools only: bench_fidelity, fidelity_test, shadow_preset); the player's quality settings set the features");
    // The player's quality settings: 0 low, 1 medium, 2 high, 3 ultra (Settings > Graphics > Quality).
    F.quality[FG_AA] = cvar_reg_int("r_quality_aa", 3, 0, 3, A | CVAR_EDITABLE, "anti-aliasing quality 0 low .. 3 ultra: FXAA, SMAA, TAA, TAA and sharpening");
    F.quality[FG_POST] = cvar_reg_int("r_quality_post", 3, 0, 3, A | CVAR_EDITABLE,
                                      "post-processing quality 0 low .. 3 ultra: AO, contact shadows, reflections, GI, volumetrics, depth of field, bloom, motion blur, lens finish");
    F.quality[FG_SHADOWS] = cvar_reg_int("r_quality_shadows", 3, 0, 3, A | CVAR_EDITABLE, "shadow quality 0 low .. 3 ultra: sun / moon and lamp shadows (resolution, cascades, softness, casters)");
    F.quality[FG_TEXTURES] = cvar_reg_int("r_quality_textures", 3, 0, 3, A | CVAR_EDITABLE, "texture quality 0 low .. 3 ultra: resolution, anisotropic filtering, terrain shading");
    F.quality[FG_EFFECTS] = cvar_reg_int("r_quality_effects", 3, 0, 3, A | CVAR_EDITABLE, "effects quality 0 low .. 3 ultra: particles, lit particles, rain / snow, water, the scope picture");
    F.quality[FG_FOLIAGE] = cvar_reg_int("r_quality_foliage", 3, 0, 3, A | CVAR_EDITABLE, "foliage quality 0 low .. 3 ultra: the foliage wind layers");
    F.quality[FG_VIEW_DISTANCE] = cvar_reg_int("r_quality_view_distance", 3, 0, 3, A | CVAR_EDITABLE, "view distance quality 0 low .. 3 ultra: the shadow views' geometry detail");
    for (int g = 0; g < FG_COUNT; g++) F.quality[g]->on_change = on_quality;
    F.overlay = cvar_reg_bool("r_perf_overlay", false, A | CVAR_EDITABLE, "FPS / frame time overlay");
    cmd_register("bench_fidelity", cmd_bench, "bench_fidelity <off|low|medium|high|modern|all> [frames] [quit]: fixed flythrough, GPU ms per pass");
    cmd_register("fidelity_check", cmd_check, "validate this frame's graph and the cvars against the current Engine Fidelity level");
    cmd_register("fidelity_test", cmd_test, "fidelity_test [quit]: every level (graph, Off post passes, table), 50 switches (GPU memory)");
    cmd_register("fidelity_status", cmd_status, "print the Engine Fidelity level, era and every feature's bounds");
    char dir[512], err[256] = "";
    platform_data_path("assets/render/fidelity", dir, sizeof(dir));
    if (!fid_load_dir(dir, err, sizeof(err))) {
        log_error("fidelity: %s (no quality tables: the quality settings do nothing)", err);
        return;
    }
    fid_bind_cvars();   // no bounds: the player's quality settings are the only thing setting these
    for (int f = 0; f < FF_COUNT; f++)
        if (!cvar_find(fid_feature(f)->cvar)) log_warn("fidelity: feature %s has no cvar %s", fid_feature(f)->id, fid_feature(f)->cvar);
    F.ready = true;
    F.loading = true;   // until the saved settings are in (game_fidelity_settings_loaded)
    fid_set_tracking(false);
    F.level->on_change = on_level;
    fid_init_level(F.level->i);
    render_set_camera_override(fly_camera);
    log_info("fidelity: tables loaded (%d features x %d levels x %d eras)", FF_COUNT, FID_LEVELS, FID_ERAS);
}

// After the config (or none in --test runs) and before the command line / scripts.
void game_fidelity_settings_loaded(bool first_run) {
    (void)first_run;
    if (!F.ready) return;
    fid_init_level(F.level->i);
    F.loading = false;
    game_quality_apply();   // the player's saved quality settings, as they are
}

void game_fidelity_frame(void) {
    if (!F.ready) return;
    float ms = (float)g_engine.frame_dt * 1000.0f;
    F.hist[F.hist_pos] = ms;
    F.hist_pos = (F.hist_pos + 1) % HIST;
    if (F.trim_in > 0 && --F.trim_in == 0) render_graph_trim();
    switch (F.run) {
    case RUN_BENCH: bench_frame(); break;
    case RUN_TEST: test_frame(); break;
    default: break;
    }
}

// ------------------------------------------------------------------ overlays

void game_fidelity_draw(void) {
    if (!F.ready) return;
    float s = ui_scale();
    if (!F.overlay->i) return;
    int l = fid_level();
    float budget = fid_level_budget_ms(l), gpu = render_gpu_frame_ms(), cpu = (float)g_engine.frame_dt * 1000.0f;
    float x = 16 * s, y = 70 * s, w = 340 * s, gh = 60 * s;
    ui_rect(x - 8 * s, y - 6 * s, w + 16 * s, gh + 58 * s, UI_RGBA(0, 0, 0, 170));
    ui_textf(x, y, FONT_SMALL, UI_RGBA(255, 220, 140, 255), UI_ALIGN_LEFT, "%s  %.0f fps  CPU %.2f ms  GPU %.2f ms", fid_level_name(l), g_engine.fps_smoothed, cpu, gpu);
    u32 rw = 0, rh = 0;
    render_render_size(&rw, &rh);
    ui_textf(x, y + 16 * s, FONT_SMALL, UI_RGBA(200, 200, 200, 255), UI_ALIGN_LEFT, "budget %.1f ms (1080p)  render %ux%u  era %s", budget, rw, rh, fid_era_id(fid_era()));
    // Frame time history (CPU frame ms), the budget line, 33 ms full scale.
    float gy = y + 38 * s, scale = gh / 33.0f;
    ui_rect(x, gy, w, gh, UI_RGBA(255, 255, 255, 18));
    float bw = w / (float)HIST;
    for (int i = 0; i < HIST; i++) {
        float v = F.hist[(F.hist_pos + i) % HIST];
        float h = fminf(v * scale, gh);
        u32 c = v <= budget ? UI_RGBA(120, 210, 140, 255) : (v <= 16.7f ? UI_RGBA(240, 200, 90, 255) : UI_RGBA(240, 110, 90, 255));
        ui_rect(x + (float)i * bw, gy + gh - h, fmaxf(bw - 0.5f, 1.0f), h, c);
    }
    float by = gy + gh - fminf(budget * scale, gh);
    ui_rect(x, by, w, fmaxf(1.0f, s), UI_RGBA(255, 255, 255, 200));
}
