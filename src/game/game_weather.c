// Dynamic weather, game side (docs/WEATHER.md): the preset library (assets/world/weather/*.json),
// the host's WeatherState (World.weather: evaluated per tick, surfaces integrated, the Markov
// scheduler; replicated in snapshots), console commands, local events (rain / snow / storm start
// and stop, lightning), the weather_at() query (is this point under the sky? cached upward casts),
// the per-level puddle mask (low points from a height bake + authored "puddles" boxes), the
// renderer / sky / post / audio hooks, lens droplets, the debug overlay, the demo and the tests.
#include "game_internal.h"
#include "world/weather.h"
#include "world/season.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "render/post/post_settings.h"
#include "physics/physics.h"
#include "audio/audio.h"
#include "net/net.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include "cJSON.h"
#include "world/level.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define WX_MAX_CB 8
#define WX_CACHE 2048
#define WX_PUDDLE_RES 256
#define WX_PUDDLE_SIZE 192.0f

typedef struct ProbeEntry { i32 x, y, z; u32 tick; float exposed; } ProbeEntry;

static struct {
    WeatherLib lib;
    bool lib_loaded;
    SeasonTable seasons;           // assets/world/seasons.json
    int season;                    // active season (-1 = none: the plain Markov scheduler)
    Cvar *debug, *quality, *puddle_refl, *lens_cv, *lens_k, *cloud_shadows, *audio_vol;
    Cvar *on_rain_start, *on_rain_stop, *on_snow_start, *on_snow_stop, *on_storm_start, *on_storm_stop;
    // this frame (every machine)
    WeatherParams cur;
    bool have;
    float flash, rain, snowfall, exposed_cam;
    vec3 wind;
    float gust;
    double last_frame_tick;
    // events
    bool raining, snowing, storm;
    WeatherEventFn cbs[WX_MAX_CB];
    void *cb_user[WX_MAX_CB];
    int cb_count;
    int counts[WEV_COUNT];
    // thunder (delayed by the distance)
    struct { double at; float vol; } thunder[8];
    int thunder_n;
    u32 last_strike_tick;
    // weather_at cache
    ProbeEntry cache[WX_CACHE];
    // rain occlusion map
    vec3 occl_center;
    int occl_age;
    bool occl_init;
    // puddle mask
    TextureId puddle_tex;
    vec4 puddle_xf;
    u8 *puddle_mask;   // WX_PUDDLE_RES^2 (kept for tests / the debug view)
    // lens droplets
    WeatherLens lens;
    RenderLensDrop drops[RENDER_LENS_DROPS];
    vec3 prev_cam;
    float prev_yaw;
    bool lens_init;
    // demo
    bool demo;
    int demo_i;
    double demo_next;
    float demo_step;
    // tests
    struct { int mode, frames; bool quit; float min_wet, max_wet; int max_drops; } t;
} WX;

static double now_tick(World *w) {
    if (!w) return 0.0;
    if (!w->authority) return net_client_render_tick();
    return (double)w->tick + (G ? (double)G->alpha : 0.0);
}

bool weather_active(World *w) { return w && w->level.sky && w->weather.version != 0; }

static void ensure_lib(void) {
    if (WX.lib_loaded) return;
    weather_lib_load(&WX.lib, "assets/world/weather");
    seasons_load(&WX.seasons, "assets/world/seasons.json");
    WX.season = -1;
    WX.lib_loaded = true;
}

static float precip_rain(const WeatherParams *p) { return p->v[WP_PRECIP] * (1.0f - weather_precip_type(p->v[WP_TEMPERATURE])); }
static float precip_snow(const WeatherParams *p) { return p->v[WP_PRECIP] * weather_precip_type(p->v[WP_TEMPERATURE]); }

bool weather_current(World *w, WeatherParams *out) {
    if (!weather_active(w)) return false;
    if (out) *out = WX.cur;
    return WX.have;
}

float weather_lightning_flash(void) { return WX.have ? WX.flash : 0.0f; }

const char *weather_name(World *w) {
    if (!weather_active(w)) return w && w->level.sky ? "clear" : "indoor";
    ensure_lib();
    int p = w->weather.preset;
    return p < WX.lib.count ? WX.lib.presets[p].name : "custom";
}

// ------------------------------------------------------------------ queries

// Under the sky at p? 1 = open, 0 = a roof / overhang above. Cached per 1 m cell for 5 s.
float weather_exposure(World *w, vec3 p) {
    if (!w || !w->phys) return 1.0f;
    i32 x = (i32)floorf(p.x), y = (i32)floorf(p.y * 0.5f), z = (i32)floorf(p.z);
    u32 h = ((u32)x * 73856093u) ^ ((u32)y * 19349663u) ^ ((u32)z * 83492791u);
    ProbeEntry *e = &WX.cache[h % WX_CACHE];
    u32 tick = w->tick ? w->tick : 1;
    if (e->tick && e->x == x && e->y == y && e->z == z && tick - e->tick < 300u) return e->exposed;
    // Five upward casts (the cell centre + corners): a partial roof gives a partial value.
    static const float off[5][2] = { { 0.5f, 0.5f }, { 0.15f, 0.15f }, { 0.85f, 0.15f }, { 0.15f, 0.85f }, { 0.85f, 0.85f } };
    float open = 0;
    for (int i = 0; i < 5; i++) {
        vec3 o = v3((float)x + off[i][0], p.y + 0.3f, (float)z + off[i][1]);
        RayHit rh;
        if (!phys_raycast(w->phys, o, v3(0, 80, 0), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) open += 1.0f;
    }
    e->x = x; e->y = y; e->z = z; e->tick = tick;
    e->exposed = open / 5.0f;
    return e->exposed;
}

WeatherLocal weather_at(World *w, vec3 pos) {
    WeatherLocal l;
    memset(&l, 0, sizeof(l));
    l.exposed = 1.0f;
    if (!weather_active(w) || !WX.have) return l;
    l.exposed = weather_exposure(w, pos);
    l.rain = precip_rain(&WX.cur) * l.exposed;
    l.snow = precip_snow(&WX.cur) * l.exposed;
    l.wind = v3_scale(wind_game_at(pos), 0.25f + 0.75f * l.exposed);
    l.temperature = WX.cur.v[WP_TEMPERATURE];
    l.wetness = w->weather.wetness * l.exposed;
    l.snow_cover = w->weather.snow * l.exposed;
    l.fog = WX.cur.v[WP_FOG];
    l.indoor = l.exposed < 0.3f;
    return l;
}

// Ground grip multiplier (vehicles, sliding): wet -15 %, puddles -25 %, snow -45 %.
float weather_grip(World *w, vec3 pos) {
    if (!weather_active(w)) return 1.0f;
    float e = weather_exposure(w, pos);
    float g = 1.0f - e * (0.15f * w->weather.wetness + 0.10f * w->weather.puddles + 0.45f * w->weather.snow);
    return fmaxf(g, 0.4f);
}

// Hearing (zombies / AI): rain and wind mask sounds. 1 = clear .. 0.55 in a storm.
float weather_hearing_scale(World *w) {
    if (!weather_active(w) || !WX.have) return 1.0f;
    float r = WX.cur.v[WP_PRECIP], wi = fminf(WX.cur.v[WP_WIND] / 20.0f, 1.0f);
    return 1.0f - 0.3f * r - 0.15f * wi;
}

// ------------------------------------------------------------------ events

void weather_add_listener(WeatherEventFn fn, void *user) {
    if (WX.cb_count >= WX_MAX_CB || !fn) return;
    WX.cbs[WX.cb_count] = fn;
    WX.cb_user[WX.cb_count++] = user;
}

static const char *k_ev_names[WEV_COUNT] = { "rain_start", "rain_stop", "snow_start", "snow_stop", "storm_start", "storm_stop", "lightning" };
const char *weather_event_name(WeatherEvent e) { return e >= 0 && e < WEV_COUNT ? k_ev_names[e] : "?"; }

static void fire(WeatherEvent e) {
    WX.counts[e]++;
    for (int i = 0; i < WX.cb_count; i++) WX.cbs[i](e, WX.cb_user[i]);
    Cvar *cmds[WEV_COUNT] = { WX.on_rain_start, WX.on_rain_stop, WX.on_snow_start, WX.on_snow_stop, WX.on_storm_start, WX.on_storm_stop, NULL };
    if (cmds[e] && cmds[e]->str[0]) cbuf_add(cmds[e]->str);
    if (e != WEV_LIGHTNING && WX.debug && WX.debug->i) log_info("weather: %s", k_ev_names[e]);
}

// Hysteresis: start above 0.12, stop below 0.06 (no flicker during a fade).
static void update_events(void) {
    float r = precip_rain(&WX.cur), s = precip_snow(&WX.cur);
    bool storm = WX.cur.v[WP_LIGHTNING] > 0.5f || (WX.cur.v[WP_PRECIP] > 0.85f && WX.cur.v[WP_WIND] > 11.0f);
    if (!WX.raining && r > 0.12f) { WX.raining = true; fire(WEV_RAIN_START); }
    if (WX.raining && r < 0.06f) { WX.raining = false; fire(WEV_RAIN_STOP); }
    if (!WX.snowing && s > 0.12f) { WX.snowing = true; fire(WEV_SNOW_START); }
    if (WX.snowing && s < 0.06f) { WX.snowing = false; fire(WEV_SNOW_STOP); }
    if (storm != WX.storm) { WX.storm = storm; fire(storm ? WEV_STORM_START : WEV_STORM_STOP); }
}

// ------------------------------------------------------------------ puddle mask

static float vnoise(float x, float z) {
    float fx = floorf(x), fz = floorf(z);
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
#define H(a, b) ((float)((((u32)(i32)(a) * 1597334677u) ^ ((u32)(i32)(b) * 3812015801u)) * 2654435761u >> 8) / 16777216.0f)
    float a = H(fx, fz), b = H(fx + 1, fz), c = H(fx, fz + 1), d = H(fx + 1, fz + 1);
#undef H
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * tz;
}

// Low points: a top-down height bake of the static world (downward casts), each texel compared
// with its 7 x 7 neighbourhood (a 1 cm dip fills first), plus large faint patches on flat ground
// (fill only in heavy rain) and the level's authored "puddles" boxes ({"pos": [x, z], "size":
// [w, d], "value": 1}).
static void bake_puddles(World *w, const char *map) {
    if (WX.puddle_tex) { render_texture_destroy(WX.puddle_tex); WX.puddle_tex = 0; }
    if (!w || !w->phys || !w->level.sky) return;
    const int N = WX_PUDDLE_RES;
    double t0 = platform_time();
    float size = WX_PUDDLE_SIZE, texel = size / (float)N;
    float cx = 0, cz = 0;
    if (w->level.bounds_max.x > w->level.bounds_min.x) {
        cx = clampf((w->level.bounds_min.x + w->level.bounds_max.x) * 0.5f, -500.0f, 500.0f);
        cz = clampf((w->level.bounds_min.z + w->level.bounds_max.z) * 0.5f, -500.0f, 500.0f);
        if (w->level.bounds_max.x - w->level.bounds_min.x > size * 4.0f) cx = cz = 0;   // huge ground plane: around the origin
    }
    float ox = cx - size * 0.5f, oz = cz - size * 0.5f;
    float *hgt = (float *)malloc(sizeof(float) * N * N);
    float top = fminf(w->level.bounds_max.y + 1.0f, 80.0f);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            vec3 o = v3(ox + ((float)i + 0.5f) * texel, top, oz + ((float)j + 0.5f) * texel);
            RayHit rh;
            float h = -1e9f;
            if (phys_raycast(w->phys, o, v3(0, -(top + 20.0f), 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh) && rh.normal.y > 0.9f) h = rh.point.y;
            hgt[j * N + i] = h;
        }
    if (!WX.puddle_mask) WX.puddle_mask = (u8 *)malloc((size_t)N * N);
    for (int j = 0; j < N; j++)
        for (int i = 0; i < N; i++) {
            float h = hgt[j * N + i];
            float m = 0;
            if (h > -1e8f) {
                float sum = 0;
                int n = 0;
                for (int dj = -3; dj <= 3; dj++)
                    for (int di = -3; di <= 3; di++) {
                        int x = i + di, y = j + dj;
                        if (x < 0 || y < 0 || x >= N || y >= N) continue;
                        float hh = hgt[y * N + x];
                        if (hh < -1e8f || fabsf(hh - h) > 0.4f) continue;   // walls / crates are not the ground
                        sum += hh;
                        n++;
                    }
                float depth = n ? sum / (float)n - h : 0.0f;
                float dip = depth > 0.004f ? fminf((depth - 0.004f) / 0.04f, 1.0f) : 0.0f;
                float wx = ox + (float)i * texel, wz = oz + (float)j * texel;
                float nz = vnoise(wx / 5.0f, wz / 5.0f) * 0.65f + vnoise(wx / 1.7f, wz / 1.7f) * 0.35f;
                float flat = 0.4f * fmaxf(0.0f, fminf((nz - 0.55f) / 0.25f, 1.0f));
                m = fmaxf(dip, flat);
            }
            WX.puddle_mask[j * N + i] = (u8)(clampf(m, 0, 1) * 255.0f + 0.5f);
        }
    // Authored areas.
    if (map && map[0]) {
        cJSON *root = level_json_load(map);   // includes resolved
        const cJSON *pz;
        int count = 0;
        cJSON_ArrayForEach(pz, root ? cJSON_GetObjectItemCaseSensitive(root, "puddles") : NULL) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(pz, "pos"), *s = cJSON_GetObjectItemCaseSensitive(pz, "size"), *v = cJSON_GetObjectItemCaseSensitive(pz, "value");
            if (cJSON_GetArraySize(p) < 2 || cJSON_GetArraySize(s) < 2) continue;
            float px = (float)cJSON_GetArrayItem(p, 0)->valuedouble, pz_ = (float)cJSON_GetArrayItem(p, cJSON_GetArraySize(p) >= 3 ? 2 : 1)->valuedouble;
            float sx = (float)cJSON_GetArrayItem(s, 0)->valuedouble, sz = (float)cJSON_GetArrayItem(s, cJSON_GetArraySize(s) >= 3 ? 2 : 1)->valuedouble;
            float val = cJSON_IsNumber(v) ? (float)v->valuedouble : 1.0f;
            for (int j = 0; j < N; j++)
                for (int i = 0; i < N; i++) {
                    float wx = ox + ((float)i + 0.5f) * texel, wz = oz + ((float)j + 0.5f) * texel;
                    float dx = fabsf(wx - px) / (sx * 0.5f), dz = fabsf(wz - pz_) / (sz * 0.5f);
                    float d = fmaxf(dx, dz);
                    if (d >= 1.0f) continue;
                    float edge = fminf((1.0f - d) * 4.0f, 1.0f);   // soft rim
                    u8 *m = &WX.puddle_mask[j * N + i];
                    float nv = fmaxf((float)*m / 255.0f, val * edge);
                    *m = (u8)(clampf(nv, 0, 1) * 255.0f + 0.5f);
                }
            count++;
        }
        if (root) cJSON_Delete(root);
        if (count) log_info("weather: %d authored puddle areas", count);
    }
    free(hgt);
    u8 *rgba = (u8 *)malloc((size_t)N * N * 4);
    for (int i = 0; i < N * N; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = WX.puddle_mask[i]; rgba[i * 4 + 3] = 255; }
    WX.puddle_tex = render_texture_create(N, N, rgba, false, false);
    free(rgba);
    WX.puddle_xf = v4(ox, oz, 1.0f / size, 1.0f / size);
    log_info("weather: puddle mask %dx%d over %.0f m at (%.0f, %.0f) baked in %.0f ms", N, N, size, cx, cz, (platform_time() - t0) * 1000.0);
}

float weather_puddle_mask_at(float x, float z) {
    if (!WX.puddle_mask) return 0.0f;
    int i = (int)((x - WX.puddle_xf.x) * WX.puddle_xf.z * WX_PUDDLE_RES), j = (int)((z - WX.puddle_xf.y) * WX.puddle_xf.w * WX_PUDDLE_RES);
    if (i < 0 || j < 0 || i >= WX_PUDDLE_RES || j >= WX_PUDDLE_RES) return 0.0f;
    return (float)WX.puddle_mask[j * WX_PUDDLE_RES + i] / 255.0f;
}

// ------------------------------------------------------------------ host / level

static void set_preset(World *w, const char *spec, float seconds) {
    ensure_lib();
    WeatherParams tgt;
    float tr = 0;
    if (!weather_compose(&WX.lib, spec, &w->weather.to, &tgt, &tr)) { log_info("weather: unknown preset '%s'", spec); return; }
    weather_seasonal_params(&WX.lib, &tgt);   // the season's temperature: winter precipitation is snow
    weather_set_target(&w->weather, &tgt, w->tick, seconds >= 0 ? seconds : tr);
    int idx = weather_find_preset(&WX.lib, spec);
    w->weather.preset = idx >= 0 ? (u8)idx : 255;
    if (w->weather.random) {
        const WeatherPreset *p = idx >= 0 ? &WX.lib.presets[idx] : NULL;
        float stay = p ? (p->min_stay + p->max_stay) * 0.5f : 300.0f;
        w->weather.next_change_tick = w->tick + (u32)((w->weather.transition + stay) * SIM_HZ);
    }
}

const SeasonTable *weather_seasons(void) { ensure_lib(); return &WX.seasons; }
int weather_season(void) { ensure_lib(); return WX.season; }

// Host: the season drives the weather from now on (docs/MENU_LOBBY.md "Time, season and weather"):
// the scheduler's preset weights and temperature offset, the start weather (start "" = a weighted
// pick of the season's presets) set at once, the ground's snow / wetness, dynamic = the scheduler on.
void weather_apply_season(World *w, int season, const char *start, bool dynamic) {
    ensure_lib();
    if (season < 0 || season >= SEASON_COUNT) {
        WX.season = -1;
        weather_lib_set_bias(&WX.lib, NULL, 0);
        return;
    }
    const SeasonDef *sd = &WX.seasons.s[season];
    float bias[WEATHER_MAX_PRESETS];
    season_weather_bias(sd, &WX.lib, bias);
    weather_lib_set_bias(&WX.lib, bias, sd->temp_offset);
    WX.season = season;
    if (!w || !w->authority) return;
    const char *spec = start && start[0] ? start : season_pick_weather(sd, w->level.seed ^ (u32)season * 977u ^ w->tick);
    if (!spec[0]) spec = "clear";
    WeatherParams base = w->weather.to, p;
    float tr = 0;
    if (!weather_compose(&WX.lib, spec, &base, &p, &tr)) p = base;
    weather_seasonal_params(&WX.lib, &p);
    u32 seed = w->weather.seed;
    weather_state_init(&w->weather, &p, seed);
    int idx = weather_find_preset(&WX.lib, spec);
    w->weather.preset = idx >= 0 ? (u8)idx : 255;
    w->weather.random = dynamic ? 1 : 0;
    w->weather.snow = sd->snow;
    w->weather.wetness = sd->wetness;
    w->weather.puddles = sd->wetness * 0.6f;
    if (dynamic && idx >= 0) {
        const WeatherPreset *pr = &WX.lib.presets[idx];
        w->weather.next_change_tick = w->tick + (u32)(((pr->min_stay + pr->max_stay) * 0.5f) * SIM_HZ);
    }
    w->weather.version++;
    log_info("season %s: start weather %s, %+.0f C, %s weather", sd->id, spec, (double)sd->temp_offset, dynamic ? "dynamic" : "fixed");
}

// season [spring|summer|autumn|winter|off] [start preset] [static]: host test command.
static void cmd_season(int argc, const char **argv) {
    ensure_lib();
    if (argc < 2) {
        log_info("season: %s  (season <spring|summer|autumn|winter|off> [start preset] [static])", WX.season >= 0 ? WX.seasons.s[WX.season].id : "off");
        for (int i = 0; i < SEASON_COUNT; i++) {
            const SeasonDef *s = &WX.seasons.s[i];
            char b[256] = "";
            for (int k = 0; k < s->weather_count; k++) { size_t l = strlen(b); snprintf(b + l, sizeof(b) - l, " %s:%g", s->weather[k], (double)s->weight[k]); }
            log_info("  %-7s day %3d  %+3.0f C  snow %.1f  weather%s", s->id, s->day_of_year, (double)s->temp_offset, (double)s->snow, b);
        }
        return;
    }
    World *w = G ? G->world : NULL;
    if (!w || !w->authority) { log_info("season: host only"); return; }
    int s = season_from_id(argv[1]);
    if (s < 0 && strcmp(argv[1], "off")) { log_info("season: unknown '%s'", argv[1]); return; }
    bool dynamic = !(argc > 3 && !strcmp(argv[3], "static"));
    weather_apply_season(w, s, argc > 2 && strcmp(argv[2], "random") ? argv[2] : "", dynamic);
    if (s >= 0) daynight_set_calendar(w, WX.seasons.s[s].day_of_year, -1.0f);
}

void weather_level_loaded(World *w, const char *map) {
    ensure_lib();
    memset(WX.cache, 0, sizeof(WX.cache));
    WX.occl_init = false;
    WX.have = false;
    WX.raining = WX.snowing = WX.storm = false;
    WX.thunder_n = 0;
    WX.demo = false;
    weather_lens_init(&WX.lens, 0x1234u);
    if (!w) return;
    if (w->authority) {
        // Level default: "weather": "preset" or {"preset": "...", "random": true}.
        char spec[64] = "little_cloudy";
        bool random = false;
        cJSON *root = level_json_load(map);   // includes resolved
        const cJSON *wj = root ? cJSON_GetObjectItemCaseSensitive(root, "weather") : NULL;
        if (cJSON_IsString(wj)) dg_strcpy(spec, sizeof(spec), wj->valuestring);
        else if (cJSON_IsObject(wj)) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(wj, "preset");
            if (cJSON_IsString(p)) dg_strcpy(spec, sizeof(spec), p->valuestring);
            random = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(wj, "random"));
        }
        if (root) cJSON_Delete(root);
        WeatherParams p;
        memset(&p, 0, sizeof(p));
        WeatherParams base;
        int clear = weather_find_preset(&WX.lib, "clear");
        base = clear >= 0 ? WX.lib.presets[clear].p : p;
        base.v[WP_WIND_DIR] = 60.0f;
        if (!weather_compose(&WX.lib, spec, &base, &p, NULL)) p = base;
        weather_state_init(&w->weather, &p, w->level.seed ^ 0x9E3779B9u);
        int idx = weather_find_preset(&WX.lib, spec);
        w->weather.preset = idx >= 0 ? (u8)idx : 255;
        w->weather.random = random;
    }
    bake_puddles(w, map);
}

// Frozen world (scene handpose): the weather holds its current values (or a clear sky), the
// scheduler and the surface integration stop; unfreezing restores the state, the transition
// resuming where it stopped.
void weather_freeze(World *w, bool on, bool neutral) {
    static WeatherState saved;
    static u32 tick0;
    static bool frozen;
    if (!w || !w->authority) return;
    if (on && !frozen) {
        ensure_lib();
        saved = w->weather;
        tick0 = w->tick;
        frozen = true;
        if (!w->weather.version) return;
        if (neutral) set_preset(w, "clear", 0.0f);
        else {
            WeatherParams cur;
            weather_eval(&w->weather, w->tick, SIM_DT, &cur);
            weather_set_target(&w->weather, &cur, w->tick, 0.0f);
        }
        w->weather.random = 0;
    } else if (!on && frozen) {
        frozen = false;
        u32 dt = w->tick - tick0;
        saved.start_tick += dt;
        if (saved.next_change_tick) saved.next_change_tick += dt;
        u16 v = w->weather.version;
        w->weather = saved;
        w->weather.version = (u16)(v + 1);
    }
}

void weather_host_tick(World *w) {
    if (!w || !w->authority || !w->weather.version) return;
    if (game_world_frozen()) return;
    ensure_lib();
    weather_schedule(&w->weather, &WX.lib, w->tick, SIM_DT);
    WeatherParams cur;
    weather_eval(&w->weather, w->tick, SIM_DT, &cur);
    weather_integrate(&w->weather, &cur, SIM_DT);
}

// ------------------------------------------------------------------ per frame

static int quality(void) {
    int q = WX.quality->i;
    if (q >= 0) return q;
    Cvar *pp = cvar_find("r_post_preset");
    int p = pp ? pp->i : 2;
    return p <= 0 ? 1 : (p == 1 ? 2 : 3);
}

static void lens_frame(World *w, const RenderCamera *cam, const Player *lp, float dt) {
    if (!WX.lens_init) { weather_lens_init(&WX.lens, 0xBEEFu); WX.lens_init = true; }
    bool fp = lp && !cvar_find("cl_thirdperson")->i && lp->sim.move.mode != MOVE_DEAD;
    WeatherLensInput in;
    memset(&in, 0, sizeof(in));
    in.rain = WX.rain;
    in.exposed = WX.exposed_cam;
    vec3 fwd = dir_from_angles(cam->yaw, cam->pitch);
    in.look_up = fwd.y;
    vec3 vel = lp ? lp->sim.move.vel : v3(0, 0, 0);
    vec3 flat = v3_norm(v3(fwd.x, 0, fwd.z));
    in.forward_speed = v3_dot(vel, flat);
    in.side_speed = v3_dot(vel, right_from_yaw(cam->yaw)) + (WX.lens_init ? (cam->yaw - WX.prev_yaw) / fmaxf(dt, 1e-3f) * -2.0f : 0.0f);
    // the wind drives the drops sideways across the lens (its component along the camera's right)
    in.side_speed += v3_dot(WX.wind, right_from_yaw(cam->yaw)) * 0.35f * WX.exposed_cam;
    in.side_speed = clampf(in.side_speed, -20.0f, 20.0f);
    in.temperature = WX.cur.v[WP_TEMPERATURE];
    in.wiped = !fp;
    weather_lens_step(&WX.lens, &in, dt);
    WX.prev_yaw = cam->yaw;
    (void)w;
}

void weather_frame(World *w, const RenderCamera *cam, const Player *lp) {
    RenderWeather rw;
    memset(&rw, 0, sizeof(rw));
    float dt = fminf((float)g_engine.frame_dt, 0.1f);
    if (!weather_active(w)) {
        WX.have = false;
        wind_game_frame(NULL, now_tick(w) * SIM_DT, cam->pos, dt, &rw);   // foliage still moves: a light breeze
        render_set_weather(&rw);
        audio_loop_set("rain_loop", 0);
        audio_loop_set("wind_loop", 0);
        return;
    }
    double tick = now_tick(w);
    u32 itick = (u32)tick;
    weather_eval(&w->weather, itick, SIM_DT, &WX.cur);
    // Clients integrate the surfaces between packs (the host does it per tick).
    if (!w->authority && WX.have && tick > WX.last_frame_tick) weather_integrate(&w->weather, &WX.cur, (float)(tick - WX.last_frame_tick) * SIM_DT);
    WX.last_frame_tick = tick;
    bool strike = false;
    float dist = 0;
    WX.flash = weather_lightning(&w->weather, &WX.cur, itick, SIM_DT, &strike, &dist);
    if (strike && itick != WX.last_strike_tick) {
        WX.last_strike_tick = itick;
        fire(WEV_LIGHTNING);
        if (WX.thunder_n < 8) {
            WX.thunder[WX.thunder_n].at = platform_time() + dist / 343.0;
            WX.thunder[WX.thunder_n++].vol = clampf(1.6f - dist / 3000.0f, 0.25f, 1.0f);
        }
    }
    for (int i = 0; i < WX.thunder_n;) {
        if (platform_time() >= WX.thunder[i].at) {
            audio_play_2d("thunder", WX.thunder[i].vol * WX.audio_vol->f, SND_SFX);
            WX.thunder[i] = WX.thunder[--WX.thunder_n];
        } else i++;
    }
    WX.have = true;
    WX.rain = precip_rain(&WX.cur);
    WX.snowfall = precip_snow(&WX.cur);
    // the wind field (game_wind.c, docs/FOLIAGE_WIND.md): gust fronts, turbulence, local sources
    WeatherWind wind = weather_wind(&WX.cur, tick * SIM_DT);
    wind_game_frame(&WX.cur, tick * SIM_DT, cam->pos, dt, &rw);
    WX.wind = rw.wind;
    WX.gust = wind.gust;
    WX.exposed_cam = weather_exposure(w, cam->pos);
    update_events();

    // Rain occlusion map: re-rendered when the camera moved 4 m or every 30 frames (doors, props).
    vec3 c = v3(floorf(cam->pos.x / 2.0f) * 2.0f, cam->pos.y, floorf(cam->pos.z / 2.0f) * 2.0f);
    bool upd = !WX.occl_init || v3_len(v3_sub(c, WX.occl_center)) > 4.0f || ++WX.occl_age >= 30;
    if (upd) { WX.occl_center = c; WX.occl_age = 0; WX.occl_init = true; }

    lens_frame(w, cam, lp, dt);
    int nd = DG_MIN(WX.lens.count, RENDER_LENS_DROPS);
    for (int i = 0; i < nd; i++) {
        const WeatherDrop *d = &WX.lens.d[i];
        WX.drops[i].pos_r = v4(d->x, d->y, d->r, d->stretch);
        WX.drops[i].info = v4(clampf(d->alpha, 0, 1), d->age, d->vy, 0);
    }

    rw.wetness = w->weather.wetness;
    rw.puddles = w->weather.puddles;
    rw.snow_cover = w->weather.snow;
    rw.rain = WX.rain;
    rw.snowfall = WX.snowfall;
    rw.lightning = WX.flash;
    rw.gust = WX.gust;
    rw.precip_quality = quality();
    rw.occlusion_update = upd;
    rw.occlusion_center = WX.occl_center;
    rw.occlusion_extent = 48.0f;
    rw.puddle_mask = WX.puddle_tex;
    rw.puddle_xform = WX.puddle_xf;
    rw.puddle_reflections = WX.puddle_refl->i != 0;
    rw.drops = WX.drops;
    rw.drop_count = WX.lens_cv->i ? nd : 0;
    rw.lens_strength = WX.lens_cv->i ? WX.lens_k->f : 0.0f;
    render_set_weather(&rw);

    // Ambience: rain loop (muffled under a roof), wind loop by strength.
    float v = WX.audio_vol->f;
    audio_loop_set("rain_loop", v * WX.rain * (0.35f + 0.65f * WX.exposed_cam));
    // the wind loop follows the local wind (gusts sweep past), louder among trees (rustle)
    float rustle = wind_game_rustle(cam->pos);
    audio_loop_set("wind_loop", v * clampf((v3_len(WX.wind) - 2.0f) / 14.0f, 0.0f, 1.0f) * (0.4f + 0.6f * WX.exposed_cam) * (0.75f + 0.5f * rustle) + v * WX.snowfall * 0.2f);
}

// Sky hooks (daynight_environment): cloud cover / density / softness / darkness, light loss.
void weather_environment(World *w, RenderEnvironment *env) {
    if (!weather_active(w) || !WX.have) return;
    const WeatherParams *p = &WX.cur;
    env->cloud_density = clampf(0.55f + 0.45f * p->v[WP_CLOUD_DENSITY], 0.05f, 1.0f);
    env->cloud_softness = clampf(p->v[WP_FOG] * 20.0f + (1.0f - p->v[WP_CLOUD_DENSITY]) * 0.3f, 0.0f, 1.0f);
    env->cloud_darkness = clampf(p->v[WP_PRECIP] * 0.6f + (p->v[WP_CLOUD_DENSITY] - 0.7f) * 0.8f, 0.0f, 0.9f);
    if (!WX.cloud_shadows->i) env->cloud_shadow_tau = 0.0f;
    // Lightning lights everything (ambient) for a few frames.
    env->ambient_scale *= 1.0f + WX.flash * 8.0f;
}

void weather_post(PostSettings *s) {
    World *w = G ? G->world : NULL;
    if (!weather_active(w) || !WX.have) return;
    float f = WX.cur.v[WP_FOG];
    // Fog into the froxel volumetrics: the weather's density is a floor, its height blends in as the
    // fog thickens; rain / snow add a little scattering haze.
    float d = f * 1.5f + (WX.rain + WX.snowfall * 1.5f) * 0.004f;
    if (d > s->fog_density) {
        float k = clampf((d - s->fog_density) / fmaxf(d, 1e-4f), 0.0f, 1.0f);
        s->fog_height = s->fog_height + (WX.cur.v[WP_FOG_HEIGHT] - s->fog_height) * k;
        s->fog_density = d;
    }
    s->fog_ambient = fmaxf(s->fog_ambient, clampf((s->fog_density - 0.006f) / 0.03f, 0.0f, 1.0f));
    s->exposure_comp += WX.flash * 0.4f;
}

float weather_cloud_cover(World *w, float fallback) {
    if (!weather_active(w) || !WX.have) return fallback;
    return WX.cur.v[WP_CLOUD_COVER];
}

float weather_fog_density(World *w, float fallback) {
    if (!weather_active(w) || !WX.have) return fallback;
    return fmaxf(fallback, WX.cur.v[WP_FOG]);
}

// ------------------------------------------------------------------ commands

static World *host_world(const char *cmd) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority) { log_info("%s: host only", cmd); return NULL; }
    if (!w->weather.version) { log_info("%s: no weather on this level", cmd); return NULL; }
    return w;
}

static void cmd_weather_set(int argc, const char **argv) {
    World *w = host_world("weather_set");
    if (!w) return;
    if (argc < 2) {
        ensure_lib();
        char b[512] = "";
        for (int i = 0; i < WX.lib.count; i++) { size_t l = strlen(b); snprintf(b + l, sizeof(b) - l, " %s", WX.lib.presets[i].name); }
        log_info("usage: weather_set <preset[+preset]> [seconds]  presets:%s", b);
        return;
    }
    float sec = argc > 2 ? (float)atof(argv[2]) : -1.0f;
    set_preset(w, argv[1], sec);
    log_info("weather_set %s over %.0f s", argv[1], w->weather.transition);
}

static void cmd_weather_lock(int argc, const char **argv) {
    World *w = host_world("weather_lock");
    if (!w) return;
    w->weather.locked = argc > 1 ? (u8)(atoi(argv[1]) != 0) : (u8)!w->weather.locked;
    w->weather.version++;
    log_info("weather %s", w->weather.locked ? "locked" : "unlocked");
}

static void cmd_weather_random(int argc, const char **argv) {
    World *w = host_world("weather_random");
    if (!w) return;
    bool on = argc > 1 ? (!strcmp(argv[1], "on") || atoi(argv[1]) != 0) : !w->weather.random;
    w->weather.random = on;
    w->weather.next_change_tick = on ? w->tick + 1 : 0;
    w->weather.version++;
    log_info("weather_random %s", on ? "on" : "off");
}

static void print_params(World *w) {
    char b[512];
    int n = snprintf(b, sizeof(b), "weather v%u preset %s%s%s |", w->weather.version, w->weather.preset < WX.lib.count ? WX.lib.presets[w->weather.preset].name : "custom",
                     w->weather.locked ? " locked" : "", w->weather.random ? " random" : "");
    for (int i = 0; i < WP_COUNT && n < (int)sizeof(b); i++) n += snprintf(b + n, sizeof(b) - n, " %s %.4g", weather_param_name(i), WX.cur.v[i]);
    log_info("%s", b);
    log_info("weather: wetness %.3f puddles %.3f snow %.3f | rain %.2f snowfall %.2f | flash %.2f | exposed here %.2f | transition %.0f s from tick %u | next change %u",
             w->weather.wetness, w->weather.puddles, w->weather.snow, WX.rain, WX.snowfall, WX.flash, WX.exposed_cam, w->weather.transition, w->weather.start_tick,
             w->weather.next_change_tick);
}

// weather_params                        print
// weather_params <name> <value> ... [time <s>]   set parameters on the host (custom target)
static void cmd_weather_params(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    if (!w || !w->weather.version) { log_info("weather_params: no weather"); return; }
    ensure_lib();
    if (argc < 3) { print_params(w); return; }
    if (!w->authority) { log_info("weather_params: host only"); return; }
    WeatherParams tgt = w->weather.to;
    float sec = 10.0f;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "time")) { sec = (float)atof(argv[i + 1]); continue; }
        int k = -1;
        for (int p = 0; p < WP_COUNT; p++) if (!strcmp(argv[i], weather_param_name(p))) k = p;
        if (k < 0) { log_info("weather_params: unknown '%s'", argv[i]); return; }
        tgt.v[k] = (float)atof(argv[i + 1]);
    }
    weather_set_target(&w->weather, &tgt, w->tick, sec);
    w->weather.preset = 255;
}

// weather_surface <wetness> <puddles> <snow>: force the accumulated state (host; tests / shots).
static void cmd_weather_surface(int argc, const char **argv) {
    World *w = host_world("weather_surface");
    if (!w) return;
    if (argc < 4) { log_info("usage: weather_surface <wetness> <puddles> <snow>"); return; }
    w->weather.wetness = clampf((float)atof(argv[1]), 0, 1);
    w->weather.puddles = clampf((float)atof(argv[2]), 0, 1);
    w->weather.snow = clampf((float)atof(argv[3]), 0, 1);
    w->weather.version++;
}

static const char *k_demo[] = { "clear", "little_cloudy", "cloudy", "light_rain", "rain", "storm", "fog", "overcast", "light_snow", "snow", "blizzard", "clear" };
#define DEMO_N ((int)(sizeof(k_demo) / sizeof(k_demo[0])))

// weather_demo [seconds|stop]: every preset in turn over (default) 120 s, surfaces sped up.
static void cmd_weather_demo(int argc, const char **argv) {
    World *w = host_world("weather_demo");
    if (!w) return;
    if (argc > 1 && !strcmp(argv[1], "stop")) { WX.demo = false; return; }
    float total = argc > 1 ? fmaxf((float)atof(argv[1]), 12.0f) : 120.0f;
    WX.demo = true;
    WX.demo_i = 0;
    WX.demo_step = total / (float)DEMO_N;
    WX.demo_next = 0;
    log_info("weather_demo: %d presets, %.0f s each", DEMO_N, WX.demo_step);
}

static void demo_frame(void) {
    World *w = G ? G->world : NULL;
    if (!WX.demo || !w || !w->authority || !w->weather.version) return;
    double now = platform_time();
    if (now < WX.demo_next) return;
    if (WX.demo_i >= DEMO_N) { WX.demo = false; log_info("weather_demo: done"); return; }
    set_preset(w, k_demo[WX.demo_i], WX.demo_step * 0.6f);
    // Surfaces follow faster than real time so the demo shows them.
    const char *n = k_demo[WX.demo_i];
    if (strstr(n, "rain") || !strcmp(n, "storm")) { w->weather.wetness = fmaxf(w->weather.wetness, 0.7f); w->weather.puddles = fmaxf(w->weather.puddles, !strcmp(n, "light_rain") ? 0.3f : 0.8f); }
    if (strstr(n, "snow") || !strcmp(n, "blizzard")) w->weather.snow = fmaxf(w->weather.snow, 0.6f);
    if (!strcmp(n, "clear") && WX.demo_i > 0) w->weather.wetness = w->weather.puddles = w->weather.snow = 0.0f;
    log_info("weather_demo: %s", n);
    WX.demo_i++;
    WX.demo_next = now + WX.demo_step;
}

// ------------------------------------------------------------------ tests

// weather_test indoor: weather_at() over the dev room's weather yard (roof, overhang, open).
// weather_test drops [quit]: the lens droplet sim, 300 frames headless in heavy rain then 300 dry.
// weather_test dump: the replicated state (identical on host and clients at fixed ticks).
// weather_test rates: puddles / wetness over 10 simulated minutes of rain then drying.
static void cmd_weather_test(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    const char *what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "indoor")) {
        if (!w) return;
        struct { const char *name; vec3 p; float lo, hi; } k[] = {
            { "open yard", v3(-56, 0.5f, 28), 0.99f, 1.0f },
            { "under the roof", v3(-50, 0.5f, 20), 0.0f, 0.01f },
            { "under the overhang", v3(-40, 0.5f, 12.3f), 0.0f, 0.21f },
            { "car (under the body)", v3(-58, 0.0f, 18), 0.0f, 0.21f },
            { "inside the dev room hall", v3(0, 1.0f, 0), 0.0f, 1.0f },
        };
        int fails = 0;
        for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++) {
            memset(WX.cache, 0, sizeof(WX.cache));
            float e = weather_exposure(w, k[i].p);
            WeatherLocal l = weather_at(w, k[i].p);
            bool ok = e >= k[i].lo && e <= k[i].hi;
            fails += !ok;
            log_info("weather_test indoor: %-24s exposed %.2f indoor %d -> %s", k[i].name, e, l.indoor, ok ? "ok" : "FAIL");
        }
        // Cache: a second query does no casts (same answer).
        double t0 = platform_time();
        for (int i = 0; i < 10000; i++) weather_exposure(w, v3(-56, 0.5f, 28));
        log_info("weather_test indoor: 10000 cached queries %.2f ms", (platform_time() - t0) * 1000.0);
        log_info("weather_test indoor: %s", fails ? "FAIL" : "PASS");
        if (argc > 2 && !strcmp(argv[2], "quit")) cbuf_add("quit");
        return;
    }
    if (!strcmp(what, "drops")) {
        WeatherLens l;
        weather_lens_init(&l, 77u);
        WeatherLensInput in;
        memset(&in, 0, sizeof(in));
        in.rain = 1.0f;
        in.exposed = 1.0f;
        in.look_up = 0.3f;
        in.forward_speed = 3.0f;
        in.temperature = 10.0f;
        int max_n = 0, ran = 0;
        bool bad = false;
        for (int f = 0; f < 600; f++) {
            if (f == 300) in.rain = 0.0f;
            weather_lens_step(&l, &in, 1.0f / 60.0f);
            max_n = DG_MAX(max_n, l.count);
            for (int i = 0; i < l.count; i++) {
                const WeatherDrop *d = &l.d[i];
                if (!isfinite(d->x) || !isfinite(d->y) || d->r <= 0 || d->r > 0.041f || d->alpha > 1.0f) bad = true;
                if (d->vy > 0.01f) ran++;
            }
            if (f == 299) log_info("weather_test drops: after 300 frames of rain: %d drops (max %d), %d running drop-frames", l.count, max_n, ran);
        }
        bool ok = !bad && max_n > 20 && max_n <= WEATHER_LENS_MAX && l.count < max_n / 3 && ran > 0;
        log_info("weather_test drops: 300 dry frames later: %d drops -> %s", l.count, ok ? "PASS" : "FAIL");
        if (argc > 2 && !strcmp(argv[2], "quit")) cbuf_add("quit");
        return;
    }
    if (!strcmp(what, "dump")) {
        if (!w || !w->weather.version) return;
        WeatherParams a, b;
        weather_eval(&w->weather, 100000, SIM_DT, &a);
        weather_eval(&w->weather, w->weather.start_tick + 600, SIM_DT, &b);
        log_info("weather_test dump: %s v%u preset %u seed %u start %u tr %.1f | cover@100000 %.5f precip %.5f | cover@start+10s %.5f precip %.5f fog %.5f | wet %.3f",
                 w->authority ? "host" : "client", w->weather.version, w->weather.preset, w->weather.seed, w->weather.start_tick, w->weather.transition,
                 a.v[WP_CLOUD_COVER], a.v[WP_PRECIP], b.v[WP_CLOUD_COVER], b.v[WP_PRECIP], b.v[WP_FOG], w->weather.wetness);
        return;
    }
    if (!strcmp(what, "rates")) {
        ensure_lib();
        WeatherState s;
        int ri = weather_find_preset(&WX.lib, "rain"), ci = weather_find_preset(&WX.lib, "clear");
        weather_state_init(&s, &WX.lib.presets[ri].p, 1);
        float t_wet = -1, t_pud = -1;
        for (int i = 0; i < 600 * 60; i++) {
            weather_integrate(&s, &WX.lib.presets[ri].p, SIM_DT);
            float t = (float)i * SIM_DT;
            if (t_wet < 0 && s.wetness >= 0.99f) t_wet = t;
            if (t_pud < 0 && s.puddles >= 0.99f) t_pud = t;
        }
        float t_dry = -1;
        for (int i = 0; i < 3600 * 60; i++) {
            weather_integrate(&s, &WX.lib.presets[ci].p, SIM_DT);
            if (t_dry < 0 && s.wetness <= 0.01f) t_dry = (float)i * SIM_DT;
        }
        log_info("weather_test rates: rain soaks in %.0f s, puddles full in %.0f s, dry again in %.0f s (puddles %.2f) -> %s", t_wet, t_pud, t_dry, s.puddles,
                 t_wet > 15 && t_wet < 60 && t_pud > 60 && t_pud < 400 && t_dry > 60 ? "PASS" : "FAIL");
        return;
    }
    if (!strcmp(what, "frames")) {
        // Frame stats: run N frames, log the precipitation / lens pass GPU time.
        WX.t.mode = 1;
        WX.t.frames = 0;
        WX.t.quit = argc > 2 && !strcmp(argv[2], "quit");
        return;
    }
    log_info("usage: weather_test indoor|drops|dump|rates|frames [quit]");
}

static void test_frame(void) {
    if (WX.t.mode != 1) return;
    if (++WX.t.frames < 120) return;
    float ms[4] = { 0 };
    static const char *names[4] = { "lens_drops", "lens_apply", "transparent", "rain_occlusion" };
    int n = render_pass_count();
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 4; k++)
            if (!strcmp(render_pass_name(i), names[k])) ms[k] += render_pass_ms(i);
    log_info("weather_test frames: lens %.3f + %.3f ms, transparent (incl. precipitation) %.3f ms, drops %d, rain %.2f snow %.2f quality %d", ms[0], ms[1], ms[2],
             WX.lens.count, WX.rain, WX.snowfall, quality());
    WX.t.mode = 0;
    if (WX.t.quit) cbuf_add("quit");
}

// ------------------------------------------------------------------ debug overlay

void weather_draw(void) {
    demo_frame();
    test_frame();
    World *w = G ? G->world : NULL;
    if (!WX.debug->i || !w || !w->weather.version) return;
    float s = ui_scale(), x = 12 * s, y = ui_height() * 0.42f;
    u32 col = UI_RGBA(190, 225, 255, 235);
    const char *pn = w->weather.preset < WX.lib.count ? WX.lib.presets[w->weather.preset].name : "custom";
    ui_textf(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, "WEATHER  %s  v%u%s%s", pn, w->weather.version, w->weather.locked ? "  locked" : "", w->weather.random ? "  random" : "");
    y += 16 * s;
    for (int i = 0; i < WP_COUNT; i++) {
        float from = w->weather.from.v[i], to = w->weather.to.v[i];
        ui_textf(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, "%-14s %8.4g  (%.4g -> %.4g)", weather_param_name(i), WX.cur.v[i], from, to);
        y += 14 * s;
    }
    ui_textf(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, "wet %.2f  puddles %.2f  snow %.2f  rain %.2f  snowfall %.2f", w->weather.wetness, w->weather.puddles, w->weather.snow, WX.rain,
             WX.snowfall);
    y += 14 * s;
    ui_textf(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, "exposed %.2f  flash %.2f  drops %d  grip %.2f  quality %d", WX.exposed_cam, WX.flash, WX.lens.count,
             game_local_player() ? weather_grip(w, game_local_player()->sim.move.pos) : 1.0f, quality());
    y += 14 * s;
    if (w->weather.next_change_tick && w->weather.random)
        ui_textf(x, y, FONT_SMALL, col, UI_ALIGN_LEFT, "next change in %.0f s", (float)((i64)w->weather.next_change_tick - (i64)w->tick) * SIM_DT);
    if (WX.debug->i >= 2 && game_local_player()) {
        // Exposure probes around the player (green open, red covered).
        vec3 p = game_local_player()->sim.move.pos;
        for (int dz = -6; dz <= 6; dz++)
            for (int dx = -6; dx <= 6; dx++) {
                vec3 q = v3(floorf(p.x) + (float)dx + 0.5f, p.y + 0.1f, floorf(p.z) + (float)dz + 0.5f);
                float e = weather_exposure(w, q);
                dbg_cross(q, 0.12f, e > 0.5f ? DBG_GREEN : DBG_RED, 0);
            }
    }
}

// ------------------------------------------------------------------ registration

void weather_register(void) {
    WX.debug = cvar_reg_int("weather_debug", 0, 0, 2, CVAR_LOCKED, "weather overlay: 1 parameters, 2 + exposure probes around the player");
    WX.quality = cvar_reg_int("r_precip_quality", -1, -1, 3, CVAR_TIER, "rain / snow quality: 0 off, 1 low, 2 medium, 3 high, -1 = from the post preset");
    WX.puddle_refl = cvar_reg_bool("r_puddle_reflections", true, CVAR_TIER, "mirror-smooth puddles (screen-space reflections)");
    WX.lens_cv = cvar_reg_bool("r_lens_drops", true, CVAR_LOCKED, "rain droplets on the camera lens (first person)");
    WX.lens_k = cvar_reg_float("r_lens_drops_intensity", 0.8f, 0.0f, 1.0f, CVAR_LOCKED, "lens droplet strength");
    WX.cloud_shadows = cvar_reg_bool("r_cloud_shadows", true, CVAR_LOCKED, "cloud shadows on the ground");
    WX.audio_vol = cvar_reg_float("snd_weather", 0.8f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "rain / wind / thunder volume");
    WX.on_rain_start = cvar_reg_string("weather_on_rain_start", "", CVAR_LOCKED, "console commands run when rain starts");
    WX.on_rain_stop = cvar_reg_string("weather_on_rain_stop", "", CVAR_LOCKED, "console commands run when rain stops");
    WX.on_snow_start = cvar_reg_string("weather_on_snow_start", "", CVAR_LOCKED, "console commands run when snow starts");
    WX.on_snow_stop = cvar_reg_string("weather_on_snow_stop", "", CVAR_LOCKED, "console commands run when snow stops");
    WX.on_storm_start = cvar_reg_string("weather_on_storm_start", "", CVAR_LOCKED, "console commands run when a storm starts");
    WX.on_storm_stop = cvar_reg_string("weather_on_storm_stop", "", CVAR_LOCKED, "console commands run when a storm ends");
    cmd_register("weather_set", cmd_weather_set, "weather_set <preset[+preset]> [seconds] (host)");
    cmd_register("season", cmd_season, "season [spring|summer|autumn|winter|off] [start preset|random] [static]: the season drives the calendar day and the weather (host); no args: list");
    cmd_register("weather_lock", cmd_weather_lock, "weather_lock [0|1]: no random changes (host)");
    cmd_register("weather_random", cmd_weather_random, "weather_random on|off: Markov scheduler (host)");
    cmd_register("weather_params", cmd_weather_params, "weather_params [<name> <value> ... [time <s>]]: print / set parameters");
    cmd_register("weather_surface", cmd_weather_surface, "weather_surface <wetness> <puddles> <snow> (host)");
    cmd_register("weather_demo", cmd_weather_demo, "weather_demo [seconds|stop]: every preset in turn (host)");
    cmd_register("weather_test", cmd_weather_test, "weather_test indoor|drops|dump|rates|frames [quit]");
    ensure_lib();
}
