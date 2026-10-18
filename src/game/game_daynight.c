// Day / night cycle, game side (docs/DAYNIGHT.md): the config (assets/world/daynight.json, hot
// reload, per-level overrides), the host's time state (World.tod, replicated in every snapshot),
// events (sunrise / sunset / midnight / noon -> EV_TOD -> scripts, callbacks, ambience), light
// schedules, the per-frame sky / lighting environment for the renderer (sun / moon positions,
// key + fill light colours through the atmosphere transmittance, stable shadow direction,
// clouds, stars, IBL scale), exposure / fog curves, the gameplay API, the clock HUD, the
// time-of-day editor and the tests. The shadows (key / second light, softness, cutoff, cloud
// optical depth) come from the sky-driven shadow driver (game_shadow_sky.c, docs/SHADOWS.md).
#include "game_internal.h"
#include "render/shadow_sky.h"
#include "ui/hud.h"
#include "ui/menus.h"
#include "scene.h"
#include "world/time_of_day.h"
#include "render/render.h"
#include "render/post/post_settings.h"
#include "physics/physics.h"
#include "audio/audio.h"
#include "net/net.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/engine.h"
#include "core/platform.h"
#include "nuklear_config.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)
#define DN_MAX_CB 8

static struct {
    TodConfig cfg;
    bool cfg_loaded;
    i64 cfg_mtime;
    double next_poll;
    char level[32];
    Cvar *atmosphere, *clock, *sky_debug, *aerial_scale, *aerial_max, *weather_clouds, *weather_fog;
    Cvar *on_sunrise, *on_sunset, *on_midnight, *on_noon, *night_ambience, *day_ambience;
    TodSky sky;                  // this frame
    double frame_days;
    bool have_sky;
    float daylight;            // 0 night .. 1 full sun (daynight_daylight)
    double host_last_days;
    bool host_init;
    vec3 key_dir;                // shadowed light direction (stable steps)
    vec3 second_dir;             // the moon's tile while the sun owns the cascades (stable steps)
    bool key_moon, key_init, second_init;
    float cloud_cover;
    // demo
    bool demo;
    float demo_prev_len;
    double demo_end_days;
    // editor
    bool editor;
    int edit_curve;
    // callbacks
    TodEventFn cbs[DN_MAX_CB];
    void *cb_user[DN_MAX_CB];
    int cb_count;
    // tests
    struct {
        int mode;                // 1 = cycle test running
        int frames, warm;
        float min_ev, max_ev, floor_ev;
        float prev_lum, max_jump, max_jump_hours, prev_latest;
        int refilters_start;
        double start_days;
        int events[4];
        float sky_ms[7];
        int perf_frames;
        bool quit;
    } t;
    Cvar *disc_zoom;
} DN;

static double now_tick(World *w) {
    if (!w) return 0.0;
    if (!w->authority) return net_client_render_tick();
    return (double)w->tick + (G ? (double)G->alpha : 0.0);
}

// ------------------------------------------------------------------ config

static void load_config(const char *level) {
    TodConfig c;
    tod_config_default(&c);
    tod_config_load(&c, "assets/world/daynight.json", level);
    DN.cfg = c;
    DN.cfg_loaded = true;
    dg_strcpy(DN.level, sizeof(DN.level), level ? level : "");
    char p[512];
    DN.cfg_mtime = platform_file_mtime(platform_data_path("assets/world/daynight.json", p, sizeof(p)));
}

static void poll_reload(void) {
    if (g_engine.real_time < DN.next_poll) return;
    DN.next_poll = g_engine.real_time + 1.0;
    char p[512];
    i64 t = platform_file_mtime(platform_data_path("assets/world/daynight.json", p, sizeof(p)));
    if (t && t != DN.cfg_mtime) {
        load_config(DN.level);
        log_info("daynight: %s changed, reloaded", p);
    }
}

static const char *data_source_path(char *buf, size_t n) {
#ifdef DG_SOURCE_DIR
    snprintf(buf, n, "%s/assets/world/daynight.json", DG_SOURCE_DIR);
#else
    platform_data_path("assets/world/daynight.json", buf, n);
#endif
    return buf;
}

static bool save_config(void) {
    char *txt = tod_config_to_json(&DN.cfg);
    if (!txt) return false;
    char path[600];
    data_source_path(path, sizeof(path));
    // Keep the per-level overrides of the file on disk.
    char rp[512];
    char *old = platform_read_file(platform_data_path("assets/world/daynight.json", rp, sizeof(rp)), NULL);
    cJSON *oj = old ? cJSON_Parse(old) : NULL;
    free(old);
    cJSON *nj = cJSON_Parse(txt);
    cJSON_free(txt);
    if (oj && nj) {
        cJSON *lv = cJSON_DetachItemFromObject(oj, "levels");
        if (lv) cJSON_AddItemToObject(nj, "levels", lv);
    }
    char *out = nj ? cJSON_Print(nj) : NULL;
    cJSON_Delete(oj);
    cJSON_Delete(nj);
    if (!out) return false;
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) { fputs(out, f); fputc('\n', f); fclose(f); ok = true; }
    char rt[600];
    snprintf(rt, sizeof(rt), "%sassets/world/daynight.json", platform_base_dir());
    if (ok && strcmp(rt, path)) {
        FILE *g = fopen(rt, "wb");
        if (g) { fputs(out, g); fputc('\n', g); fclose(g); }
    }
    cJSON_free(out);
    DN.cfg_mtime = platform_file_mtime(platform_data_path("assets/world/daynight.json", rp, sizeof(rp)));
    log_info(ok ? "daynight: saved %s" : "daynight: cannot write %s", path);
    return ok;
}

// ------------------------------------------------------------------ atmosphere (CPU)

// Transmittance from the ground towards an elevation (same medium as shaders/sky/sky_common.glsl).
static vec3 transmittance_cpu(float elev, float mie_scale) {
    const double RB = 6360.0, RT = 6460.0;
    const double ray[3] = { 5.802e-3, 13.558e-3, 33.1e-3 }, oz[3] = { 0.650e-3, 1.881e-3, 0.085e-3 };
    double r = RB + 0.002, mu = sin(elev);
    // Distance to the top of the atmosphere.
    double b = r * mu, c = r * r - RT * RT;
    double t = -b + sqrt(fmax(b * b - c, 0.0));
    // Blocked by the planet (well below the horizon): nothing.
    double cg = r * r - RB * RB, dg = b * b - cg;
    if (mu < 0 && dg > 0 && -b - sqrt(dg) > 0) return v3(0, 0, 0);
    double od[3] = { 0, 0, 0 };
    const int N = 48;
    double dt = t / N;
    for (int i = 0; i < N; i++) {
        double s = (i + 0.5) * dt;
        double x = s * sqrt(fmax(1.0 - mu * mu, 0.0)), y = r + s * mu;
        double h = sqrt(x * x + y * y) - RB;
        double dr = exp(-fmax(h, 0.0) / 8.0), dm = exp(-fmax(h, 0.0) / 1.2) * mie_scale, doz = fmax(0.0, 1.0 - fabs(h - 25.0) / 15.0);
        for (int k = 0; k < 3; k++) od[k] += (ray[k] * dr + 4.40e-3 * dm + oz[k] * doz) * dt;
    }
    return v3((float)exp(-od[0]), (float)exp(-od[1]), (float)exp(-od[2]));
}

// Light reaching the ground from a body at elevation `elev` (rgb, engine illuminance): the disc
// fades in over its own size around the horizon.
static vec3 body_light(float elev, float illuminance, float mie) {
    float e = fmaxf(elev, -0.02f);
    vec3 t = transmittance_cpu(fmaxf(e, 0.0f), mie);
    float horizon = smoothstepf(-0.015f, 0.01f, elev);
    return v3_scale(t, illuminance * horizon);
}

// ------------------------------------------------------------------ host

void daynight_level_loaded(World *w) {
    const char *lvl = w ? w->level.name : "";
    load_config(lvl);
    if (w && w->authority) {
        tod_state_init(&w->tod, &DN.cfg, w->tick);
        DN.host_last_days = tod_days(&w->tod, w->tick);
        DN.host_init = true;
    }
    DN.key_init = false;
    DN.second_init = false;
    DN.demo = false;
    shadow_sky_level_loaded();
    char b[16];
    tod_format_hours(DN.cfg.start_hours, b, sizeof(b));
    log_info("daynight: '%s' latitude %.1f, day %d, start %s, %.1f min per day", lvl, DN.cfg.latitude_deg, DN.cfg.day_of_year, b, DN.cfg.day_length_minutes);
}

static void fire_event(World *w, u32 bits, float hours) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_TOD;
    ev.target = EVENT_ALL;
    ev.except = EVENT_ALL;
    ev.id = bits;
    ev.value = hours;
    ev_push(w, &ev);
}

void daynight_host_tick(World *w) {
    if (!w || !w->authority) return;
    if (!DN.cfg_loaded) load_config(w->level.name);
    if (!DN.host_init || w->tod.version == 0) {
        tod_state_init(&w->tod, &DN.cfg, w->tick);
        DN.host_last_days = tod_days(&w->tod, w->tick);
        DN.host_init = true;
    }
    double days = tod_days(&w->tod, (double)w->tick);
    u32 ev = tod_events_between(&DN.cfg, DN.host_last_days, days);
    DN.host_last_days = days;
    for (u32 bit = 1; bit <= TOD_EV_NOON; bit <<= 1)
        if (ev & bit) fire_event(w, bit, (float)((days - floor(days)) * 24.0));
    // Demo: one day, then back to the previous speed.
    if (DN.demo && days >= DN.demo_end_days) {
        tod_set_day_length(&w->tod, (double)w->tick, DN.demo_prev_len);
        DN.demo = false;
        log_info("daynight: 24 h demo done");
    }
    // Light schedules: the host switches EF_ON when the schedule changes (a switch can override
    // a lamp until the next change).
    float hours = (float)((days - floor(days)) * 24.0);
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_LIGHT || !w->light[i].scheduled) continue;
        bool on = tod_light_on(&DN.cfg.lamps, hours, (u32)i);
        if (on != w->light[i].sched_state) {
            w->light[i].sched_state = on;
            if (on) w->flags[i] |= EF_ON;
            else w->flags[i] &= ~(u32)EF_ON;
        }
    }
}

// Events on every machine (EV_TOD): console scripts, callbacks, ambience.
void daynight_handle_event(const GameEvent *ev) {
    u32 bits = ev->id;
    for (u32 bit = 1; bit <= TOD_EV_NOON; bit <<= 1) {
        if (!(bits & bit)) continue;
        char b[16];
        tod_format_hours(ev->value, b, sizeof(b));
        log_info("daynight: %s at %s", tod_event_name(bit), b);
        Cvar *script = bit == TOD_EV_SUNRISE ? DN.on_sunrise : bit == TOD_EV_SUNSET ? DN.on_sunset : bit == TOD_EV_MIDNIGHT ? DN.on_midnight : DN.on_noon;
        if (script && script->str[0]) cbuf_add(script->str);
        for (int i = 0; i < DN.cb_count; i++) DN.cbs[i](bit, ev->value, DN.cb_user[i]);
        // Ambient hook: night / day soundscape.
        if (bit == TOD_EV_SUNSET && DN.night_ambience->str[0]) audio_play_music(DN.night_ambience->str);
        if (bit == TOD_EV_SUNRISE && DN.day_ambience->str[0]) audio_play_music(DN.day_ambience->str);
        if (DN.t.mode == 1) {
            int k = bit == TOD_EV_SUNRISE ? 0 : bit == TOD_EV_SUNSET ? 1 : bit == TOD_EV_MIDNIGHT ? 2 : 3;
            DN.t.events[k]++;
        }
    }
}

void daynight_add_listener(TodEventFn fn, void *user) {
    if (DN.cb_count < DN_MAX_CB) { DN.cbs[DN.cb_count] = fn; DN.cb_user[DN.cb_count] = user; DN.cb_count++; }
}

// ------------------------------------------------------------------ gameplay API

static bool sky_now(World *w, TodSky *out) {
    if (!w || !w->tod.version) return false;
    tod_sky(&DN.cfg, tod_days(&w->tod, now_tick(w)), out);
    return true;
}

float world_time_hours(World *w) { return w && w->tod.version ? tod_hours(&w->tod, now_tick(w)) : 12.0f; }
int world_day_number(World *w) { return w && w->tod.version ? (int)tod_days(&w->tod, now_tick(w)) : 0; }
bool world_is_night(World *w) {
    TodSky s;
    return sky_now(w, &s) && tod_is_night(&s);
}
float world_sun_elevation_deg(World *w) {
    TodSky s;
    return sky_now(w, &s) ? s.sun_elev * DG_RAD2DEG : 45.0f;
}

// 0..1 light level at a point: sun / moon (visible from the point, through the atmosphere and
// the clouds) + sky + lamps that are on. For AI (zombies seeing the player), spawn rules, the
// flashlight hint.
float world_light_level(World *w, vec3 pos) {
    TodSky s;
    if (!sky_now(w, &s)) return 1.0f;
    float cover = DN.cloud_cover;
    float loss = 1.0f - DN.cfg.cloud_light_loss * cover;
    vec3 sun = body_light(s.sun_elev, DN.cfg.sun_illuminance * loss, 1.0f);
    vec3 moon = body_light(s.moon_elev, DN.cfg.moon_illuminance * s.moon_illum * loss, 1.0f);
    float ls = (sun.x + sun.y + sun.z) / 3.0f, lm = (moon.x + moon.y + moon.z) / 3.0f;
    float direct = 0;
    vec3 L = ls >= lm ? s.sun_dir : s.moon_dir;
    float key = fmaxf(ls, lm);
    if (key > 1e-5f && w->phys) {
        RayHit rh;
        bool blocked = phys_raycast(w->phys, v3_add(pos, v3(0, 0.1f, 0)), v3_scale(L, 200.0f), PL_MASK_WORLD, 0, &rh);
        direct = blocked ? 0.0f : key * fmaxf(L.y, 0.0f);
    }
    float sky_amb = (ls + lm) * 0.25f + DN.cfg.night_sky_floor * 3.0f;
    float lamps = 0;
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_LIGHT || !(w->flags[i] & EF_ON)) continue;
        float d = v3_dist(pos, w->xf[i].pos);
        float r = w->light[i].radius;
        if (d > r) continue;
        float f = saturate(1.0f - powf(d / r, 4.0f));
        lamps += w->light[i].intensity * f * f / (d * d + 1.0f) * 0.02f;
    }
    float total = direct + sky_amb + lamps;
    return saturate(1.0f - expf(-total / 0.6f));
}

// ------------------------------------------------------------------ per frame

bool daynight_active(World *w) {
    return w && DN.atmosphere && DN.atmosphere->i && w->level.sky && w->tod.version;
}

void daynight_environment(World *w, const RenderCamera *cam, RenderEnvironment *env) {
    poll_reload();
    DN.have_sky = false;
    if (!daynight_active(w)) return;
    double days = tod_days(&w->tod, now_tick(w));
    TodSky s;
    tod_sky(&DN.cfg, days, &s);
    DN.sky = s;
    DN.frame_days = days;
    DN.have_sky = true;
    float h = s.hours;
    // Clouds / fog: the weather (game_weather.c) when the level has one, else the daynight.json
    // curves; the weather_clouds / weather_fog cvars override both (debug).
    float cover = DN.weather_clouds->f >= 0 ? DN.weather_clouds->f : weather_cloud_cover(w, tod_curve(&DN.cfg, TOD_CLOUD_COVER, h));
    DN.cloud_cover = saturate(cover);
    float fog = weather_fog_density(w, tod_curve(&DN.cfg, TOD_FOG_DENSITY, h)) * (DN.weather_fog->f >= 0 ? DN.weather_fog->f : 1.0f);
    WeatherParams wxp;
    float density = weather_current(w, &wxp) ? wxp.v[WP_CLOUD_DENSITY] : 0.6f;
    float mie = 1.0f + fmaxf(0.0f, fog - 0.005f) * 60.0f + DN.cloud_cover * 0.5f;   // dawn mist / overcast haze
    // The sky (and the clouds' own light) under the cover: cloud_light_loss. The direct light is
    // not dimmed by it: the shadow driver attenuates it by the cloud sheet's optical depth
    // (exp(-tau / mu): gone under a thick deck, no floor) and the cloud shadow map adds the patches.
    float sky_loss = fmaxf(1.0f - DN.cfg.cloud_light_loss * DN.cloud_cover * (0.6f + 0.6f * density), 0.0f);
    vec3 sun_atm = body_light(s.sun_elev, DN.cfg.sun_illuminance, mie);
    vec3 moon_atm = v3_mul(body_light(s.moon_elev, DN.cfg.moon_illuminance * s.moon_illum, mie), v3(0.82f, 0.9f, 1.0f));
    float ambient_scale = tod_curve(&DN.cfg, TOD_AMBIENT_SCALE, h);
    ShadowSkyInput sin_;
    memset(&sin_, 0, sizeof(sin_));
    sin_.dt = (float)g_engine.frame_dt;
    sin_.sun_elev = s.sun_elev;
    sin_.moon_elev = s.moon_elev;
    sin_.sun_light = sun_atm;
    sin_.moon_light = moon_atm;
    sin_.sky_up = v3_scale(render_sky_irradiance(v3(0, 1, 0)), ambient_scale);
    sin_.cloud_cover = DN.cloud_cover;
    sin_.cloud_density = density;
    sin_.fog = fog;
    sin_.mie_scale = mie;
    bool wx = weather_current(w, &wxp);
    sin_.precip = wx ? wxp.v[WP_PRECIP] : 0.0f;
    sin_.snow = w->weather.snow;
    sin_.wetness = w->weather.wetness;
    // Cloud drift: the weather wind aloft, else daynight.json's cloud wind (already aloft).
    sin_.wind_dir_deg = wx ? wxp.v[WP_WIND_DIR] : DN.cfg.cloud_wind_dir_deg;
    sin_.wind_speed = wx ? wxp.v[WP_WIND] : -1.0f;
    ShadowSkyOut sso;
    shadow_sky_frame(&sin_, &sso, wx ? 0.0f : DN.cfg.cloud_wind_mps);
    bool moon_key = sso.key_is_moon;
    vec3 key_target = moon_key ? s.moon_dir : s.sun_dir;
    // Stable shadows: the shadowed direction moves in small steps (sub-texel at the shadow map's
    // resolution), coarser for the moon; the sun hands the cascades to the moon once its disc is
    // gone (shadow_sky.c: nothing of the sun is left to change on screen).
    float step_deg = moon_key ? 0.25f : 0.04f;
    if (!DN.key_init || moon_key != DN.key_moon || acosf(clampf(v3_dot(key_target, DN.key_dir), -1.0f, 1.0f)) * DG_RAD2DEG > step_deg) {
        DN.key_dir = key_target;
        DN.key_moon = moon_key;
        DN.key_init = true;
    }
    if (!DN.second_init || acosf(clampf(v3_dot(s.moon_dir, DN.second_dir), -1.0f, 1.0f)) * DG_RAD2DEG > 0.25f) {
        DN.second_dir = s.moon_dir;
        DN.second_init = true;
    }
    // A body below the horizon lights nothing (its direction would light from below).
    vec3 kd = DN.key_dir;
    if (kd.y < 0.02f) kd = v3_norm(v3(kd.x, 0.02f, kd.z));
    env->sun_dir = kd;
    env->sun_color = sso.key.color;
    env->sun_intensity = sso.key.intensity;
    // How much the eye is daylight-adapted (exposure-aware flashes): the sun's share of full noon.
    float ls = moon_key ? 0.0f : sso.key.intensity;
    DN.daylight = smoothstepf(0.0f, 1.0f, ls / fmaxf(DN.cfg.sun_illuminance * 0.35f, 1e-6f));
    // Second light: the moon while the sun owns the cascades, shadowed in atlas tile 3 on the
    // tiers with moon shadows (no unshadowed fill: it lit through roofs and filled every shadow).
    vec3 sd = DN.second_dir;
    env->fill_dir = sd.y > 0.02f ? sd : v3_norm(v3(sd.x, 0.02f, sd.z));
    env->fill_color = sso.second.color;
    env->fill_intensity = moon_key ? 0.0f : sso.second.intensity;
    env->moon_key = moon_key;
    env->shadow_sky = sso;
    env->shadow_sky_valid = true;
    env->ground_albedo = sso.ground_albedo;
    env->atmosphere = true;
    env->sun_true_dir = s.sun_dir;
    env->moon_dir = s.moon_dir;
    env->sun_illuminance = DN.cfg.sun_illuminance * sky_loss;
    env->moon_illuminance = DN.cfg.moon_illuminance * s.moon_illum * sky_loss;
    env->moon_full_illuminance = DN.cfg.moon_illuminance;
    // sky_disc_zoom (inspection): bigger discs, the same surface brightness.
    float zoom = DN.disc_zoom ? DN.disc_zoom->f : 1.0f;
    env->sun_size = tod_curve(&DN.cfg, TOD_SUN_SIZE, h) * zoom;
    env->moon_size = tod_curve(&DN.cfg, TOD_MOON_SIZE, h) * zoom;
    env->moon_full_illuminance *= zoom * zoom;
    env->cel_x = s.cel_x;
    env->cel_y = s.cel_y;
    env->cel_z = s.cel_z;
    env->star_brightness = tod_curve(&DN.cfg, TOD_STAR_BRIGHTNESS, h) * (1.0f - 0.9f * DN.cloud_cover);
    env->milky_way = env->star_brightness * 0.8f;
    env->night_floor = DN.cfg.night_sky_floor;
    env->cloud_cover = DN.cloud_cover;
    env->cloud_altitude = DN.cfg.cloud_altitude_m;
    env->cloud_scale = 3000.0f;
    // Cloud shadow map: Beer-Lambert through the patches; drift at the wind aloft (real seconds).
    env->cloud_shadow_tau = sso.cumulus_tau;
    env->cloud_offset = sso.cloud_offset;
    env->ambient_scale = ambient_scale;
    env->aerial_scale = DN.aerial_scale->f;
    env->aerial_max = DN.aerial_max->f;
    env->mie_scale = mie;
    env->sky_debug = DN.sky_debug->i;
    // Legacy hemispheric ambient (particles, analytic fog colour, the PiP clear): from the SH.
    vec3 up = render_sky_irradiance(v3(0, 1, 0)), down = render_sky_irradiance(v3(0, -1, 0)), side = render_sky_irradiance(v3(1, 0, 0));
    env->sky_ambient = v3_scale(up, 1.0f / DG_PI);
    env->ground_ambient = v3_scale(down, 1.0f / DG_PI);
    env->ambient_intensity = env->ambient_scale;
    env->fog_color = v3_scale(side, 1.0f / DG_PI);
    env->fog_density = fog;
    env->shadow_center = v3_madd(cam->pos, dir_from_angles(cam->yaw, 0), 12.0f);
    weather_environment(w, env);
    TodProbeIn pin = { now_tick(w), days * 86400.0, w->tod.day_length > 0 ? 86400.0 / w->tod.day_length : 0.0, w->tod.day_length, w->tod.version, false, 0.0, 0.0,
                       w->authority, 0.0, 0.0, 0 };
    tod_probe_frame(w, &DN.cfg, &s, env, &pin);
}

float daynight_daylight(void) { return DN.have_sky ? DN.daylight : 1.0f; }

void daynight_ev_range(float *night, float *day) {
    *night = DN.cfg.night_floor_ev;
    *day = DN.cfg.day_max_ev;
    if (*day <= *night + 1.0f) { *night = -1.5f; *day = 7.0f; }
}

void daynight_post(PostSettings *s) {
    World *w = G ? G->world : NULL;
    if (!DN.have_sky || !daynight_active(w)) return;
    float h = DN.sky.hours;
    // Auto exposure follows the cycle: a night floor (never brightened past it: night stays
    // night, moonlight keys the scene), the per-time compensation curve.
    s->ev_min = DN.cfg.night_floor_ev;
    s->ev_max = DN.cfg.day_max_ev;
    s->exposure_comp += tod_curve(&DN.cfg, TOD_EXPOSURE_COMP, h);
    s->exposure_comp += shadow_sky_exposure();   // overcast / snow: metered grey reads darker than it is
    s->exposure_manual = fmaxf(s->exposure_manual, DN.cfg.night_floor_ev);
    // Fog density by time (dawn mist), the volumetrics use the key light (sun / moon).
    float fog = weather_fog_density(w, tod_curve(&DN.cfg, TOD_FOG_DENSITY, h)) * (DN.weather_fog->f >= 0 ? DN.weather_fog->f : 1.0f);
    s->fog_density = fmaxf(fog * 1.5f, 0.001f);
}

// ------------------------------------------------------------------ commands

static World *host_world(const char *cmd) {
    World *w = G ? G->world : NULL;
    if (!w || !w->authority) { log_info("%s: host only", cmd); return NULL; }
    return w;
}

void daynight_set_calendar(World *w, int day_of_year, float hours) {
    if (!w || !w->authority) return;
    DN.cfg.day_of_year = DG_CLAMP(day_of_year, 1, 365);
    w->tod.day_of_year = (u16)DN.cfg.day_of_year;
    float h = hours >= 0 ? hours : tod_hours(&w->tod, (double)w->tick);
    tod_set_time(&w->tod, (double)w->tick, h);   // re-anchors and bumps the version (replicated)
    DN.host_last_days = tod_days(&w->tod, w->tick);
    DN.key_init = false;
}

// Story mode (src/story/story_run.c): host, the time (hours) and the clock rate in game minutes per
// real second (0 = frozen). Only touches the replicated state when something changed.
void daynight_story_drive(World *w, float hours, float minutes_per_second) {
    if (!w || !w->authority) return;
    float len = minutes_per_second > 1e-4f ? 1440.0f / minutes_per_second : 0.0f;
    float now = tod_hours(&w->tod, (double)w->tick);
    float drift = fabsf(now - hours);
    if (drift > 12.0f) drift = 24.0f - drift;
    if (fabsf(w->tod.day_length - len) > fmaxf(0.01f * len, 0.5f)) tod_set_day_length(&w->tod, (double)w->tick, len);
    if (drift * 60.0f > 0.25f) {   // more than 15 game seconds off: snap (scrubs, saves, blends)
        tod_set_time(&w->tod, (double)w->tick, hours);
        DN.host_last_days = tod_days(&w->tod, w->tick);
        DN.key_init = false;
    }
}

static void cmd_time_set(int argc, const char **argv) {
    World *w = host_world("time_set");
    if (!w) return;
    if (argc < 2) { log_info("usage: time_set HH:MM"); return; }
    float h = tod_parse_hours(argv[1]);
    if (h < 0) h = (float)atof(argv[1]);
    tod_set_time(&w->tod, (double)w->tick, h);
    DN.host_last_days = tod_days(&w->tod, w->tick);
    DN.key_init = false;
    char b[16];
    tod_format_hours(h, b, sizeof(b));
    log_info("time_set %s", b);
}

static void cmd_time_scale(int argc, const char **argv) {
    World *w = host_world("time_scale");
    if (!w) return;
    if (argc < 2) { log_info("usage: time_scale <x>  (1 = %.0f min per day, 0 = pause)", DN.cfg.day_length_minutes); return; }
    float k = (float)atof(argv[1]);
    float len = k > 0 ? DN.cfg.day_length_minutes * 60.0f / k : 0.0f;
    tod_set_day_length(&w->tod, (double)w->tick, len);
    log_info("time_scale %.2f: %.1f s per day", k, len);
}

static void cmd_time_pause(int argc, const char **argv) {
    World *w = host_world("time_pause");
    if (!w) return;
    bool pause = argc > 1 ? atoi(argv[1]) != 0 : w->tod.day_length > 0;
    static float resume = 0;
    if (pause) { resume = w->tod.day_length > 0 ? w->tod.day_length : DN.cfg.day_length_minutes * 60.0f; tod_set_day_length(&w->tod, w->tick, 0.0f); }
    else tod_set_day_length(&w->tod, w->tick, resume > 0 ? resume : DN.cfg.day_length_minutes * 60.0f);
    log_info("time %s", pause ? "paused" : "running");
}

static void cmd_time_days(int argc, const char **argv) {
    World *w = host_world("time_days");
    if (!w) return;
    int n = argc > 1 ? atoi(argv[1]) : 1;
    tod_add_days(&w->tod, (double)w->tick, n);
    DN.host_last_days = tod_days(&w->tod, w->tick);
    log_info("time_days %+d: day %d", n, (int)DN.host_last_days);
}

// Shadow reference presets (game_shadow_sky.c): the time when the sun stands at elev_deg
// (evening side), or a full moon night (moon_full: skips days to the next full moon, midnight).
bool daynight_preset_time(World *w, float elev_deg, bool moon_full) {
    if (!w || !w->authority || !w->tod.version) return false;
    double day = floor(tod_days(&w->tod, (double)w->tick));
    TodSky sk;
    if (moon_full) {
        int best = 0;
        float best_illum = -1;
        for (int d = 0; d < 30; d++) {
            tod_sky(&DN.cfg, day + d + 0.999, &sk);
            if (sk.moon_illum > best_illum) { best_illum = sk.moon_illum; best = d; }
        }
        if (best) tod_add_days(&w->tod, (double)w->tick, best);
        tod_set_time(&w->tod, (double)w->tick, 23.99f);
    } else {
        // from noon towards the evening: the first time the sun drops under elev_deg
        float found = 12.0f;
        for (float hh = 12.0f; hh < 23.9f; hh += 1.0f / 120.0f) {
            tod_sky(&DN.cfg, day + hh / 24.0, &sk);
            if (sk.sun_elev * DG_RAD2DEG <= elev_deg) { found = hh; break; }
        }
        tod_set_time(&w->tod, (double)w->tick, found);
    }
    DN.host_last_days = tod_days(&w->tod, w->tick);
    DN.key_init = false;
    DN.second_init = false;
    return true;
}

// The morning sun: from noon backwards, the last time the sun is still above elev_deg.
bool daynight_preset_morning(World *w, float elev_deg) {
    if (!w || !w->authority || !w->tod.version) return false;
    double day = floor(tod_days(&w->tod, (double)w->tick));
    TodSky sk;
    float found = 12.0f;
    for (float hh = 12.0f; hh > 0.1f; hh -= 1.0f / 120.0f) {
        tod_sky(&DN.cfg, day + hh / 24.0, &sk);
        if (sk.sun_elev * DG_RAD2DEG <= elev_deg) { found = hh + 1.0f / 120.0f; break; }
    }
    tod_set_time(&w->tod, (double)w->tick, found);
    DN.host_last_days = tod_days(&w->tod, w->tick);
    DN.key_init = false;
    DN.second_init = false;
    return true;
}

// tod_demo: 24 h in 60 s, then the previous speed.
static void cmd_tod_demo(int argc, const char **argv) {
    World *w = host_world("tod_demo");
    if (!w) return;
    if (argc > 1 && !strcmp(argv[1], "stop")) {
        if (DN.demo) tod_set_day_length(&w->tod, w->tick, DN.demo_prev_len);
        DN.demo = false;
        return;
    }
    float secs = argc > 1 ? (float)atof(argv[1]) : 60.0f;
    if (!DN.demo) DN.demo_prev_len = w->tod.day_length;
    tod_set_day_length(&w->tod, (double)w->tick, fmaxf(secs, 5.0f));
    DN.demo = true;
    DN.demo_end_days = tod_days(&w->tod, w->tick) + 1.0;
    log_info("tod_demo: 24 h in %.0f s", secs);
}

static void cmd_sky_reload(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load_config(DN.level);
    log_info("sky_reload: assets/world/daynight.json");
}

static void cmd_tod_info(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    World *w = G ? G->world : NULL;
    if (!w) return;
    TodSky s;
    if (!sky_now(w, &s)) { log_info("tod: no time state"); return; }
    char b[16];
    tod_format_hours(s.hours, b, sizeof(b));
    log_info("tod: day %d %s | sun elev %.2f az %.2f | moon elev %.2f age %.2f d illum %.2f %s | night %d | light level here %.3f | %s %.1f s/day",
             (int)s.days, b, s.sun_elev * DG_RAD2DEG, s.sun_azim * DG_RAD2DEG, s.moon_elev * DG_RAD2DEG, s.moon_age, s.moon_illum, s.waxing ? "waxing" : "waning",
             tod_is_night(&s), game_local_player() ? world_light_level(w, game_local_player()->sim.move.pos) : -1.0f, w->authority ? "host" : "client",
             w->tod.day_length);
}

// sky_look sun|moon: aim the view at the body.
static void cmd_sky_look(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    TodSky s;
    if (!w || !sky_now(w, &s)) return;
    vec3 d = argc > 1 && !strcmp(argv[1], "sun") ? s.sun_dir : s.moon_dir;
    G->view.yaw = atan2f(-d.x, -d.z);
    G->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
}

static void cmd_tod_editor(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    DN.editor = !DN.editor;
}

bool daynight_editor_open(void) { return DN.editor; }

// tod_test cycle [quit]: 24 h in 60 s; exposure never below the night floor, the IBL luminance
// never jumps between frames, every event once, sky pass timings.
static void cmd_tod_test(int argc, const char **argv) {
    World *w = G ? G->world : NULL;
    const char *what = argc > 1 ? argv[1] : "";
    if (tod_probe_command(argc, argv)) return;   // continuity / trace / shots (game_tod_probe.c)
    if (!strcmp(what, "cycle")) {
        if (!w || !w->authority) { log_info("tod_test: host only"); return; }
        memset(&DN.t, 0, sizeof(DN.t));
        DN.t.mode = 1;
        DN.t.min_ev = 1e9f;
        DN.t.max_ev = -1e9f;
        DN.t.floor_ev = DN.cfg.night_floor_ev;
        DN.t.quit = argc > 2 && !strcmp(argv[2], "quit");
        DN.t.refilters_start = (int)render_sky_refilters();
        tod_set_time(&w->tod, w->tick, 12.1f);
        const char *a[2] = { "tod_demo", "60" };
        cmd_tod_demo(2, a);
        DN.t.start_days = tod_days(&w->tod, w->tick);
        log_info("tod_test cycle: one day in 60 s from noon");
        return;
    }
    if (!strcmp(what, "dump")) {
        // Replication check: the replicated state and the time it gives at fixed server ticks
        // (identical on the host and every client), plus the time shown right now.
        if (!w) return;
        const TimeState *t = &w->tod;
        log_info("tod_test dump: %s v%u ref_tick %u ref_ms %u day %u doy %u len %.3f | hours at tick 100000 %.6f, at 200000 %.6f | now %.4f",
                 w->authority ? "host" : "client", t->version, t->ref_tick, t->ref_ms, t->ref_day, t->day_of_year, t->day_length, tod_hours(t, 100000.0),
                 tod_hours(t, 200000.0), world_time_hours(w));
        return;
    }
    log_info("usage: tod_test cycle [quit] | continuity <seconds> [quit] [tag] | trace <frames> <csv> | shots <frames> <dir> | dump");
}

static void test_frame(void) {
    World *w = G ? G->world : NULL;
    if (DN.t.mode != 1 || !w) return;
    DN.t.frames++;
    if (DN.t.frames < 30) return;   // exposure / environment settle
    float ev = render_exposure_ev();
    DN.t.min_ev = fminf(DN.t.min_ev, ev);
    DN.t.max_ev = fmaxf(DN.t.max_ev, ev);
    float latest = 0;
    float lum = render_sky_luminance(&latest);
    if (DN.t.prev_lum > 0 && lum > 0) {
        // Relative change of the irradiance the lit shaders use, per frame (a refilter pop would
        // show as a spike; the sky itself changes smoothly).
        float jump = fabsf(lum - DN.t.prev_lum) / fmaxf(fmaxf(lum, DN.t.prev_lum), 1e-5f);
        if (jump > DN.t.max_jump) { DN.t.max_jump = jump; DN.t.max_jump_hours = DN.sky.hours; }
    }
    DN.t.prev_lum = lum;
    static const char *names[7] = { "sky_luts", "sky_view", "sky_cshadow", "sky_env", "sky", "fog_apply", "sky_clouds" };
    int n = render_pass_count();
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 7; k++)
            if (!strcmp(render_pass_name(i), names[k])) DN.t.sky_ms[k] += render_pass_ms(i);
    DN.t.perf_frames++;
    double days = tod_days(&w->tod, w->tick);
    if (days >= DN.t.start_days + 1.0 || !DN.demo) {
        float pf = (float)DN.t.perf_frames;
        float sky_ms = (DN.t.sky_ms[1] + DN.t.sky_ms[3] + DN.t.sky_ms[4] + DN.t.sky_ms[6]) / pf, cs_ms = DN.t.sky_ms[2] / pf;
        bool ev_ok = DN.t.min_ev >= DN.t.floor_ev - 0.02f;
        bool ibl_ok = DN.t.max_jump < 0.08f;
        bool ev_count = DN.t.events[0] == 1 && DN.t.events[1] == 1 && DN.t.events[2] == 1 && DN.t.events[3] == 1;
        bool perf_ok = sky_ms <= 0.5f && cs_ms <= 0.2f;
        log_info("tod_test cycle: %d frames | exposure EV %.2f .. %.2f (night floor %.2f) -> %s", DN.t.frames, DN.t.min_ev, DN.t.max_ev, DN.t.floor_ev, ev_ok ? "ok" : "FAIL");
        log_info("tod_test cycle: IBL luminance max frame-to-frame change %.2f %% (at %.2f h), %d refilters -> %s", DN.t.max_jump * 100.0f, DN.t.max_jump_hours,
                 (int)render_sky_refilters() - DN.t.refilters_start, ibl_ok ? "ok" : "FAIL");
        log_info("tod_test cycle: events sunrise %d sunset %d midnight %d noon %d -> %s", DN.t.events[0], DN.t.events[1], DN.t.events[2], DN.t.events[3], ev_count ? "ok" : "FAIL");
        log_info("tod_test cycle: GPU sky view + env + sky %.3f ms (budget 0.5), cloud shadow %.3f ms (budget 0.2), LUTs %.3f ms avg, aerial in fog_apply %.3f ms -> %s",
                 sky_ms, cs_ms, DN.t.sky_ms[0] / pf, DN.t.sky_ms[5] / pf, perf_ok ? "ok" : "over budget");
        log_info("tod_test cycle: per pass: sky_view %.3f  sky_clouds %.3f  sky_env %.3f  sky %.3f ms", DN.t.sky_ms[1] / pf, DN.t.sky_ms[6] / pf, DN.t.sky_ms[3] / pf, DN.t.sky_ms[4] / pf);
        log_info("tod_test cycle: %s", ev_ok && ibl_ok && ev_count && perf_ok ? "PASS" : "FAIL");
        DN.t.mode = 0;
        if (DN.t.quit) cbuf_add("quit");
    }
}

// ------------------------------------------------------------------ UI

static void draw_clock(void) {
    World *w = G ? G->world : NULL;
    if (!DN.clock->i || !DN.have_sky || !w || scene_hides_hud()) return;   // dev scenes and the main menu background hide it
    char b[16], line[160];
    tod_format_hours(DN.sky.hours, b, sizeof(b));
    float e = DN.sky.sun_elev * DG_RAD2DEG;
    const char *phase = e > 6 ? "day" : e > -0.833f ? "golden hour" : e > -6 ? "civil twilight" : e > -12 ? "nautical twilight" : e > -18 ? "astronomical twilight" : "night";
    float speed = w->tod.day_length > 0 ? DN.cfg.day_length_minutes * 60.0f / w->tod.day_length : 0.0f;
    snprintf(line, sizeof(line), "Day %d  %s  %s", (int)DN.frame_days + 1, b, phase);
    float s = ui_scale();
    // Under the compass (top centre): the top right belongs to the kill counter (docs/UI_WHEELS_HUD.md).
    float x = ui_width() * 0.5f, y = fmaxf(10 * s, hud_compass_bottom() + 6 * s);
    if (menus_open()) y = 10 * s;
    ui_text_shadow(x, y, FONT_UI, UI_RGBA(235, 230, 215, 240), UI_ALIGN_CENTER, line);
    if (!G->dev_mode->i) return;
    snprintf(line, sizeof(line), "sun %.1f deg  moon %.0f %%  clouds %.0f %%  EV %.2f  %s", e, DN.sky.moon_illum * 100.0f, DN.cloud_cover * 100.0f,
             render_exposure_ev(), speed <= 0 ? "paused" : (speed > 1.01f ? "fast" : ""));
    ui_text_shadow(x, y + 18 * s, FONT_SMALL, UI_RGBA(200, 200, 190, 220), UI_ALIGN_CENTER, line);
}

static void draw_editor(void) {
    if (!DN.editor) return;
    struct nk_context *ctx = ui_nk();
    World *w = G ? G->world : NULL;
    float s = ui_scale();
    if (nk_begin(ctx, "TIME OF DAY", nk_rect(20 * s, 80 * s, 430 * s, 560 * s), NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) {
        bool host = w && w->authority;
        nk_layout_row_dynamic(ctx, 22 * s, 1);
        char b[64];
        tod_format_hours(DN.have_sky ? DN.sky.hours : 0, b, sizeof(b));
        nk_label(ctx, host ? "Time (scrub: sets the host time)" : "Time (host only)", NK_TEXT_LEFT);
        float hh = DN.have_sky ? DN.sky.hours : 12.0f;
        float before = hh;
        nk_layout_row_dynamic(ctx, 22 * s, 1);
        nk_slider_float(ctx, 0.0f, &hh, 23.99f, 0.05f);
        if (host && fabsf(hh - before) > 1e-4f) tod_set_time(&w->tod, w->tick, hh);
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        snprintf(b + 5, 40, "  day %d", DN.have_sky ? (int)DN.frame_days + 1 : 0);
        nk_label(ctx, b, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 26 * s, 3);
        if (nk_button_label(ctx, "Pause")) cbuf_add("time_pause");
        if (nk_button_label(ctx, "24 h / 60 s")) cbuf_add("tod_demo 60");
        if (nk_button_label(ctx, "Normal")) cbuf_add("tod_demo stop; time_scale 1");
        nk_layout_row_dynamic(ctx, 20 * s, 2);
        nk_property_float(ctx, "latitude", -89.0f, &DN.cfg.latitude_deg, 89.0f, 1.0f, 0.2f);
        float doy = (float)DN.cfg.day_of_year;
        nk_property_float(ctx, "day of year", 1.0f, &doy, 365.0f, 1.0f, 1.0f);
        DN.cfg.day_of_year = (int)doy;
        nk_property_float(ctx, "min per day", 0.5f, &DN.cfg.day_length_minutes, 240.0f, 1.0f, 0.5f);
        nk_property_float(ctx, "moon age", 0.0f, &DN.cfg.moon_age_days, 29.5f, 0.5f, 0.1f);
        nk_property_float(ctx, "night floor EV", -12.0f, &DN.cfg.night_floor_ev, 2.0f, 0.1f, 0.05f);
        nk_property_float(ctx, "sun illuminance", 0.1f, &DN.cfg.sun_illuminance, 20.0f, 0.1f, 0.02f);
        nk_property_float(ctx, "moon illuminance", 0.0f, &DN.cfg.moon_illuminance, 1.0f, 0.005f, 0.001f);
        nk_property_float(ctx, "cloud light loss", 0.0f, &DN.cfg.cloud_light_loss, 1.0f, 0.05f, 0.01f);
        // Curves.
        nk_layout_row_dynamic(ctx, 24 * s, 1);
        const char *names[TOD_CURVE_COUNT];
        for (int i = 0; i < TOD_CURVE_COUNT; i++) names[i] = tod_curve_name(i);
        DN.edit_curve = nk_combo(ctx, names, TOD_CURVE_COUNT, DN.edit_curve, (int)(22 * s), nk_vec2(240 * s, 220 * s));
        TodCurveKeys *k = &DN.cfg.curves[DN.edit_curve];
        float vmax = DN.edit_curve == TOD_FOG_DENSITY ? 0.1f : (DN.edit_curve == TOD_EXPOSURE_COMP ? 3.0f : 4.0f);
        float vmin = DN.edit_curve == TOD_EXPOSURE_COMP ? -3.0f : 0.0f;
        int remove = -1;
        for (int i = 0; i < k->count; i++) {
            nk_layout_row_begin(ctx, NK_DYNAMIC, 22 * s, 3);
            nk_layout_row_push(ctx, 0.42f);
            nk_property_float(ctx, "#h", 0.0f, &k->hour[i], 24.0f, 0.25f, 0.05f);
            nk_layout_row_push(ctx, 0.42f);
            nk_property_float(ctx, "#v", vmin, &k->value[i], vmax, (vmax - vmin) / 100.0f, (vmax - vmin) / 400.0f);
            nk_layout_row_push(ctx, 0.14f);
            if (nk_button_label(ctx, "x")) remove = i;
            nk_layout_row_end(ctx);
        }
        if (remove >= 0 && k->count > 1) {
            for (int i = remove; i < k->count - 1; i++) { k->hour[i] = k->hour[i + 1]; k->value[i] = k->value[i + 1]; }
            k->count--;
        }
        nk_layout_row_dynamic(ctx, 26 * s, 3);
        if (nk_button_label(ctx, "Add key") && k->count < TOD_MAX_CURVE_KEYS) {
            k->hour[k->count] = DN.have_sky ? DN.sky.hours : 12.0f;
            k->value[k->count] = tod_curve(&DN.cfg, (TodCurve)DN.edit_curve, k->hour[k->count]);
            k->count++;
        }
        if (nk_button_label(ctx, "Save")) save_config();
        if (nk_button_label(ctx, "Reload")) load_config(DN.level);
        // Keys stay sorted by hour.
        for (int x = 1; x < k->count; x++)
            for (int y = x; y > 0 && k->hour[y] < k->hour[y - 1]; y--) {
                float th = k->hour[y], tv = k->value[y];
                k->hour[y] = k->hour[y - 1]; k->value[y] = k->value[y - 1];
                k->hour[y - 1] = th; k->value[y - 1] = tv;
            }
        nk_layout_row_dynamic(ctx, 20 * s, 1);
        snprintf(b, sizeof(b), "curve now: %.4f", tod_curve(&DN.cfg, (TodCurve)DN.edit_curve, DN.have_sky ? DN.sky.hours : 12.0f));
        nk_label(ctx, b, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 22 * s, 1);
        if (nk_button_label(ctx, "Close")) DN.editor = false;
    }
    nk_end(ctx);
}

void daynight_draw(void) {
    test_frame();
    draw_clock();
    draw_editor();
}

// ------------------------------------------------------------------ registration

void daynight_register(void) {
    DN.atmosphere = cvar_reg_bool("r_atmosphere", true, CVAR_LOCKED, "physically based sky + day / night cycle on open levels (0 = the fixed gradient sky)");
    DN.clock = cvar_reg_bool("cl_clock", true, CVAR_ARCHIVE | CVAR_EDITABLE, "on-screen clock (time of day, sun elevation, exposure)");
    DN.sky_debug = cvar_reg_int("sky_debug", 0, 0, 6, CVAR_LOCKED, "sky views: 1 transmittance, 2 multiple scattering, 3 sky view, 4 cloud shadow, 5 environment, 6 aerial perspective");
    DN.aerial_scale = cvar_reg_float("sky_aerial_scale", 2.0f, 0.0f, 20.0f, CVAR_LOCKED, "aerial perspective distance multiplier (1 = physical)");
    DN.aerial_max = cvar_reg_float("sky_aerial_max", 3000.0f, 100.0f, 32000.0f, CVAR_LOCKED, "aerial perspective froxel depth (m)");
    DN.weather_clouds = cvar_reg_float("weather_clouds", -1.0f, -1.0f, 1.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "weather hook: cloud cover 0..1 (-1 = the daynight.json curve)");
    DN.weather_fog = cvar_reg_float("weather_fog", -1.0f, -1.0f, 10.0f, CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "weather hook: fog density multiplier (-1 = 1)");
    DN.on_sunrise = cvar_reg_string("tod_on_sunrise", "", CVAR_LOCKED, "console commands run at sunrise");
    DN.on_sunset = cvar_reg_string("tod_on_sunset", "", CVAR_LOCKED, "console commands run at sunset");
    DN.on_midnight = cvar_reg_string("tod_on_midnight", "", CVAR_LOCKED, "console commands run at midnight");
    DN.disc_zoom = cvar_reg_float("sky_disc_zoom", 1.0f, 0.1f, 40.0f, CVAR_LOCKED, "sun / moon disc size multiplier (inspection; same surface brightness)");
    DN.on_noon = cvar_reg_string("tod_on_noon", "", CVAR_LOCKED, "console commands run at noon");
    DN.night_ambience = cvar_reg_string("tod_night_ambience", "", CVAR_LOCKED, "ambient track started at sunset (empty = keep)");
    DN.day_ambience = cvar_reg_string("tod_day_ambience", "", CVAR_LOCKED, "ambient track started at sunrise (empty = keep)");
    cmd_register("time_set", cmd_time_set, "time_set HH:MM (host)");
    cmd_register("time_scale", cmd_time_scale, "time_scale <x>: day speed multiplier, 0 pauses (host)");
    cmd_register("time_pause", cmd_time_pause, "time_pause [0|1] (host)");
    cmd_register("time_days", cmd_time_days, "time_days <n>: skip days (moon phases, host)");
    cmd_register("tod_demo", cmd_tod_demo, "tod_demo [seconds|stop]: 24 h in 60 s (host)");
    cmd_register("sky_reload", cmd_sky_reload, "reload assets/world/daynight.json");
    cmd_register("sky_look", cmd_sky_look, "sky_look sun|moon: aim the view at the sun / moon");
    cmd_register("tod_info", cmd_tod_info, "time of day, sun / moon, light level");
    cmd_register("tod_editor", cmd_tod_editor, "toggle the time-of-day editor");
    cmd_register("tod_test", cmd_tod_test,
                 "tod_test cycle [quit]: exposure floor, IBL continuity, events, sky budget over 24 h in 60 s | continuity <s> [quit] [tag]: per-frame clock / sun / "
                 "light / shadow continuity | trace <frames> <csv> | shots <frames> <dir> | dump");
    load_config("");
}
