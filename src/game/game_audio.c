// Audio glue (docs/AUDIO_PROCEDURAL.md "Integration"): the world the environment chain listens to.
//   - ray casts for occlusion, room probes and echo scans: the physics world (static + moving bodies,
//     not characters; the listener's own vehicle is skipped), the hit body's surface material
//   - vegetation density: the foliage placements around a point
//   - listener state every frame: in a vehicle, head under water, health, ADS, the weather at the ear
//     (rain, wind, snow cover, temperature, humidity from wetness / rain / fog, sky exposure)
//   - audio_debug: per-voice readout (distance, gain, occlusion bands and walls, zone, echoes), the
//     occlusion / diffraction rays, the listener's room box and probe rays, the last echo scan
#include "game_internal.h"
#include "audio/audio.h"
#include "audio/mixer.h"
#include "audio/env/env_world.h"
#include "render/render.h"
#include "render/debug_draw.h"
#include "ui/ui.h"
#include "ui/gui/gui_draw.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "gameplay/weapons.h"
#include "game_water.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define G (g_engine.game)

static struct {
    World *w;
    BodyId ignore;
    Cvar *debug;
} GA;

static bool ray(vec3 from, vec3 dir_len, EnvRayHit *out, void *user) {
    (void)user;
    if (!GA.w || !GA.w->phys) return false;
    RayHit rh;
    if (!phys_raycast(GA.w->phys, from, dir_len, PL_MASK_WORLD, GA.ignore, &rh)) return false;
    out->hit = true;
    out->dist = rh.distance;
    out->point = rh.point;
    out->normal = rh.normal;
    out->material = rh.material;
    return true;
}

static float foliage(vec3 p, float radius, void *user) {
    (void)user;
    RenderFoliageHit hits[64];
    int n = render_foliage_query(p, radius, hits, 64);
    // ~40 trees / bushes within 25 m read as a dense stand.
    return (float)n / 40.0f;
}

void game_audio_register(void) {
    EnvWorldQuery q = { ray, foliage, NULL };
    env_world_set_query(&q);
    GA.debug = cvar_find("audio_debug");
}

void game_audio_frame(World *w, const RenderCamera *cam, const Player *p, bool ads) {
    GA.w = w;
    GA.ignore = p && G ? weapon_shooter_ignore_body(w, G->local_slot) : PHYS_INVALID_BODY;
    AudioListenerState s;
    memset(&s, 0, sizeof(s));
    s.health = 1.0f;
    if (p) {
        s.in_vehicle = p->sim.move.mode == MOVE_SEATED;
        s.ads = ads;
        const Health *h = &w->health[ENT_INDEX(p->ent)];
        if (h->max_hp > 0) s.health = h->hp / h->max_hp;
    }
    float wh = game_water_height(cam->pos.x, cam->pos.z, (double)w->tick);
    s.underwater = (p && p->sim.move.head_under) || (wh != WATER_NONE && cam->pos.y < wh - 0.05f);
    WeatherLocal wl = weather_at(w, cam->pos);
    s.temp_c = weather_active(w) ? wl.temperature : 15.0f;
    // Humidity: wet air under rain, fog, drying puddles (the weather model has no humidity of its own).
    float wet = fmaxf(fmaxf(wl.rain, wl.wetness * 0.7f), fminf(wl.fog * 4.0f, 1.0f));
    s.humidity = 45.0f + 50.0f * fminf(wet, 1.0f);
    s.rain = wl.rain;
    s.wind = v3_len(wl.wind);
    s.snow = wl.snow_cover;
    s.exposure = wl.exposed;
    audio_set_listener_state(&s);
}

// ------------------------------------------------------------------ audio_debug

static const char *zone_name(int z) { return z == 0 ? "room" : z == 1 ? "other room" : z == 2 ? "outdoor" : "-"; }

void game_audio_draw(vec3 cam_pos) {
    if (!GA.debug) GA.debug = cvar_find("audio_debug");
    int level = GA.debug ? GA.debug->i : 0;
    if (level <= 0) return;
    static AudioVoiceDebug v[MIX_VOICES];
    int n = audio_debug_voices(v, MIX_VOICES);
    const Mixer *m = audio_mixer();
    MixStats ms = m ? mixer_stats(m) : (MixStats){ 0 };
    EnvWorldStats es = env_world_stats();
    float forest, urban;
    env_world_field(&forest, &urban);
    const EnvRoom *room = env_world_room(cam_pos, false);
    float y = ui_height() * 0.12f, x = 12.0f, sz = 13.0f;
    char line[256];
    snprintf(line, sizeof(line), "AUDIO  %d voices (%d virtual)  mixer %.0f / %.0f us  degrade %d  rays %d  probes %d  scans %d", n, ms.virtual_count, ms.render_avg_us, ms.buffer_us,
             ms.degrade, es.rays, es.probes, es.echo_scans);
    gd_rect((GuiRect){ x - 6, y - 4, 860, (float)(n < 24 ? n : 24) * 16.0f + 56.0f }, 0xB0000000u);
    gd_text_simple(x, y, sz, 1, 0xFFFFFFFFu, 0, line);
    y += 17;
    if (room)
        snprintf(line, sizeof(line), "listener: %s  box %.1f x %.1f x %.1f m  RT60 %.2f / %.2f / %.2f s  enclosed %.0f%%  forest %.2f  urban %.2f", room->indoor ? "INDOOR" : "outdoor",
                 room->dims.x, room->dims.y, room->dims.z, room->rt60[0], room->rt60[1], room->rt60[2], room->enclosed * 100.0f, forest, urban);
    else snprintf(line, sizeof(line), "listener: no world");
    gd_text_simple(x, y, sz, 0, 0xFFB0E0FFu, 0, line);
    y += 17;
    for (int i = 0; i < n && i < 24; i++) {
        const EnvVoiceInfo *e = v[i].env;
        if (e)
            snprintf(line, sizeof(line), "%-14s%s%5.1f m  +%3.0f ms  gain %.3f  lp %5.0f  bands %4.0f %4.0f %4.0f  walls %d%s  %s%s  peak %.2f", v[i].recipe, v[i].echo ? "*" : " ", e->dist,
                     e->delay * 1000.0f, e->gain, e->lp_hz, e->band_db[0], e->band_db[1], e->band_db[2], e->walls, e->diffracted ? " (around)" : "", zone_name(e->zone), v[i].virt ? " VIRTUAL" : "",
                     v[i].peak);
        else snprintf(line, sizeof(line), "%-14s  2D  vol %.2f  peak %.2f", v[i].recipe, v[i].volume, v[i].peak);
        gd_text_simple(x, y, sz, 0, v[i].virt ? 0xFF808080u : (v[i].echo ? 0xFF80FFFFu : 0xFFE0E0E0u), 0, line);
        y += 16;
        if (level >= 2 && e) {
            // The direct path: green clear, red through walls, yellow around an edge; echoes cyan.
            u32 col = v[i].echo ? DBG_CYAN : e->walls == 0 ? DBG_GREEN : e->diffracted ? DBG_YELLOW : DBG_RED;
            dbg_line(cam_pos, v[i].pos, col, 0, false);
            dbg_cross(v[i].pos, 0.25f, col, 0);
            dbg_text(v3_add(v[i].pos, v3(0, 0.35f, 0)), v[i].recipe, col, 0.05f, 0);
        }
    }
    if (level >= 3) {
        vec3 o, ends[ENV_ECHO_RAYS];
        bool hits[ENV_ECHO_RAYS];
        int k = env_world_debug_probe(&o, ends, hits, ENV_PROBE_RAYS);
        for (int i = 0; i < k; i++) dbg_line(o, ends[i], hits[i] ? DBG_ORANGE : DBG_GREY, 0, false);
        if (room && room->indoor) dbg_aabb(room->mn, room->mx, DBG_MAGENTA, 0);
        k = env_world_debug_scan(&o, ends, hits, ENV_ECHO_RAYS);
        for (int i = 0; i < k; i++) if (hits[i]) dbg_line(o, ends[i], DBG_BLUE, 0, false);
    }
}
