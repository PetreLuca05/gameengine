// Sky-driven shadows, game side (docs/SHADOWS.md): assets/render/shadows_sky.json (hot reload),
// the driver's state (src/render/shadow_sky.c) stepped once per frame from daynight_environment,
// the fidelity inputs (r_shadow_filter = softness model, r_moon_shadows, the 2010 era's fixed
// hardness), the reference presets (shadow_preset: time + weather for the dev room's shadow
// corner), the readout (r_shadow_debug: sun vs sky irradiance, contrast, source radius, mode, the
// predicted lit / shade ratio of the reference surface) and shadow_info.
#include "game_internal.h"
#include "render/shadow_sky.h"
#include "render/render.h"
#include "render/fidelity/fidelity.h"
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
    return "PCSS";
}

// The reference surface: 18 % grey, horizontal, AO 1 (lit_body.glsl's terms; cloud patches excluded).
static float ref_ratio(const ShadowSkyOut *o, float elev) {
    float ndl = fmaxf(sinf(elev), 0.0f);
    float lit = o->sky_e + o->key.intensity * ndl;
    float shd = o->sky_e + o->key.intensity * ndl * (1.0f - o->key.fade);
    return shd > 1e-9f ? lit / shd : 1.0f;
}

static void info_lines(char lines[][160], int *n) {
    const ShadowSkyOut *o = &SS.out;
    const ShadowSkyInput *in = &SS.in;
    float elev = o->key_is_moon ? in->moon_elev : in->sun_elev;
    int k = 0;
    snprintf(lines[k++], 160, "shadows: key %s %.1f deg | direct %.4f  sky %.4f  C %.2f | lit/shade (ground) %.2f", o->key_is_moon ? "moon" : "sun", elev * DG_RAD2DEG,
             o->key.intensity, o->sky_e, o->key.contrast, ref_ratio(o, elev));
    snprintf(lines[k++], 160, "source %.2f deg | %s  fade %.2f  cascade0 %.2f  pcf %.2f | veil tau %.2f (T %.2f)  cumulus tau %.1f", o->key.radius_deg, mode_name(o), o->key.fade,
             o->near_weight, o->pcf_radius, o->veil_tau, o->veil_t, o->cumulus_tau);
    snprintf(lines[k++], 160, "moon tile %s (direct %.4f C %.2f fade %.2f) | clouds drift %.1f m/s | EV %+.2f | ground %.2f %.2f %.2f",
             o->second_shadowed ? "on" : "off", o->second.intensity, o->second.contrast, o->second.fade, sqrtf(o->cloud_vel.x * o->cloud_vel.x + o->cloud_vel.y * o->cloud_vel.y),
             o->exposure_comp, o->ground_albedo.x, o->ground_albedo.y, o->ground_albedo.z);
    *n = k;
}

void shadow_sky_draw(void) {
    Cvar *dbg = find(&SS.debug, "r_shadow_debug");   // the renderer's (render.c)
    if (!dbg || !dbg->i || !SS.have || scene_hides_hud()) return;
    char lines[4][160];
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
    char lines[4][160];
    int n;
    info_lines(lines, &n);
    for (int i = 0; i < n; i++) log_info("%s", lines[i]);
}

// shadow_preset <name>: time + weather for the reference conditions (host; locks the weather).
typedef struct Preset { const char *name; const char *weather; const char *params; float elev; int moon; } Preset;
static const Preset k_presets[] = {
    { "clear_noon", "clear", NULL, 90, 0 },
    { "hazy", "clear", "fog_density 0.02", 90, 0 },
    { "light_cloud", "clear", "cloud_cover 0.72 cloud_density 0.35", 90, 0 },
    { "overcast", "overcast", NULL, 90, 0 },
    { "sunset", "clear", NULL, 5, 0 },
    { "night_moon", "clear", NULL, 0, 1 },
    { "fog", "fog", NULL, 90, 0 },
    { "rain", "rain", NULL, 90, 0 },
};

static void cmd_preset(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    int n = (int)(sizeof(k_presets) / sizeof(k_presets[0]));
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
    else daynight_preset_time(w, p->elev, p->moon != 0);
    ssky_state_reset(&SS.st);   // a jump, not a transition: snap the fades
    log_info("shadow_preset: %s", p->name);
}

void shadow_sky_register(void) {
    SS.moon_shadows = cvar_reg_bool("r_moon_shadows", true, CVAR_ARCHIVE, "moon shadows while the sun still owns the cascades (atlas tile 3; Engine Fidelity High / Modern)");
    cmd_register("shadows_sky_reload", cmd_reload, "reload assets/render/shadows_sky.json (also automatic on save)");
    cmd_register("shadow_info", cmd_info, "sky-driven shadows: key light, direct vs sky irradiance, contrast, source radius, mode, moon tile");
    cmd_register("shadow_preset", cmd_preset, "shadow_preset clear_noon|hazy|light_cloud|overcast|sunset|night_moon|fog|rain: time + weather for the shadow reference corner (host)");
}
