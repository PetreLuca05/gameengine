// Sky-driven shadows, game side (docs/SHADOWS.md): assets/render/shadows_sky.json (hot reload),
// the driver's state (src/render/shadow_sky.c) stepped once per frame from daynight_environment,
// the fidelity inputs (r_shadow_filter = softness model, r_moon_shadows, the 2010 era's fixed
// hardness), the reference presets (shadow_preset: time + weather for the dev room's shadow
// corner), the readout (r_shadow_debug: sun vs sky irradiance, contrast, source radius, mode, the
// predicted lit / shade ratio of the reference surface, the cascades, bias, filter and jitter state;
// r_shadow_debug 8 draws the fitting spheres and light boxes) and shadow_info; the shadow test
// tools: shadow_switch_stress (fidelity presets and cloud sweeps against a measured reference edge),
// shadow_orbit (a slow turn that shows swimming).
#include "game_internal.h"
#include "render/shadow_sky.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
#include "render/debug_draw.h"
#include "scene.h"
#include "ui/ui.h"
#include "ui/hud.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

static struct {
    ShadowSkyConfig cfg;
    ShadowSkyState st;
    ShadowSkyInput in;
    ShadowSkyOut out;
    bool loaded, have;
    i64 mtime;
    double next_poll;
    Cvar *moon_shadows, *debug, *filter, *era;
} SS;

static void load(void) {
    ShadowSkyConfig c = SS.cfg;
    if (!SS.loaded) ssky_config_default(&c);
    char p[512], err[256];
    platform_data_path(SSKY_PATH, p, sizeof(p));
    if (ssky_config_load(&c, p, err, sizeof(err))) SS.cfg = c;
    else log_warn("shadows_sky: %s (keeping the previous values)", err);
    if (!SS.loaded) SS.cfg = c;
    SS.loaded = true;
    SS.mtime = platform_file_mtime(p);
}

static void poll_reload(void) {
    if (!SS.loaded) { load(); return; }
    if (g_engine.real_time < SS.next_poll) return;
    SS.next_poll = g_engine.real_time + 1.0;
    char p[512];
    i64 t = platform_file_mtime(platform_data_path(SSKY_PATH, p, sizeof(p)));
    if (t && t != SS.mtime) {
        load();
        log_info("shadows_sky: %s changed, reloaded", p);
    }
}

static Cvar *find(Cvar **slot, const char *name) {
    if (!*slot) *slot = cvar_find(name);
    return *slot;
}

static void stress_frame(void);
static void orbit_frame(void);

void shadow_sky_frame(ShadowSkyInput *in, ShadowSkyOut *out, float tod_cloud_wind) {
    poll_reload();
    Cvar *f = find(&SS.filter, "r_shadow_filter"), *e = find(&SS.era, "r_era");
    in->softness = f ? DG_CLAMP(f->i, 0, 2) : 2;
    in->moon_shadows = SS.moon_shadows && SS.moon_shadows->i;
    in->fixed_hardness = e && e->i == FID_ERA_2010;
    // daynight.json's cloud wind is the speed at the layer already.
    if (in->wind_speed < 0.0f) in->wind_speed = tod_cloud_wind / fmaxf(SS.cfg.cloud_wind_aloft, 0.01f);
    ssky_update(&SS.st, &SS.cfg, in, out);
    SS.in = *in;
    SS.out = *out;
    SS.have = true;
    stress_frame();
    orbit_frame();
}

float shadow_sky_exposure(void) { return SS.have ? SS.out.exposure_comp : 0.0f; }

void shadow_sky_level_loaded(void) {
    ssky_state_reset(&SS.st);   // snap: no fade from the previous level's sky
    SS.have = false;
}

// ------------------------------------------------------------------ readout

static const char *mode_name(const ShadowSkyOut *o) {
    if (!o->key.cast) return "off (sky only)";
    if (o->near_weight < 0.999f) return o->near_weight > 0.0f ? "PCSS -> wide" : "wide";
    if (o->fixed_texels > 0.0f) return "fixed (2010)";
    if (o->pcss_weight >= 1.0f) return "PCSS";
    return o->pcss_weight > 0.0f ? "PCF <-> PCSS" : "PCF";
}

// The reference surface: 18 % grey, horizontal, AO 1 (lit_body.glsl's terms; cloud patches excluded).
static float ref_ratio(const ShadowSkyOut *o, float elev) {
    float ndl = fmaxf(sinf(elev), 0.0f);
    float lit = o->sky_e + o->key.intensity * ndl;
    float shd = o->sky_e + o->key.intensity * ndl * (1.0f - o->key.fade);
    return shd > 1e-9f ? lit / shd : 1.0f;
}

#define INFO_LINES 6
static void info_lines(char lines[][160], int *n) {
    const ShadowSkyOut *o = &SS.out;
    const ShadowSkyInput *in = &SS.in;
    float elev = o->key_is_moon ? in->moon_elev : in->sun_elev;
    RenderShadowInfo ri;
    render_shadow_info(&ri);
    int k = 0;
    snprintf(lines[k++], 160, "shadows: key %s %.1f deg | direct %.4f  sky %.4f  C %.2f | lit/shade (ground) %.2f", o->key_is_moon ? "moon" : "sun", elev * DG_RAD2DEG,
             o->key.intensity, o->sky_e, o->key.contrast, ref_ratio(o, elev));
    snprintf(lines[k++], 160, "source %.2f deg | %s  fade %.2f  cascade0 %.2f  pcss %.2f  max %.1f tx | veil tau %.2f (T %.2f)  cumulus tau %.1f", o->key.radius_deg, mode_name(o),
             o->key.fade, o->near_weight, o->pcss_weight, o->pcss_max_texels, o->veil_tau, o->veil_t, o->cumulus_tau);
    snprintf(lines[k++], 160, "moon tile %s (direct %.4f C %.2f fade %.2f) | clouds drift %.1f m/s | EV %+.2f | ground %.2f %.2f %.2f",
             o->second_shadowed ? "on" : "off", o->second.intensity, o->second.contrast, o->second.fade, sqrtf(o->cloud_vel.x * o->cloud_vel.x + o->cloud_vel.y * o->cloud_vel.y),
             o->exposure_comp, o->ground_albedo.x, o->ground_albedo.y, o->ground_albedo.z);
    char cs[3][48] = { "", "", "" };
    for (int c = 0; c < ri.cascades && c < 3; c++)
        snprintf(cs[c], sizeof(cs[c]), " c%d %.1f m r %.1f %.1f cm", c, ri.split[c], ri.radius[c], ri.texel_m[c] * 100.0f);
    snprintf(lines[k++], 160, "cascades %d x %u | distance %.0f m |%s%s%s", ri.cascades, ri.tile, ri.distance, cs[0], cs[1], cs[2]);
    snprintf(lines[k++], 160, "bias c0 const %.1f mm normal %.1f mm (low sun x%.2f) | kernel %.1f .. %.1f tx | jitter %s (kernel %s)", ri.bias_m[0] * 1000.0f,
             ri.nbias_m[0] * 1000.0f, ri.low_sun, o->min_texels, o->pcss_max_texels, ri.taa ? "TAA" : "off", ri.taa ? "rotates" : "fixed");
    snprintf(lines[k++], 160, "viewmodel tile %s %u (%.2f mm, r %.2f m, pcss %.2f) | unrendered samples %u", ri.vm_active ? "on" : "off", ri.vm_tile, ri.vm_texel_m * 1000.0f,
             ri.vm_radius, ri.vm_pcss, ri.unrendered);
    *n = k;
}

// r_shadow_debug 8: the fitting spheres (red / green / blue, the moon grey) and each tile's light box
// (its square towards the light), the viewmodel tile in magenta.
static void draw_fit(void) {
    RenderShadowInfo ri;
    render_shadow_info(&ri);
    static const u32 col[4] = { 0xFF3333FFu, 0xFF33FF33u, 0xFFFF5533u, 0xFF999999u };
    vec3 L = ri.light_dir;
    vec3 up = fabsf(L.y) > 0.99f ? v3(0, 0, 1) : v3(0, 1, 0);
    vec3 ax = v3_norm(v3_cross(up, L)), ay = v3_cross(L, ax);
    for (int c = 0; c < 4; c++) {
        if (!ri.active[c] || (c < 3 && c >= ri.cascades)) continue;
        float r = c < 3 ? ri.radius[c] : ri.moon_radius;
        vec3 o = ri.center[c];
        dbg_sphere(o, r, col[c], 0.0f);
        // the tile's square, and its edges towards the light
        vec3 q[4] = { v3_add(o, v3_add(v3_scale(ax, r), v3_scale(ay, r))), v3_add(o, v3_sub(v3_scale(ax, r), v3_scale(ay, r))),
                      v3_sub(o, v3_add(v3_scale(ax, r), v3_scale(ay, r))), v3_sub(o, v3_sub(v3_scale(ax, r), v3_scale(ay, r))) };
        for (int i = 0; i < 4; i++) {
            dbg_line(q[i], q[(i + 1) & 3], col[c], 0.0f, true);
            dbg_line(q[i], v3_madd(q[i], L, 60.0f), col[c], 0.0f, true);
        }
    }
    if (ri.vm_active) dbg_sphere(ri.vm_center, ri.vm_radius, 0xFFFF33FFu, 0.0f);
}

void shadow_sky_draw(void) {
    Cvar *dbg = find(&SS.debug, "r_shadow_debug");   // the renderer's (render.c)
    if (!dbg || !dbg->i || !SS.have || scene_hides_hud()) return;
    if (dbg->i == 8) draw_fit();
    char lines[INFO_LINES][160];
    int n;
    info_lines(lines, &n);
    float s = ui_scale(), y = ui_height() - (18.0f * n + 70.0f) * s;
    for (int i = 0; i < n; i++) ui_text_shadow(12 * s, y + 18.0f * s * i, FONT_SMALL, UI_RGBA(235, 235, 220, 235), UI_ALIGN_LEFT, lines[i]);
}

// ------------------------------------------------------------------ commands

static void cmd_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load();
    log_info("shadows_sky: reloaded %s", SSKY_PATH);
}

static void cmd_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!SS.have) { log_info("shadow_info: no time-of-day sky on this level"); return; }
    char lines[INFO_LINES][160];
    int n;
    info_lines(lines, &n);
    for (int i = 0; i < n; i++) log_info("%s", lines[i]);
}

// shadow_preset <name>: time + weather for the reference conditions (host; locks the weather).
// elev: 90 noon, > 0 the evening sun at that elevation, < 0 the morning sun at -elev.
typedef struct Preset { const char *name; const char *weather; const char *params; float elev; int moon; } Preset;
static const Preset k_presets[] = {
    { "clear_noon", "clear", NULL, 90, 0 },
    { "sunrise", "clear", NULL, -5, 0 },
    { "hazy", "clear", "fog_density 0.02", 90, 0 },
    { "light_cloud", "clear", "cloud_cover 0.72 cloud_density 0.35", 90, 0 },
    { "overcast", "overcast", NULL, 90, 0 },
    { "sunset", "clear", NULL, 5, 0 },
    { "night_moon", "clear", NULL, 0, 1 },
    { "fog", "fog", NULL, 90, 0 },
    { "rain", "rain", NULL, 90, 0 },
};

// The shadow test corner's other switches (docs/SHADOWS.md): "noon" / "night" aliases, "cover_<0..1>"
// (cloud cover over 4 s, the driver's slow path), "fid_<level>" (Engine Fidelity), "stress", "orbit".
static void cmd_stress(int argc, const char **argv);
static void cmd_orbit(int argc, const char **argv);
static bool preset_action(const char *a) {
    char line[96];
    if (!strcmp(a, "noon")) { cmd_execute_line("shadow_preset clear_noon"); return true; }
    if (!strcmp(a, "night")) { cmd_execute_line("shadow_preset night_moon"); return true; }
    if (!strncmp(a, "cover_", 6)) {
        float cv = clampf((float)atof(a + 6), 0.0f, 1.0f);
        cmd_execute_line("weather_lock 1");
        snprintf(line, sizeof(line), "weather_params cloud_cover %.2f cloud_density %.2f time 4", cv, 0.3f + 0.6f * cv);
        cmd_execute_line(line);
        log_info("shadow_preset: cloud cover -> %.2f over 4 s", cv);
        return true;
    }
    if (!strncmp(a, "fid_", 4)) {
        for (int l = 0; l < FID_LEVELS; l++)
            if (!strcmp(a + 4, fid_level_id(l))) { game_fidelity_set(l); log_info("shadow_preset: Engine Fidelity %s", fid_level_id(l)); return true; }
        return false;
    }
    if (!strcmp(a, "stress")) { cmd_stress(0, NULL); return true; }
    if (!strcmp(a, "orbit")) { cmd_orbit(0, NULL); return true; }
    return false;
}

static void cmd_preset(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    int n = (int)(sizeof(k_presets) / sizeof(k_presets[0]));
    if (argc >= 2 && preset_action(argv[1])) return;
    if (argc < 2) {
        char b[256] = "";
        size_t len = 0;
        for (int i = 0; i < n && len < sizeof(b); i++) len += (size_t)snprintf(b + len, sizeof(b) - len, "%s%s", i ? "|" : "", k_presets[i].name);
        log_info("usage: shadow_preset <%s>", b);
        return;
    }
    if (!w || !w->authority) { log_info("shadow_preset: host only"); return; }
    const Preset *p = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(argv[1], k_presets[i].name)) p = &k_presets[i];
    if (!p) { log_info("shadow_preset: unknown '%s'", argv[1]); return; }
    char line[160];
    snprintf(line, sizeof(line), "weather_set %s 0", p->weather);
    cmd_execute_line(line);
    cmd_execute_line("weather_lock 1");
    if (p->params) {
        snprintf(line, sizeof(line), "weather_params %s time 0", p->params);
        cmd_execute_line(line);
    }
    if (p->elev >= 89.0f) cmd_execute_line("time_set 12:30");
    else if (p->elev < 0.0f) daynight_preset_morning(w, -p->elev);
    else daynight_preset_time(w, p->elev, p->moon != 0);
    ssky_state_reset(&SS.st);   // a jump, not a transition: snap the fades
    log_info("shadow_preset: %s", p->name);
}

// ------------------------------------------------------------------ shadow_switch_stress

// The reference edge measured every frame: a blocker 1 m above flat ground in cascade 0, as the
// shader computes it: penumbra half width (m), the edge offset the bias adds (m), the cast fade and
// the shaded luminance ratio of the reference surface.
typedef struct EdgeSample { float pen_m, offset_m, fade, ratio, texel_m; } EdgeSample;

static EdgeSample edge_sample(void) {
    const ShadowSkyOut *o = &SS.out;
    RenderShadowInfo ri;
    render_shadow_info(&ri);
    float elev = o->key_is_moon ? SS.in.moon_elev : SS.in.sun_elev;
    float t0 = fmaxf(ri.texel_m[0], 1e-5f);
    float tan_r = tanf(o->key.radius_deg * DG_DEG2RAD);
    float d = o->pcf_ref_m + (1.0f - o->pcf_ref_m) * o->pcss_weight;
    float pen = o->fixed_texels > 0.0f ? o->fixed_texels * t0 : d * tan_r;
    EdgeSample e;
    e.pen_m = clampf(pen / t0, o->min_texels, fmaxf(o->pcss_max_texels, o->min_texels)) * t0;
    e.offset_m = (ri.bias_m[0] + ri.nbias_m[0] * cosf(elev)) / fmaxf(tanf(fmaxf(elev, 0.02f)), 0.05f);
    e.fade = o->key.fade;
    e.ratio = ref_ratio(o, elev);
    e.texel_m = t0;
    return e;
}

static const int k_stress_cycle[] = { 0, 1, 2, 3, 4, 3, 2, 1, 0, 4 };
#define STRESS_CYCLE ((int)(sizeof(k_stress_cycle) / sizeof(k_stress_cycle[0])))

static struct {
    bool on, quit;
    int phase, step, level0, frames, fails, toggles, max_toggles;
    float t, hold, sweep;
    bool prev_cast, have_prev;
    EdgeSample prev;
    float worst_pen, worst_off, worst_fade, worst_ratio;
    unsigned unrendered0;
} ST;

static void stress_set_cover(float cv, float sec) {
    char line[96];
    snprintf(line, sizeof(line), "weather_params cloud_cover %.2f cloud_density %.2f time %.2f", cv, 0.3f + 0.6f * cv, sec);
    cmd_execute_line(line);
}

static void stress_measure(void) {
    EdgeSample e = edge_sample();
    if (ST.have_prev) {
        // A resolution change moves the clamped widths by its texel ratio; one texel of slack covers it.
        float tx = fmaxf(e.texel_m, ST.prev.texel_m);
        float dp = fabsf(e.pen_m - ST.prev.pen_m) - fmaxf(0.15f * ST.prev.pen_m, tx);
        float doff = fabsf(e.offset_m - ST.prev.offset_m) - fmaxf(0.25f * ST.prev.offset_m, 1.5f * tx);
        float df = fabsf(e.fade - ST.prev.fade) - 0.02f;
        float dr = fabsf(e.ratio - ST.prev.ratio) / fmaxf(ST.prev.ratio, 1.0f) - 0.02f;
        ST.worst_pen = fmaxf(ST.worst_pen, dp);
        ST.worst_off = fmaxf(ST.worst_off, doff);
        ST.worst_fade = fmaxf(ST.worst_fade, df);
        ST.worst_ratio = fmaxf(ST.worst_ratio, dr);
        if (dp > 0 || doff > 0 || df > 0 || dr > 0) {
            if (ST.fails++ < 8)
                log_warn("shadow_switch_stress: frame %d jump: pen %.4f -> %.4f m, offset %.4f -> %.4f m, fade %.3f -> %.3f, ratio %.3f -> %.3f", ST.frames, ST.prev.pen_m, e.pen_m,
                         ST.prev.offset_m, e.offset_m, ST.prev.fade, e.fade, ST.prev.ratio, e.ratio);
        }
        if (SS.out.key.cast != ST.prev_cast) ST.toggles++;
    }
    ST.prev = e;
    ST.prev_cast = SS.out.key.cast;
    ST.have_prev = true;
    ST.frames++;
}

static void stress_finish(void) {
    RenderShadowInfo ri;
    render_shadow_info(&ri);
    unsigned unr = ri.unrendered - ST.unrendered0;
    bool ok = ST.fails == 0 && unr == 0 && ST.max_toggles <= 1;
    log_info("shadow_switch_stress: %d frames, %d jumps (worst over threshold: pen %+.4f m, offset %+.4f m, fade %+.3f, ratio %+.3f), cutoff toggles per sweep <= %d, unrendered %u -> %s",
             ST.frames, ST.fails, ST.worst_pen, ST.worst_off, ST.worst_fade, ST.worst_ratio, ST.max_toggles, unr, ok ? "PASS" : "FAIL");
    game_fidelity_set(ST.level0);
    stress_set_cover(0.3f, 2.0f);
    ST.on = false;
    if (ST.quit) g_engine.quit = true;
}

// Phase 0: every preset in turn (and back) at a fixed cover; phase 1: on each preset, cover
// 0 -> 1 -> 0 (clear -> hazy -> overcast cutoff -> back), the weather's own smooth transition.
static void stress_frame(void) {
    if (!ST.on) return;
    float dt = (float)g_engine.frame_dt;
    ST.t += dt;
    stress_measure();
    if (ST.phase == 0) {
        if (ST.t < ST.hold) return;
        ST.t = 0;
        if (++ST.step < STRESS_CYCLE) { game_fidelity_set(k_stress_cycle[ST.step]); return; }
        ST.phase = 1;
        ST.step = 0;
        game_fidelity_set(0);
        stress_set_cover(1.0f, ST.sweep);
        ST.toggles = 0;
        return;
    }
    // phase 1: two half sweeps per preset; each half may toggle the cutoff at most once
    if (ST.t < ST.sweep + 1.0f) return;
    ST.t = 0;
    ST.max_toggles = DG_MAX(ST.max_toggles, ST.toggles);
    ST.toggles = 0;
    ST.step++;
    if (ST.step >= 2 * FID_LEVELS) { stress_finish(); return; }
    if (ST.step & 1) stress_set_cover(0.0f, ST.sweep);
    else {
        game_fidelity_set(ST.step / 2);
        stress_set_cover(1.0f, ST.sweep);
    }
}

// shadow_switch_stress [sweep seconds] [quit]: host, a level with the time-of-day sky.
static void cmd_stress(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority || !SS.have) { log_info("shadow_switch_stress: host on a level with the time-of-day sky"); return; }
    memset(&ST, 0, sizeof(ST));
    ST.on = true;
    ST.sweep = argc > 1 ? fmaxf((float)atof(argv[1]), 2.0f) : 8.0f;
    ST.hold = 1.0f;
    ST.quit = argc > 2 && !strcmp(argv[2], "quit");
    Cvar *lv = cvar_find("r_fidelity");
    ST.level0 = lv ? lv->i : FID_MODERN;
    RenderShadowInfo ri;
    render_shadow_info(&ri);
    ST.unrendered0 = ri.unrendered;
    cmd_execute_line("weather_lock 1");
    stress_set_cover(0.3f, 0.0f);
    game_fidelity_set(k_stress_cycle[0]);
    log_info("shadow_switch_stress: %d presets x %.1f s, then 0 -> 1 -> 0 cloud cover sweeps of %.1f s on each", STRESS_CYCLE, ST.hold, ST.sweep);
}

// shadow_orbit [deg/s | stop]: turn the view slowly (texel snapping: edges must not swim).
static struct { bool on; float rate; } OR;
static void orbit_frame(void) {
    if (!OR.on || !G) return;
    G->view.yaw += OR.rate * DG_DEG2RAD * (float)g_engine.frame_dt;
}
static void cmd_orbit(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "stop")) { OR.on = false; return; }
    OR.on = !(OR.on && argc <= 1);
    OR.rate = argc > 1 ? (float)atof(argv[1]) : 6.0f;
    log_info("shadow_orbit: %s (%.1f deg/s)", OR.on ? "on" : "off", OR.rate);
}

void shadow_sky_register(void) {
    SS.moon_shadows = cvar_reg_bool("r_moon_shadows", true, CVAR_ARCHIVE, "moon shadows while the sun still owns the cascades (atlas tile 3; Engine Fidelity High / Modern)");
    cmd_register("shadows_sky_reload", cmd_reload, "reload assets/render/shadows_sky.json (also automatic on save)");
    cmd_register("shadow_info", cmd_info, "sky-driven shadows: key light, direct vs sky irradiance, contrast, source radius, mode, moon tile");
    cmd_register("shadow_preset", cmd_preset,
                 "shadow_preset clear_noon|sunrise|hazy|light_cloud|overcast|sunset|night_moon|fog|rain (time + weather), noon|night, cover_<0..1>, fid_<level>, stress, orbit: the shadow test corner (host)");
    cmd_register("shadow_switch_stress", cmd_stress,
                 "shadow_switch_stress [sweep s] [quit]: cycle the Engine Fidelity presets, then sweep cloud cover 0 -> 1 -> 0 on each; a reference shadow edge is measured every frame (host)");
    cmd_register("shadow_orbit", cmd_orbit, "shadow_orbit [deg/s | stop]: turn the view slowly to show shadow swimming (default 6 deg/s)");
}
