// Audio glue (docs/AUDIO_PROCEDURAL.md "Integration"): the world the environment chain listens to.
//   - ray casts for occlusion, room probes and echo scans: the physics world (static + moving bodies,
//     not characters; the listener's own vehicle is skipped), the hit body's surface material
//   - vegetation density: the foliage placements around a point
//   - listener state every frame: in a vehicle, head under water, health, ADS, the weather at the ear
//     (rain, wind, snow cover, temperature, humidity from wetness / rain / fog, sky exposure)
//   - sources the world implies (no events): footsteps from the characters' foot plants and the
//     ground under them, zombie calls (the nearest few each a voice, the rest as crowd loops per
//     direction), water lapping by the listener
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
#include "ui/audio_editor.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/platform.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "audio/synth/syn_material.h"
#include "game_water.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

#define ZOMBIE_VOICES 6       // individual voices; the rest are crowd loops
#define ZOMBIE_TRACK 64
#define CROWD_SECTORS 8

typedef struct ZombieVoice { EntityId ent; double next; bool was_attacking; u32 calls; AudioVoice v; } ZombieVoice;

static struct {
    World *w;
    BodyId ignore;
    Cvar *debug;
    ZombieVoice zv[ZOMBIE_TRACK];
} GA;

static bool ray(vec3 from, vec3 dir_len, EnvRayHit *out, void *user) {
    (void)user;
    // The current world, not GA.w: sounds started before this frame's game_audio_frame (vehicles)
    // right after a map load would otherwise cast into the freed previous world.
    World *w = game_world();
    if (!w || !w->phys) return false;
    RayHit rh;
    if (!phys_raycast(w->phys, from, dir_len, PL_MASK_WORLD, w == GA.w ? GA.ignore : PHYS_INVALID_BODY, &rh)) return false;
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
    audio_editor_init();
}

static void footsteps(World *w, vec3 cam, float dt);
static void zombie_voices(World *w, vec3 cam);
static void water_lap(World *w, vec3 cam);

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
    float dt = (float)g_engine.frame_dt;
    if (dt <= 0 || dt > 0.25f) dt = 1.0f / 60.0f;
    footsteps(w, cam->pos, dt);
    zombie_voices(w, cam->pos);
    water_lap(w, cam->pos);
}

// ------------------------------------------------------------------ implied sources

static int ground_material(World *w, vec3 feet, bool *in_water) {
    *in_water = false;
    float wh = game_water_height(feet.x, feet.z, (double)w->tick);
    if (wh != WATER_NONE && wh > feet.y + 0.05f) { *in_water = true; return SYN_MAT_WATER; }
    RayHit rh;
    int m = SYN_MAT_CONCRETE;
    if (w->phys && phys_raycast(w->phys, v3_add(feet, v3(0, 0.3f, 0)), v3(0, -0.8f, 0), PL_MASK_WORLD, PHYS_INVALID_BODY, &rh)) m = rh.material;
    // Outdoors under snow cover, the ground crunches as snow; wet soft ground squelches as mud.
    WeatherLocal wl = weather_at(w, feet);
    bool ground = m == SYN_MAT_DIRT || m == SYN_MAT_GRASS || m == SYN_MAT_GRAVEL || m == SYN_MAT_SAND || m == SYN_MAT_ASPHALT || m == SYN_MAT_CONCRETE || m == SYN_MAT_DEFAULT;
    if (ground && wl.snow_cover > 0.4f) return SYN_MAT_SNOW;
    if ((m == SYN_MAT_DIRT || m == SYN_MAT_GRASS) && wl.wetness > 0.6f) return SYN_MAT_MUD;
    return m;
}

static void footsteps(World *w, vec3 cam, float dt) {
    CharStep st[32];
    int n = chars_take_steps(w, cam, 40.0f, dt, st, 32);
    const Player *lp = game_local_player();
    for (int i = 0; i < n && i < 12; i++) {
        bool water;
        int mat = ground_material(w, st[i].pos, &water);
        AudioEmit e;
        audio_emit_init(&e, "footstep", v3_add(st[i].pos, v3(0, 0.05f, 0)));
        e.seed = dg_hash_u32(st[i].ent * 2654435761u ^ (u32)w->tick);
        AudioParams p = { 0 };
        audio_param(&p, "material", (float)mat);
        audio_param(&p, "speed", st[i].speed);
        audio_param(&p, "weight", st[i].zombie ? 70.0f : 82.0f);
        audio_param(&p, "crouch", st[i].crouch ? 1.0f : 0.0f);
        audio_param(&p, "water", water ? 1.0f : 0.0f);
        audio_param(&p, "local", lp && lp->ent == st[i].ent ? 1.0f : 0.0f);
        audio_param(&p, "zombie", st[i].zombie ? 1.0f : 0.0f);
        audio_emit(&e, &p);
    }
}

static ZombieVoice *zv_get(EntityId ent) {
    ZombieVoice *free_slot = NULL, *oldest = &GA.zv[0];
    for (int i = 0; i < ZOMBIE_TRACK; i++) {
        if (GA.zv[i].ent == ent) return &GA.zv[i];
        if (!GA.zv[i].ent && !free_slot) free_slot = &GA.zv[i];
        if (GA.zv[i].next < oldest->next) oldest = &GA.zv[i];
    }
    ZombieVoice *z = free_slot ? free_slot : oldest;
    memset(z, 0, sizeof(*z));
    z->ent = ent;
    // First call a little later, staggered per zombie.
    z->next = platform_time() + 0.5 + (double)(dg_hash_u32(ent) & 1023u) / 1023.0 * 3.0;
    return z;
}

static int cmp_dist(const void *a, const void *b) {
    float da = ((const float *)a)[0], db = ((const float *)b)[0];
    return da < db ? -1 : da > db;
}

static void zombie_voices(World *w, vec3 cam) {
    CharVoiceInfo zi[256];
    int n = chars_zombies_near(w, cam, 140.0f, zi, 256);
    struct { float d; int i; } order[256];
    for (int i = 0; i < n; i++) { order[i].d = v3_dist(zi[i].head, cam); order[i].i = i; }
    qsort(order, (size_t)n, sizeof(order[0]), cmp_dist);
    double now = platform_time();
    int sector_n[CROWD_SECTORS] = { 0 };
    vec3 sector_c[CROWD_SECTORS];
    for (int s = 0; s < CROWD_SECTORS; s++) sector_c[s] = v3(0, 0, 0);
    int single = 0;
    for (int k = 0; k < n; k++) {
        const CharVoiceInfo *z = &zi[order[k].i];
        if (z->dead) continue;
        if (single < ZOMBIE_VOICES && order[k].d < 70.0f) {
            single++;
            ZombieVoice *zv = zv_get(z->ent);
            bool attack_start = z->attacking && !zv->was_attacking;
            zv->was_attacking = z->attacking;
            if (!attack_start && now < zv->next) continue;
            // Call: a bark on each attack, growls while running at you, groans when shambling, now and
            // then a scream. Per zombie voice from its id: the same zombie sounds the same everywhere.
            u32 h = dg_hash_u32(z->ent * 2654435761u + zv->calls * 0x9E3779B9u);
            int call = attack_start ? 3 : z->speed > 2.2f ? ((h & 7u) == 0 ? 2 : 1) : 0;
            float gap = call == 0 ? 4.0f + 5.0f * (float)(h >> 24) / 255.0f : 1.8f + 2.2f * (float)(h >> 24) / 255.0f;
            zv->next = now + gap;
            zv->calls++;
            u32 id = dg_hash_u32(z->ent ^ 0x2B0Bu);
            AudioEmit e;
            audio_emit_init(&e, "zombie_voice", z->head);
            e.seed = h;
            AudioParams p = { 0 };
            audio_param(&p, "call", (float)call);
            audio_param(&p, "pitch", 0.8f + 0.45f * (float)(id & 0xFFu) / 255.0f);
            audio_param(&p, "tract", 0.86f + 0.3f * (float)((id >> 8) & 0xFFu) / 255.0f);
            audio_param(&p, "rough", (float)((id >> 16) & 0xFFu) / 255.0f);
            zv->v = audio_emit(&e, &p);
        } else {
            // Clustered by direction: one crowd loop per sector at the members' centroid.
            vec3 d = v3_sub(z->head, cam);
            int s = (int)floorf((atan2f(d.z, d.x) + 3.14159265f) / 6.2831853f * CROWD_SECTORS) % CROWD_SECTORS;
            sector_n[s]++;
            sector_c[s] = v3_add(sector_c[s], z->head);
        }
    }
    for (int s = 0; s < CROWD_SECTORS; s++) {
        if (sector_n[s] < 3) continue;
        AudioEmit e;
        audio_emit_init(&e, "zombie_crowd", v3_scale(sector_c[s], 1.0f / (float)sector_n[s]));
        e.seed = 0xC40Du + (u32)s;
        AudioParams p = { 0 };
        audio_param(&p, "count", (float)sector_n[s]);
        audio_loop_recipe(0xC40D0000u | (u32)s, &e, &p);
    }
}

static void water_lap(World *w, vec3 cam) {
    float wh = game_water_height(cam.x, cam.z, (double)w->tick);
    if (wh == WATER_NONE || cam.y < wh || cam.y > wh + 4.0f) return;
    AudioEmit e;
    audio_emit_init(&e, "water_lap", v3(cam.x, wh, cam.z));
    e.seed = 0x1A9u;
    AudioParams p = { 0 };
    audio_param(&p, "waves", 0.3f);
    audio_loop_recipe(0x1A9001u, &e, &p);
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
