// Wind field, game side (docs/FOLIAGE_WIND.md): one WindField per machine, built every frame from the
// replicated weather wind and the interpolated server tick (no network traffic of its own), its render
// tile handed to the renderer (foliage WPO), wind_game_at for everything else (particles, audio, lens
// droplets, weather_at), local sources (vehicle wakes, blasts), the wind_debug view and the tuning
// panel (wind_panel: wind override, gust trigger, per-layer solo, channel / bend views, the tile as an
// overlay, slow motion, per-foliage-type parameters).
#include "game_internal.h"
#include "world/wind.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/engine.h"
#include "nuklear_config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    WindField f;
    bool have;
    double time_offset;          // slow motion: wind time = tick time + offset
    float wind_time;             // s: this frame's (the foliage animation time)
    vec3 cam;                    // this frame's camera
    Cvar *debug, *timescale, *dir, *speed, *gust, *view, *solo;
    bool panel, overlay;
    int sel;                     // tuning panel: selected foliage type
    RenderFoliageWind edit;
    char edit_type[48];
} WG;

static float clampf_(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

const WindField *wind_game_field(void) { return WG.have ? &WG.f : NULL; }

vec3 wind_game_at(vec3 pos) {
    if (!WG.have) return v3(0, 0, 0);
    float p[3] = { pos.x, pos.y, pos.z }, o[3];
    wind_at(&WG.f, p, o);
    return v3(o[0], o[1], o[2]);
}

void wind_game_blast(vec3 pos, float radius, float strength) {
    float p[3] = { pos.x, pos.y, pos.z };
    wind_add_blast(&WG.f, p, radius, strength);
}

void wind_game_wake(vec3 pos, vec3 vel, float radius) {
    float p[3] = { pos.x, pos.y, pos.z }, v[3] = { vel.x, vel.y, vel.z };
    wind_add_wake(&WG.f, p, v, radius);
}

float wind_game_leaf_rate(vec3 pos) {
    // leaves torn off per second per tree: nothing below 6 m/s, ~quadratic above (gusts dominate)
    vec3 w = wind_game_at(pos);
    float s = sqrtf(w.x * w.x + w.z * w.z);
    return s < 6.0f ? 0.0f : 0.02f * (s - 6.0f) * (s - 6.0f);
}

// Every frame (weather_frame), weather active or not: base = the weather's wind (or the wind_* overrides,
// or a light breeze when the level has no weather).
void wind_game_frame(const WeatherParams *cur, double tick_time, vec3 cam, float dt, RenderWeather *rw) {
    WindBase b = { 60.0f, 3.0f, 0.3f };
    if (cur) b = (WindBase){ cur->v[WP_WIND_DIR], cur->v[WP_WIND], cur->v[WP_GUST] };
    if (WG.dir->f >= 0) b.dir_deg = WG.dir->f;
    if (WG.speed->f >= 0) b.speed = WG.speed->f;
    if (WG.gust->f >= 0) b.gust = WG.gust->f;
    float ts = WG.timescale->f;
    WG.time_offset += (double)dt * (ts - 1.0);
    if (!WG.have) wind_init(&WG.f, 0x57494E44u);   // 'WIND': every machine the same field
    wind_set(&WG.f, &b, tick_time + WG.time_offset);
    wind_sources_step(&WG.f, dt * ts);
    float c[3] = { cam.x, cam.y, cam.z };
    wind_tile_update(&WG.f, c, dt * ts);
    WG.cam = cam;
    WG.have = true;
    WG.wind_time = (float)fmod(WG.f.time, 3600.0);
    rw->wind = wind_game_at(cam);
    rw->time = WG.wind_time;
    rw->wind_texels = WG.f.texels;
    rw->wind_ox = WG.f.ox;
    rw->wind_oz = WG.f.oz;
    rw->wind_texel = WIND_TEXEL;
    rw->wind_global = v4(WG.f.dir[0] * WG.f.base.speed, WG.f.dir[1] * WG.f.base.speed, WG.f.global_bend[0], WG.f.global_bend[1]);
    rw->wind_view = WG.view->i;
    rw->wind_solo = WG.solo->i;
}

// ------------------------------------------------------------------ wind_debug

// Bend of a foliage instance as the shader computes it (main layer, ignoring the sway): degrees.
static float instance_bend_deg(const RenderFoliageWind *p, vec3 pos, float *dir_x, float *dir_z) {
    float s4[4];
    wind_tile_sample(&WG.f, pos.x, pos.z, s4);
    float mix = clampf_(p->settle_time / WIND_SETTLE_PERIOD, 0, 1);
    float wx = s4[0] + (s4[2] - s4[0]) * mix, wz = s4[1] + (s4[3] - s4[1]) * mix, s = sqrtf(wx * wx + wz * wz);
    *dir_x = s > 1e-4f ? wx / s : 1.0f;
    *dir_z = s > 1e-4f ? wz / s : 0.0f;
    float r = powf(clampf_(s / fmaxf(p->full_wind, 0.1f), 0, 1), fmaxf(p->response, 0.05f));
    float b = fminf(p->trunk_stiffness * r, tanf(p->max_bend_deg * DG_DEG2RAD));
    return atanf(b) * DG_RAD2DEG;
}

void wind_game_draw_debug(vec3 cam) {
    if (!WG.have || !WG.debug->i) return;
    // the field: arrows on an 8 m grid around the camera (length = 0.25 s of travel), colour = gust factor
    float y = cam.y - 1.5f;
    for (int j = -6; j <= 6; j++)
        for (int i = -6; i <= 6; i++) {
            vec3 a = v3(floorf(cam.x / 8.0f) * 8.0f + (float)i * 8.0f, y, floorf(cam.z / 8.0f) * 8.0f + (float)j * 8.0f);
            vec3 w = wind_game_at(a);
            float p[3] = { a.x, a.y, a.z }, g = wind_gust_at(&WG.f, p);
            u32 col = g > 1.25f ? DBG_RED : g < 0.8f ? DBG_BLUE : DBG_GREEN;
            dbg_arrow(a, v3_add(a, v3_scale(w, 0.25f)), col, 0);
        }
    // the next gust front upwind: a line across the wind
    float c[3] = { cam.x, y, cam.z }, fac = 1;
    float d = wind_gust_front(&WG.f, c, &fac);
    if (d > 0) {
        vec3 dir = v3(WG.f.dir[0], 0, WG.f.dir[1]), across = v3(-WG.f.dir[1], 0, WG.f.dir[0]);
        vec3 m = v3_sub(v3(cam.x, y + 0.5f, cam.z), v3_scale(dir, d));
        dbg_line(v3_madd(m, across, -40.0f), v3_madd(m, across, 40.0f), DBG_ORANGE, 0, false);
        char t[64];
        snprintf(t, sizeof(t), "gust front x%.2f, %.0f m, %.1f s", fac, d, d / fmaxf(WG.f.base.speed, 0.1f));
        dbg_text(v3_add(m, v3(0, 1.0f, 0)), t, DBG_ORANGE, 0.35f, 0);
    }
    // per-instance bend: the leaning stem of every foliage instance within 30 m
    static RenderFoliageHit hits[96];
    int n = render_foliage_query(cam, 30.0f, hits, 96);
    for (int i = 0; i < n; i++) {
        RenderFoliageWind p;
        if (!render_foliage_wind_get(hits[i].type, &p)) continue;
        float dx, dz, deg = instance_bend_deg(&p, hits[i].pos, &dx, &dz), a = deg * DG_DEG2RAD;
        float h = p.height * hits[i].scale;
        vec3 top = v3_add(hits[i].pos, v3(dx * sinf(a) * h, cosf(a) * h, dz * sinf(a) * h));
        dbg_line(hits[i].pos, top, deg > p.max_bend_deg * 0.8f ? DBG_RED : DBG_YELLOW, 0, false);
        if (hits[i].dist < 12.0f) {
            char t[32];
            snprintf(t, sizeof(t), "%.1f deg", deg);
            dbg_text(top, t, DBG_YELLOW, 0.25f, 0);
        }
    }
}

// ------------------------------------------------------------------ tuning panel

static void slider(struct nk_context *ctx, const char *label, float lo, float *v, float hi, float step, bool *changed) {
    char b[64];
    snprintf(b, sizeof(b), "%s %.3g", label, *v);
    nk_label(ctx, b, NK_TEXT_LEFT);
    float before = *v;
    nk_slider_float(ctx, lo, v, hi, step);
    if (*v != before) *changed = true;
}

void wind_game_draw_panel(void) {
    if (!WG.panel) return;
    struct nk_context *ctx = ui_nk();
    float s = ui_scale();
    if (nk_begin(ctx, "WIND", nk_rect(20 * s, 80 * s, 440 * s, 720 * s), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_CLOSABLE)) {
        char b[160];
        vec3 cam = WG.cam;
        vec3 w = wind_game_at(cam);
        float c[3] = { cam.x, cam.y, cam.z }, fac = 1, d = WG.have ? wind_gust_front(&WG.f, c, &fac) : 0;
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        snprintf(b, sizeof(b), "at camera %.1f m/s (gust x%.2f), base %.1f m/s towards %.0f deg, gust %.2f", sqrtf(w.x * w.x + w.z * w.z),
                 WG.have ? wind_gust_at(&WG.f, c) : 1.0f, WG.f.base.speed, WG.f.base.dir_deg, WG.f.base.gust);
        nk_label(ctx, b, NK_TEXT_LEFT);
        snprintf(b, sizeof(b), "next gust front %.0f m upwind (x%.2f)", d, fac);
        nk_label(ctx, b, NK_TEXT_LEFT);
        // override (-1 = the weather's)
        nk_layout_row_dynamic(ctx, 22 * s, 2);
        bool ovr = WG.speed->f >= 0;
        nk_bool o = ovr;
        nk_checkbox_label(ctx, "override the weather", &o);
        if ((bool)o != ovr) {
            cvar_set_float(WG.dir, o ? WG.f.base.dir_deg : -1.0f);
            cvar_set_float(WG.speed, o ? WG.f.base.speed : -1.0f);
            cvar_set_float(WG.gust, o ? WG.f.base.gust : -1.0f);
        }
        if (nk_button_label(ctx, "trigger a gust")) {
            float p[3] = { cam.x - WG.f.dir[0] * 120.0f, cam.y, cam.z - WG.f.dir[1] * 120.0f };
            wind_add_gust(&WG.f, p, fmaxf(12.0f, WG.f.base.speed * 1.5f), 30.0f, 260.0f);
        }
        nk_layout_row_dynamic(ctx, 18 * s, 1);
        if (WG.speed->f >= 0) {
            float dv = WG.dir->f, sv = WG.speed->f, gv = WG.gust->f;
            bool ch = false;
            slider(ctx, "direction (deg)", 0, &dv, 360, 1, &ch);
            slider(ctx, "speed (m/s)", 0, &sv, 35, 0.1f, &ch);
            slider(ctx, "gust", 0, &gv, 1, 0.01f, &ch);
            if (ch) { cvar_set_float(WG.dir, dv); cvar_set_float(WG.speed, sv); cvar_set_float(WG.gust, gv); }
        }
        float tsv = WG.timescale->f;
        bool tch = false;
        slider(ctx, "time scale (slow motion)", 0.05f, &tsv, 2.0f, 0.01f, &tch);
        if (tch) cvar_set_float(WG.timescale, tsv);
        // layers / views
        nk_layout_row_dynamic(ctx, 20 * s, 4);
        static const char *solo[4] = { "all", "main", "detail", "flutter" };
        for (int i = 0; i < 4; i++) if (nk_option_label(ctx, solo[i], WG.solo->i == i)) cvar_set_int(WG.solo, i);
        nk_layout_row_dynamic(ctx, 20 * s, 3);
        static const char *views[3] = { "shaded", "channels", "bend" };
        for (int i = 0; i < 3; i++) if (nk_option_label(ctx, views[i], WG.view->i == i)) cvar_set_int(WG.view, i);
        nk_layout_row_dynamic(ctx, 20 * s, 2);
        nk_bool arrows = WG.debug->i != 0, ov = WG.overlay;
        nk_checkbox_label(ctx, "field arrows (wind_debug)", &arrows);
        nk_checkbox_label(ctx, "tile overlay", &ov);
        if ((arrows != 0) != (WG.debug->i != 0)) cvar_set_int(WG.debug, arrows ? 1 : 0);
        WG.overlay = ov != 0;
        // per-type parameters
        int nt = render_foliage_type_count();
        if (nt > 0) {
            WG.sel = DG_CLAMP(WG.sel, 0, nt - 1);
            nk_layout_row_dynamic(ctx, 22 * s, 1);
            const char *items[32];
            int ni = DG_MIN(nt, 32);
            for (int i = 0; i < ni; i++) items[i] = render_foliage_type_name(i);
            WG.sel = nk_combo(ctx, items, ni, WG.sel, (int)(20 * s), nk_vec2(400 * s, 300 * s));
            const char *type = render_foliage_type_name(WG.sel);
            if (type && strcmp(type, WG.edit_type)) {
                dg_strcpy(WG.edit_type, sizeof(WG.edit_type), type);
                if (!render_foliage_wind_get(type, &WG.edit)) memset(&WG.edit, 0, sizeof(WG.edit));
            }
            RenderFoliageWind *e = &WG.edit;
            bool ch = false;
            nk_layout_row_dynamic(ctx, 18 * s, 1);
            slider(ctx, "trunk stiffness (tan lean)", 0, &e->trunk_stiffness, 1.0f, 0.005f, &ch);
            slider(ctx, "branch stiffness (m)", 0, &e->branch_stiffness, 1.0f, 0.005f, &ch);
            slider(ctx, "leaf stiffness (m)", 0, &e->leaf_stiffness, 0.3f, 0.002f, &ch);
            slider(ctx, "full wind (m/s)", 1, &e->full_wind, 40, 0.5f, &ch);
            slider(ctx, "main Hz", 0.02f, &e->main_freq, 1.0f, 0.01f, &ch);
            slider(ctx, "detail Hz", 0.1f, &e->detail_freq, 4.0f, 0.05f, &ch);
            slider(ctx, "flutter Hz", 0.5f, &e->flutter_freq, 15.0f, 0.1f, &ch);
            slider(ctx, "max bend (deg)", 0, &e->max_bend_deg, 60, 0.5f, &ch);
            slider(ctx, "settle time (s)", 0, &e->settle_time, WIND_SETTLE_PERIOD, 0.05f, &ch);
            slider(ctx, "response exponent", 0.2f, &e->response, 3.0f, 0.05f, &ch);
            if (ch && type) render_foliage_wind_set(type, e);
        }
        // the tile: speed as brightness, the settled bend's direction as hue (32 x 32, camera at the centre)
        if (WG.overlay && WG.have) {
            nk_layout_row_dynamic(ctx, 260 * s, 1);
            struct nk_rect r;
            if (nk_widget(&r, ctx)) {
                struct nk_command_buffer *cb = nk_window_get_canvas(ctx);
                float side = fminf(r.w, r.h), cell = side / 32.0f;
                for (int j = 0; j < 32; j++)
                    for (int i = 0; i < 32; i++) {
                        float x = ((float)WG.f.ox + (float)i * 2.0f + 1.0f) * WIND_TEXEL, z = ((float)WG.f.oz + (float)j * 2.0f + 1.0f) * WIND_TEXEL, t4[4];
                        wind_tile_sample(&WG.f, x, z, t4);
                        float sp = clampf_(sqrtf(t4[0] * t4[0] + t4[1] * t4[1]) / 25.0f, 0, 1), an = atan2f(t4[3], t4[2]);
                        u8 rr = (u8)(255 * sp * (0.5f + 0.5f * cosf(an))), gg = (u8)(255 * sp * (0.5f + 0.5f * cosf(an - 2.094f))), bb = (u8)(255 * sp * (0.5f + 0.5f * cosf(an + 2.094f)));
                        nk_fill_rect(cb, nk_rect(r.x + (float)i * cell, r.y + (float)j * cell, cell, cell), 0, nk_rgb(rr, gg, bb));
                    }
            }
        }
    } else WG.panel = false;
    nk_end(ctx);
}

bool wind_game_panel_open(void) { return WG.panel; }

// ------------------------------------------------------------------ console

static void cmd_wind_panel(int argc, const char **argv) {
    WG.panel = argc > 1 ? atoi(argv[1]) != 0 : !WG.panel;
}

static void cmd_wind_gust(int argc, const char **argv) {
    float st = argc > 1 ? (float)atof(argv[1]) : fmaxf(12.0f, WG.f.base.speed * 1.5f);
    vec3 cam = WG.cam;
    float p[3] = { cam.x - WG.f.dir[0] * 120.0f, cam.y, cam.z - WG.f.dir[1] * 120.0f };
    wind_add_gust(&WG.f, p, st, 30.0f, 260.0f);
    log_info("wind_gust: %.1f m/s front from 120 m upwind", (double)st);
}

static void cmd_wind_blast(int argc, const char **argv) {
    vec3 cam = WG.cam;
    float r = argc > 1 ? (float)atof(argv[1]) : 25.0f, st = argc > 2 ? (float)atof(argv[2]) : 25.0f;
    wind_game_blast(v3(cam.x, cam.y, cam.z + 10.0f), r, st);
    log_info("wind_blast: radius %.0f m, %.0f m/s, 10 m in front of +z of the camera", (double)r, (double)st);
}

static void cmd_wind_downwash(int argc, const char **argv) {
    vec3 cam = WG.cam;
    float life = argc > 1 ? (float)atof(argv[1]) : 10.0f;
    float p[3] = { cam.x, cam.y, cam.z };
    wind_add_downwash(&WG.f, p, 18.0f, 14.0f, life);
    log_info("wind_downwash: helicopter downwash stub at the camera for %.0f s", (double)life);
}

static void cmd_wind_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    vec3 cam = WG.cam;
    vec3 w = wind_game_at(cam);
    float c[3] = { cam.x, cam.y, cam.z }, fac = 1, d = wind_gust_front(&WG.f, c, &fac);
    log_info("wind: base %.1f m/s towards %.0f deg, gust %.2f, time %.2f s; at the camera (%.2f, %.2f, %.2f) m/s, gust x%.2f; next front %.0f m (x%.2f); "
             "tile origin (%d, %d)",
             (double)WG.f.base.speed, (double)WG.f.base.dir_deg, (double)WG.f.base.gust, WG.f.time, (double)w.x, (double)w.y, (double)w.z,
             (double)wind_gust_at(&WG.f, c), (double)d, (double)fac, WG.f.ox, WG.f.oz);
}

void wind_game_register(void) {
    WG.debug = cvar_reg_int("wind_debug", 0, 0, 1, 0, "wind field debug: arrows on the ground, the next gust front, every foliage instance's bend (docs/FOLIAGE_WIND.md)");
    WG.view = cvar_reg_int("wind_view", 0, 0, 2, 0, "foliage wind debug view: 0 shaded, 1 wind channels as colours (R flutter, G phase, B branch), 2 bend amount");
    WG.solo = cvar_reg_int("wind_solo", 0, 0, 3, 0, "foliage wind layer solo: 0 all (r_foliage_wind), 1 main bending, 2 detail bending, 3 edge flutter");
    WG.timescale = cvar_reg_float("wind_timescale", 1.0f, 0.0f, 4.0f, 0, "foliage wind time scale (slow motion; 1 = in step with every other machine)");
    WG.dir = cvar_reg_float("wind_dir", -1.0f, -1.0f, 360.0f, 0, "wind override: direction (deg towards, 0 = -Z), -1 = the weather's");
    WG.speed = cvar_reg_float("wind_speed", -1.0f, -1.0f, 60.0f, 0, "wind override: mean speed (m/s), -1 = the weather's");
    WG.gust = cvar_reg_float("wind_gust", -1.0f, -1.0f, 1.0f, 0, "wind override: gust amount 0..1, -1 = the weather's");
    cmd_register("wind_panel", cmd_wind_panel, "wind_panel [0|1]: foliage wind tuning panel (override, gust, layers, views, per-type parameters)");
    cmd_register("wind_gust_now", cmd_wind_gust, "wind_gust_now [m/s]: a gust front sweeping past the camera from 120 m upwind");
    cmd_register("wind_blast", cmd_wind_blast, "wind_blast [radius=25] [m/s=25]: an explosion's wind ring 10 m along +z of the camera");
    cmd_register("wind_downwash", cmd_wind_downwash, "wind_downwash [seconds=10]: helicopter downwash stub at the camera");
    cmd_register("wind_info", cmd_wind_info, "wind_info: the wind field at the camera");
}
