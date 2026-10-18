// Fabricator chamber presentation (docs/REVIVE.md): glass, liquid, interior.
#include "game_revive.h"
#include "game_internal.h"
#include "core/engine.h"
#include "render/render.h"
#include "gameplay/characters.h"
#include "vfx/vfx.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>

static void draw_cyl(vec3 pos, quat rot, vec3 local, float r, float half_h, Material m) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = game()->meshes.cylinder;
    d.model = mat4_trs(v3_add(pos, quat_rotate(rot, local)), rot, v3(r * 2.0f, half_h * 2.0f, r * 2.0f));
    d.mat = m;
    render_draw(&d);
}

void game_revive_draw_chamber(World *w, int i, vec3 pos, quat rot, const FabView *fv, float pulse) {
    (void)w;
    (void)i;
    bool busy = fv->m.state == FAB_RECONSTRUCTING || fv->m.state == FAB_OPENING;
    // Liquid: fills during the reconstruction, drains while the chamber opens.
    float fill = fv->m.state == FAB_RECONSTRUCTING ? fminf(1.0f, fv->progress * 4.0f) : (fv->m.state == FAB_OPENING ? fmaxf(0.0f, 1.0f - fv->remaining) : 0.0f);
    if (busy && fill > 0.01f) {
        float h = 2.0f * fill;
        Material liq = material_color(v4(0.25f, 0.8f, 0.75f, 0.22f));
        liq.roughness = 0.05f;
        liq.emissive = 0.4f + 0.8f * pulse;
        liq.flags = MAT_NO_SHADOW | MAT_CAUSTICS;
        draw_cyl(pos, rot, v3(0, 0.14f + h * 0.5f, 0), 0.52f, h * 0.5f, liq);
    }
    // Glass (open: slid up into the cap).
    float lift = fv->m.state == FAB_OPENING ? fminf(1.0f, 1.5f - fv->remaining) * 1.9f : 0.0f;
    Material glass = material_color(v4(0.7f, 0.88f, 0.95f, 0.14f));
    glass.roughness = 0.02f;
    glass.metallic = 0.0f;
    glass.flags = MAT_NO_SHADOW | MAT_GLASS;
    draw_cyl(pos, rot, v3(0, 1.19f + lift, 0), 0.56f, 1.04f - lift * 0.5f, glass);
}

// ------------------------------------------------------------------ reconstruction bodies + particles

#define MAX_PHANTOMS 8

typedef struct Phantom {
    EntityId fab, ent;
    int slot;
    float mote_acc, bubble_acc, mist_acc;
    u8 last_state;
} Phantom;

static Phantom g_ph[MAX_PHANTOMS];
static World *g_ph_world;
static Rng g_rng;

// assets/vfx/effects/reconstruction.json "reconstruction" block.
static struct ReconParams {
    float float_height, bob_amp, bob_hz, mote_life, mote_rate[4], bubble_rate, mist_rate, front_jitter, shake, shake_radius;
    bool loaded;
} RP = { 0.18f, 0.025f, 0.2f, 0.55f, { 0, 30, 80, 110 }, 14.0f, 3.0f, 0.7f, 0.5f, 8.0f, false };

static float rjf(const cJSON *o, const char *k, float def) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void load_recon_params(void) {
    RP.loaded = true;
    size_t n = 0;
    char *text = platform_read_file("assets/vfx/effects/reconstruction.json", &n);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    const cJSON *r = root ? cJSON_GetObjectItemCaseSensitive(root, "reconstruction") : NULL;
    if (!r) log_warn("reconstruction: assets/vfx/effects/reconstruction.json missing or invalid, defaults");
    RP.float_height = rjf(r, "float_height", RP.float_height);
    RP.bob_amp = rjf(r, "bob_amplitude", RP.bob_amp);
    RP.bob_hz = rjf(r, "bob_hz", RP.bob_hz);
    RP.mote_life = fmaxf(rjf(r, "mote_life", RP.mote_life), 0.05f);
    const cJSON *mr = r ? cJSON_GetObjectItemCaseSensitive(r, "mote_rate") : NULL;
    for (int k = 0; k < 4 && cJSON_IsArray(mr); k++) {
        const cJSON *v = cJSON_GetArrayItem(mr, k);
        if (cJSON_IsNumber(v)) RP.mote_rate[k] = (float)v->valuedouble;
    }
    RP.bubble_rate = rjf(r, "bubble_rate", RP.bubble_rate);
    RP.mist_rate = rjf(r, "mist_rate", RP.mist_rate);
    RP.front_jitter = rjf(r, "front_jitter", RP.front_jitter);
    RP.shake = rjf(r, "complete_shake", RP.shake);
    RP.shake_radius = rjf(r, "complete_shake_radius", RP.shake_radius);
    cJSON_Delete(root);
    free(text);
}

float game_revive_display_progress(World *w, EntityId fab, int *player_slot) {
    FabView fv;
    if (!revive_fab_view(w, fab, &fv)) return -1.0f;
    float pv = revive_recon_preview();
    if (pv >= 0.0f) {
        // Preview: the first fabricator shows the local player's own character.
        EntityId first = ENT_NONE;
        ENT_FOREACH(w, i) if (w->type[i] == ET_FABRICATOR) { first = ENT_MAKE(i, w->gen[i]); break; }
        if (first == fab) {
            if (player_slot) *player_slot = game() ? game()->local_slot : 0;
            return pv;
        }
    }
    if (fv.m.state != FAB_RECONSTRUCTING) return -1.0f;
    if (player_slot) *player_slot = fv.current_player;
    return fv.progress;
}

static Phantom *phantom_for(EntityId fab, bool create) {
    for (int k = 0; k < MAX_PHANTOMS; k++) if (g_ph[k].fab == fab && fab) return &g_ph[k];
    if (!create) return NULL;
    for (int k = 0; k < MAX_PHANTOMS; k++) if (!g_ph[k].fab) { memset(&g_ph[k], 0, sizeof(g_ph[k])); g_ph[k].fab = fab; return &g_ph[k]; }
    return NULL;
}

static void phantom_free(World *w, Phantom *ph) {
    if (ent_valid(w, ph->ent)) ent_destroy(w, ph->ent);
    memset(ph, 0, sizeof(*ph));
}

static float frand(void) { return rng_float(&g_rng); }

static void spawn_fx(const char *name, vec3 pos, quat rot, float energy) {
    VfxSpawnParams p;
    memset(&p, 0, sizeof(p));
    p.pos = pos;
    p.rot = rot;
    p.normal = v3(0, 1, 0);
    p.scale = 1.0f;
    p.count_mul = 1.0f;
    p.energy = energy;
    p.intensity = 1.0f;
    vfx_game_spawn(name, &p);
}

// Rotation whose -Z points along dir.
static quat look_rot(vec3 dir) {
    vec3 f = v3_norm(dir);
    float yaw = atan2f(-f.x, -f.z);
    float pitch = asinf(clampf(f.y, -1.0f, 1.0f));
    return quat_mul(quat_yaw(yaw), quat_axis_angle(v3(1, 0, 0), pitch));
}

void game_revive_frame(World *w, float dt) {
    if (w != g_ph_world) {
        memset(g_ph, 0, sizeof(g_ph));   // the old world's local entities went with it
        g_ph_world = w;
        g_rng = rng_make(0x5EC0u);
    }
    if (!w) return;
    if (!RP.loaded) load_recon_params();
    Cvar *vq = cvar_find("vfx_quality");
    int quality = vq ? vq->i : 2;   // Low (0): the dissolve only
    // Drop bodies of fabricators that are gone.
    for (int k = 0; k < MAX_PHANTOMS; k++) if (g_ph[k].fab && !ent_valid(w, g_ph[k].fab)) phantom_free(w, &g_ph[k]);
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_FABRICATOR) continue;
        EntityId fab = ENT_MAKE(i, w->gen[i]);
        FabView fv;
        revive_fab_view(w, fab, &fv);
        int slot = -1;
        float p = game_revive_display_progress(w, fab, &slot);
        Phantom *ph = phantom_for(fab, p >= 0.0f);
        vec3 spawn, chamber;
        quat rot;
        revive_fab_socket(w, fab, "spawn_point", &spawn, &rot);
        revive_fab_socket(w, fab, "chamber_center", &chamber, NULL);
        // Completion: burst + a bump of the camera for anyone close (the bass is the fab_done sound).
        if (ph && ph->last_state == FAB_RECONSTRUCTING && fv.m.state == FAB_OPENING) {
            if (quality > 0) spawn_fx("recon_complete", chamber, rot, 1.0f);
            Game *g = game();
            if (g && v3_dist(g->view.eye, chamber) < RP.shake_radius) g->view.land_vel -= RP.shake;
        }
        if (ph) ph->last_state = fv.m.state;
        if (p < 0.0f || slot < 0 || slot >= MAX_PLAYERS) {
            if (ph && ph->ent) {
                if (ent_valid(w, ph->ent)) ent_destroy(w, ph->ent);
                ph->ent = ENT_NONE;
            }
            if (ph && fv.m.state != FAB_OPENING && revive_recon_preview() < 0.0f) phantom_free(w, ph);
            continue;
        }
        // The body floats a little above the chamber floor, breathing up and down.
        float t = (float)g_engine.render_time;
        vec3 feet = v3_add(spawn, v3(0, RP.float_height + RP.bob_amp * sinf(t * RP.bob_hz * 6.2831853f), 0));
        if (!ent_valid(w, ph->ent) || ph->slot != slot) {
            if (ent_valid(w, ph->ent)) ent_destroy(w, ph->ent);
            ph->ent = ent_spawn_local(w, ET_MARKER);
            ph->slot = slot;
            int pi = (int)ENT_INDEX(ph->ent);
            w->vis[pi].shape = VIS_NONE;
            w->xf[pi].pos = w->xf[pi].prev_pos = feet;
            w->xf[pi].rot = w->xf[pi].prev_rot = rot;
            chars_attach_phantom(w, ph->ent, slot);
        }
        int pi = (int)ENT_INDEX(ph->ent);
        w->xf[pi].prev_pos = w->xf[pi].pos;
        w->xf[pi].pos = feet;
        w->xf[pi].rot = rot;
        CharacterInst *c = char_get(w, ph->ent);
        if (c) {
            c->recon = v4(feet.x, feet.y, feet.z, p);
            c->recon_flags = MAT_RECON | (quality > 0 ? MAT_RECON_FX : 0u);
        }
        if (quality <= 0 || p >= 1.0f) continue;
        // Motes from the glass converging on the dissolve front (aimed, life-timed: no attractor needed).
        float front_y = feet.y + 1.8f * saturate(p * 1.75f / 0.95f);
        ph->mote_acc += dt * RP.mote_rate[DG_CLAMP(quality, 0, 3)];
        while (ph->mote_acc >= 1.0f) {
            ph->mote_acc -= 1.0f;
            float a = frand() * 6.2831853f;
            vec3 from = v3(chamber.x + cosf(a) * 0.5f, front_y + (frand() - 0.5f) * RP.front_jitter, chamber.z + sinf(a) * 0.5f);
            vec3 to = v3(feet.x + (frand() - 0.5f) * 0.25f, front_y + (frand() - 0.5f) * 0.12f, feet.z + (frand() - 0.5f) * 0.25f);
            vec3 d = v3_sub(to, from);
            spawn_fx("reconstruction", from, look_rot(d), v3_len(d) / RP.mote_life);   // arrives at the end of its life
        }
        ph->bubble_acc += dt * RP.bubble_rate;
        while (ph->bubble_acc >= 1.0f) {
            ph->bubble_acc -= 1.0f;
            float a = frand() * 6.2831853f, r = sqrtf(frand()) * 0.45f;
            spawn_fx("recon_bubble", v3(chamber.x + cosf(a) * r, spawn.y + 0.16f, chamber.z + sinf(a) * r), quat_identity(), 1.0f);
        }
        ph->mist_acc += dt * RP.mist_rate;
        while (ph->mist_acc >= 1.0f) {
            ph->mist_acc -= 1.0f;
            spawn_fx("recon_mist", v3(chamber.x, spawn.y + 0.2f, chamber.z), quat_identity(), 1.0f);
        }
    }
}
