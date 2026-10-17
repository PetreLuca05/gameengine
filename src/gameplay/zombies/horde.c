#include "horde.h"
#include "gameplay/characters.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "game/game.h"
#include "anim/anim_library.h"
#include "physics/physics.h"
#include "render/debug_draw.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/jobs.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIB (g_engine.anim)
#define HMAX 2048
#define STEER_CHUNK 256

enum { HTARGET_PLAYERS, HTARGET_PLAYER, HTARGET_POINT, HTARGET_NONE };

typedef struct PendingSpawn { vec3 feet; float yaw; u8 cid; } PendingSpawn;

typedef struct Horde {
    HordeConfig cfg;
    int n;                                   // high-water mark of used slots
    EntityId ent[HMAX];
    u8 active[HMAX], alive[HMAX], due[HMAX], tier[HMAX], phys[HMAX], has_target[HMAX], busy[HMAX];
    vec3 pos[HMAX], vel[HMAX], vel_out[HMAX], target[HMAX], avoid[HMAX], wall_n[HMAX];
    float yaw[HMAX], yaw_out[HMAX], speed_max[HMAX], atk_cd[HMAX], atk_hit[HMAX], ground_y[HMAX], wall_t[HMAX], dist[HMAX];
    vec3 ground_at_pos[HMAX];                 // where the ground was last cast (re-cast after 0.35 m)
    i8 target_slot[HMAX];
    int free_list[HMAX], free_n;
    HordeGrid grid;
    PendingSpawn queue[HMAX];
    int q_head, q_count;
    int target_mode, target_player;
    vec3 target_point;
    Cvar *debug, *enable;
    HordeStats st;
    double ms_hist[120];
    int ms_hist_n, ms_hist_head;
    float host_ms_tick;
    // horde_test
    int test_state, test_frames, test_fail, test_checks, test_size;
    bool test_quit;
    double test_ms_sum, test_ms_max;
    int test_ms_n;
    // debug: last casts
    vec3 dbg_ray_a[HMAX], dbg_ray_b[HMAX];
    u8 dbg_ray_hit[HMAX];
    u32 dbg_ray_tick[HMAX];
    // open-world maps (horde_set_water / horde_set_sense): deep water stops zombies like an edge;
    // beyond the sense radius a zombie idles until a player comes near or it gets hurt
    float (*water)(void *user, float x, float z);
    void *water_user;
    float sense;
    u8 aggro[HMAX], wander_set[HMAX], chase[HMAX];
    vec3 wander[HMAX];
} Horde;

static Horde *H;

// Loud noises (horde_noise): heard for HORDE_NOISE_TICKS ticks.
#define HORDE_NOISES 8
#define HORDE_NOISE_TICKS 60
static vec3 s_noise[HORDE_NOISES];
static float s_noise_r[HORDE_NOISES];
static int s_noise_ttl[HORDE_NOISES];

void horde_noise(vec3 pos, float radius) {
    int k = 0;
    for (int n = 1; n < HORDE_NOISES; n++) if (s_noise_ttl[n] < s_noise_ttl[k]) k = n;
    s_noise[k] = pos;
    s_noise_r[k] = radius;
    s_noise_ttl[k] = HORDE_NOISE_TICKS;
}

static u32 hh(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

const HordeStats *horde_stats(void) { return &H->st; }
const HordeConfig *horde_config(void) { return &H->cfg; }
int horde_count(void) { return H ? H->st.count : 0; }

static void load_config(void) {
    size_t n = 0;
    char *text = platform_read_file("assets/ai/zombie.json", &n);
    char err[128] = { 0 };
    if (!text || !horde_config_parse(&H->cfg, text, err, sizeof(err))) {
        horde_config_defaults(&H->cfg);
        log_warn("horde: assets/ai/zombie.json %s, defaults", text ? err : "missing");
    }
    free(text);
}

// ------------------------------------------------------------------ slots

static int slot_alloc(void) {
    if (H->free_n > 0) return H->free_list[--H->free_n];
    if (H->n < H->cfg.capacity && H->n < HMAX) return H->n++;
    return -1;
}

static void slot_free(World *w, int s) {
    if (!H->active[s]) return;
    CharacterInst *c = char_get(w, H->ent[s]);
    if (c) c->horde = 0;
    H->active[s] = H->alive[s] = 0;
    H->ent[s] = 0;
    H->free_list[H->free_n++] = s;
}

static float zombie_speed(const CharacterInst *c, int s) {
    float base = c && c->def ? c->def->move_speed : 1.2f;
    float j = ((float)(hh((u32)s * 7919u + 17u) & 1023u) / 1023.0f - 0.5f) * 2.0f * H->cfg.speed_jitter;
    return base * H->cfg.speed_mult * (1.0f + j);
}

static void register_zombie(World *w, EntityId e, int s) {
    int i = (int)ENT_INDEX(e);
    CharacterInst *c = char_get(w, e);
    H->ent[s] = e;
    H->active[s] = 1;
    H->alive[s] = 1;
    H->pos[s] = w->xf[i].pos;
    H->vel[s] = H->vel_out[s] = v3(0, 0, 0);
    H->yaw[s] = H->yaw_out[s] = quat_get_yaw(w->xf[i].rot);
    H->tier[s] = HT_MID;
    H->phys[s] = 1;
    H->has_target[s] = 0;
    H->aggro[s] = 0;
    H->wander_set[s] = 0;
    H->busy[s] = 0;
    H->target_slot[s] = -1;
    H->avoid[s] = H->wall_n[s] = v3(0, 0, 0);
    H->wall_t[s] = 0;
    H->atk_cd[s] = 0.5f + (float)(hh((u32)s) % 100) * 0.01f;
    H->atk_hit[s] = 0;
    H->ground_y[s] = H->pos[s].y;
    H->ground_at_pos[s] = H->pos[s];
    H->speed_max[s] = zombie_speed(c, s);
    if (c) {
        c->horde = s + 1;
        c->sim_interval = 1;
        c->sim_phase = s;
        // Crowd variety: a random start phase so they don't walk in step.
        if (c->rt.ctrl.def && c->rt.ctrl.def->layer_count > 0) c->rt.ctrl.layer[0].phase = (float)(hh((u32)s * 31u) & 1023u) / 1024.0f;
    }
}

// ------------------------------------------------------------------ spawn

static bool ground_at(World *w, vec3 p, float from_y, vec3 *hit, vec3 *normal) {
    RayHit rh;
    // Level geometry only: props / pickups move (a zombie placed on one would float).
    if (!phys_raycast(w->phys, v3(p.x, from_y, p.z), v3(0, -80.0f, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) return false;
    *hit = rh.point;
    *normal = rh.normal;
    return true;
}

// Valid spawn: ground below, not too steep, the capsule free of geometry (pushed around a few
// times before giving up).
static bool place(World *w, vec3 want, float ref_y, u32 seed, vec3 *out) {
    for (int k = 0; k < 8; k++) {
        vec3 p = want;
        if (k > 0) {
            float a = (float)(hh(seed + (u32)k * 977u) & 4095u) / 4095.0f * 6.2831853f;
            p = v3_add(want, v3(cosf(a) * 0.8f * (float)k, 0, sinf(a) * 0.8f * (float)k));
        }
        vec3 g, n;
        if (!ground_at(w, p, ref_y + 25.0f, &g, &n)) continue;
        if (n.y < cosf(H->cfg.max_slope_deg * DG_DEG2RAD)) continue;
        if (g.y > ref_y + 6.0f) continue;   // on a roof / inside a tall block above the player level
        // The whole footprint supported (no spawning on a board's edge or a post top).
        bool flat = true;
        for (int q = 0; q < 4 && flat; q++) {
            vec3 o = v3(q == 0 ? 0.3f : (q == 1 ? -0.3f : 0), 0, q == 2 ? 0.3f : (q == 3 ? -0.3f : 0));
            vec3 g2, n2;
            if (!ground_at(w, v3_add(g, o), g.y + 1.0f, &g2, &n2) || fabsf(g2.y - g.y) > 0.3f) flat = false;
        }
        if (!flat) continue;
        if (phys_capsule_overlap(w->phys, v3_add(g, v3(0, 0.08f, 0)), 0.32f, 1.7f, PL_MASK_WORLD, PHYS_INVALID_BODY)) continue;
        *out = g;
        return true;
    }
    return false;
}

static u8 character_net_id(const char *name, int k) {
    static const char *const mixed[4] = { "zombie_placeholder", "zombie_runner", "zombie_brute", "zombie_helmet" };
    const char *id = (!name || !name[0] || !strcmp(name, "mixed")) ? mixed[(hh((u32)k) >> 8) % 4] : name;
    const CharacterDef *d = LIB ? anim_lib_char_by_id(LIB, id) : NULL;
    if (!d && LIB) d = anim_lib_char_by_id(LIB, "zombie_placeholder");
    return d ? d->net_id : 2;
}

int horde_spawn(World *w, vec3 center, int count, float radius, int mode, const char *character) {
    if (!H || !w || !w->authority) return 0;
    int queued = 0;
    u32 seed = hh((u32)w->tick * 2654435761u ^ (u32)count);
    vec3 cluster = v3_add(center, v3(radius, 0, 0));
    for (int k = 0; k < count && H->q_count < HMAX; k++) {
        u32 s = hh(seed + (u32)k * 0x9e3779b9u);
        vec3 want;
        if (mode == HSPAWN_RANDOM) {
            float a = (float)(s & 4095u) / 4095.0f * 6.2831853f, r = radius * sqrtf((float)((s >> 12) & 4095u) / 4095.0f);
            want = v3_add(center, v3(cosf(a) * r, 0, sinf(a) * r));
        } else if (mode == HSPAWN_CLUSTER) {
            float a = (float)(s & 4095u) / 4095.0f * 6.2831853f, r = 6.0f * sqrtf((float)((s >> 12) & 4095u) / 4095.0f);
            want = v3_add(cluster, v3(cosf(a) * r, 0, sinf(a) * r));
        } else {
            // Ring: even spacing, a little depth jitter (several rows for big counts).
            float a = (float)k / (float)count * 6.2831853f;
            float r = radius + ((float)((s >> 12) & 1023u) / 1023.0f - 0.5f) * fminf(6.0f, 2.0f + (float)count / 100.0f);
            want = v3_add(center, v3(cosf(a) * r, 0, sinf(a) * r));
        }
        vec3 feet;
        if (!place(w, want, center.y, s, &feet)) { H->st.failed_spawns++; continue; }
        PendingSpawn *p = &H->queue[(H->q_head + H->q_count) % HMAX];
        p->feet = feet;
        vec3 to = v3_sub(center, feet);
        p->yaw = atan2f(-to.x, -to.z);
        p->cid = character_net_id(character, k);
        H->q_count++;
        queued++;
    }
    return queued;
}

bool horde_spawn_at(World *w, vec3 feet, float yaw, const char *character) {
    if (!H || !w || !w->authority || H->q_count >= HMAX) return false;
    RayHit rh;
    if (!phys_raycast(w->phys, v3_add(feet, v3(0, 0.6f, 0)), v3(0, -1.6f, 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) return false;
    if (phys_capsule_overlap(w->phys, v3_add(rh.point, v3(0, 0.08f, 0)), 0.32f, 1.7f, PL_MASK_WORLD, PHYS_INVALID_BODY)) return false;
    PendingSpawn *p = &H->queue[(H->q_head + H->q_count) % HMAX];
    p->feet = rh.point;
    p->yaw = yaw;
    p->cid = character_net_id(character, (int)(w->tick + (u32)H->q_count));
    H->q_count++;
    return true;
}

void horde_set_water(float (*fn)(void *user, float x, float z), void *user) {
    if (!H) return;
    H->water = fn;
    H->water_user = user;
}

void horde_set_sense(float radius) {
    if (H) H->sense = radius;
}

int horde_aggro_count(void) {
    int n = 0;
    for (int s = 0; H && s < H->n; s++) n += H->active[s] && H->alive[s] && (H->sense <= 0 || H->aggro[s]);
    return n;
}

void horde_despawn(World *w) {
    if (!H || !w) return;
    for (int s = 0; s < H->n; s++) {
        if (!H->active[s]) continue;
        EntityId e = H->ent[s];
        slot_free(w, s);
        if (ent_valid(w, e)) ent_destroy(w, e);
    }
    H->q_count = 0;
}

void horde_level_reset(void) {
    if (!H) return;
    H->n = 0;
    H->free_n = 0;
    H->q_count = 0;
    memset(H->active, 0, sizeof(H->active));
    memset(H->alive, 0, sizeof(H->alive));
}

// ------------------------------------------------------------------ tick

typedef struct SteerJob { HordeSteerIn in; HordeSteerOut out; } SteerJob;
static void steer_range(void *u, int begin, int end) {
    SteerJob *j = (SteerJob *)u;
    horde_steer_range(&j->in, &j->out, begin, end);
}

static bool path_stub(void *ctx, vec3 from, vec3 to, vec3 *next) {
    // Pathfinder hook (navmesh later): straight line for now.
    (void)ctx; (void)from; (void)to; (void)next;
    return false;
}

void horde_host_tick(World *w, float dt) {
    if (!H || !w || !w->authority || !H->enable->i) return;
    double t0 = platform_time();
    const HordeConfig *cfg = &H->cfg;
    HordeStats *st = &H->st;
    for (int n = 0; n < HORDE_NOISES; n++) if (s_noise_ttl[n] > 0) s_noise_ttl[n]--;
    // Staggered spawning.
    for (int k = 0; k < cfg->spawn_per_tick && H->q_count > 0; k++) {
        PendingSpawn p = H->queue[H->q_head];
        H->q_head = (H->q_head + 1) % HMAX;
        H->q_count--;
        int s = slot_alloc();
        if (s < 0) { H->q_count = 0; log_warn("horde: capacity %d reached", cfg->capacity); break; }
        EntityId e = spawn_zombie(w, p.feet, p.yaw, p.cid);
        if (!e || !char_get(w, e)) {
            if (e) ent_destroy(w, e);
            H->free_list[H->free_n++] = s;
            st->failed_spawns++;
            continue;
        }
        register_zombie(w, e, s);
    }
    // Players (targets).
    vec3 ppos[MAX_PLAYERS];
    bool pfiring[MAX_PLAYERS];
    EntityId pent[MAX_PLAYERS];
    int pslot[MAX_PLAYERS], np = 0, np_count = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (!p->active || !ent_valid(w, p->ent) || p->sim.move.mode == MOVE_DEAD) continue;
        ppos[np] = p->sim.move.pos;
        pfiring[np] = (w->flags[ENT_INDEX(p->ent)] & EF_FIRING) != 0;
        pent[np] = p->ent;
        pslot[np] = s;
        np++;
    }
    np_count = np;
    // Sync with the world, tiers, targets.
    memset(st->tier_count, 0, sizeof(st->tier_count));
    st->count = st->bodies = st->steered = st->casts = st->ground_casts = 0;
    for (int s = 0; s < H->n; s++) {
        if (!H->active[s]) continue;
        EntityId e = H->ent[s];
        CharacterInst *c = ent_valid(w, e) ? char_get(w, e) : NULL;
        if (!c) { slot_free(w, s); continue; }
        int i = (int)ENT_INDEX(e);
        st->count++;
        bool dead = (w->flags[i] & EF_DEAD) || c->dead;
        H->alive[s] = !dead;
        H->pos[s] = w->xf[i].pos;
        float best = 1e9f;
        int bi = -1;
        for (int k = 0; k < np; k++) {
            float dx = ppos[k].x - H->pos[s].x, dz = ppos[k].z - H->pos[s].z;
            float d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; bi = k; }
        }
        H->dist[s] = bi >= 0 ? sqrtf(best) : 1e9f;
        H->tier[s] = (u8)horde_tier_for(cfg, H->tier[s], H->dist[s]);
        st->tier_count[H->tier[s]]++;
        // Targets: the nearest player, re-picked every retarget_ticks (staggered).
        if (((w->tick + (u32)s) % (u32)cfg->retarget_ticks) == 0 || H->target_slot[s] < 0) H->target_slot[s] = (i8)(bi >= 0 ? pslot[bi] : -1);
        H->has_target[s] = 0;
        H->chase[s] = 0;
        if (H->target_mode == HTARGET_NONE) {
        } else if (H->target_mode == HTARGET_POINT) {
            H->target[s] = H->target_point;
            H->has_target[s] = 1;
        } else {
            int ts = H->target_mode == HTARGET_PLAYER ? H->target_player : H->target_slot[s];
            if (ts >= 0 && ts < MAX_PLAYERS && w->players[ts].active && w->players[ts].sim.move.mode != MOVE_DEAD) {
                H->target[s] = w->players[ts].sim.move.pos;
                H->has_target[s] = 1;
                H->chase[s] = 1;
            }
            // Sense radius (open-world maps): chase once a player is near, shooting within 3x the
            // radius or after being hurt; give up far away; otherwise wander around (a new point
            // within 8 m every 6 s, staggered per zombie).
            if (H->sense > 0 && H->target_mode == HTARGET_PLAYERS) {
                bool shot = bi >= 0 && pfiring[bi] && H->dist[s] < H->sense * 3.0f;
                for (int n = 0; n < HORDE_NOISES && !shot; n++)
                    shot = s_noise_ttl[n] > 0 && v3_dist(s_noise[n], H->pos[s]) < s_noise_r[n];
                if (H->dist[s] < H->sense || shot || w->health[i].hp < w->health[i].max_hp) H->aggro[s] = 1;
                else if (H->dist[s] > H->sense * 2.5f) H->aggro[s] = 0;
                if (!H->aggro[s]) {
                    u32 slot_t = (w->tick + (u32)s * 37u) / 360u, hsh = hh((u32)s * 2654435761u ^ slot_t);
                    float a = (float)(hsh & 1023u) / 1023.0f * 6.2831853f, r = (float)((hsh >> 10) & 1023u) / 1023.0f * 8.0f;
                    if (((w->tick + (u32)s * 37u) % 360u) == 0 || H->wander_set[s] == 0) {
                        H->wander[s] = v3_add(H->pos[s], v3(cosf(a) * r, 0, sinf(a) * r));
                        H->wander_set[s] = 1;
                    }
                    H->target[s] = H->wander[s];
                    H->has_target[s] = (hsh >> 20) % 3 != 0;   // a third of the time it just stands
                    H->chase[s] = 0;
                }
            }
        }
        // Attacks keep the zombie standing.
        if (c->ai.attack_t > 0) c->ai.attack_t -= dt;
        H->busy[s] = c->ai.attack_t > 0 || dead;
        H->speed_max[s] = zombie_speed(c, s) * ((c->limb & LIMB_LEGS) ? 0.45f : 1.0f);
        H->due[s] = H->alive[s] && ((w->tick + (u32)s) % (u32)cfg->tier_interval[H->tier[s]]) == 0;
        c->sim_interval = cfg->anim_interval[H->tier[s]];
    }
    double t1 = platform_time();
    st->ms_sync = (float)((t1 - t0) * 1000.0);
    st->players = np;
    hgrid_build(&H->grid, H->pos, H->alive, H->n);
    double t2 = platform_time();
    // Steering: parallel chunks, each zombie writes only its own outputs.
    {
        HordeSteerIn in;
        memset(&in, 0, sizeof(in));
        in.count = H->n;
        in.pos = H->pos;
        in.vel = H->vel;
        in.yaw = H->yaw;
        in.speed_max = H->speed_max;
        in.target = H->target;
        in.has_target = H->has_target;
        in.chase = H->chase;
        in.tier = H->tier;
        in.active = H->due;
        in.busy = H->busy;
        in.avoid = H->avoid;
        in.grid = &H->grid;
        in.cfg = cfg;
        in.path = path_stub;
        in.dt = dt;
        HordeSteerOut out = { H->vel_out, H->yaw_out };
        SteerJob j = { in, out };
        Cvar *pj = cvar_find("horde_jobs");
        if (!pj || pj->i) jobs_parallel_for(H->n, STEER_CHUNK / 2, steer_range, &j);
        else steer_range(&j, 0, H->n);
        for (int s = 0; s < H->n; s++) {
            if (!H->due[s]) continue;
            H->vel[s] = H->vel_out[s];
            H->yaw[s] = H->yaw_out[s];
            st->steered++;
        }
    }
    double t3 = platform_time();
    // Wall casts, round-robin (each zombie every cast_interval ticks, near / mid only).
    for (int s = 0; s < H->n; s++) {
        if (!H->alive[s] || H->tier[s] > HT_MID) { H->avoid[s] = v3(0, 0, 0); continue; }
        if (((w->tick + (u32)s * 7u) % (u32)cfg->cast_interval) != 0) continue;
        vec3 v = H->vel[s];
        float sp = sqrtf(v.x * v.x + v.z * v.z);
        if (sp < 0.1f) { H->avoid[s] = v3_scale(H->avoid[s], 0.5f); continue; }
        vec3 dir = v3(v.x / sp, 0, v.z / sp);
        vec3 a = v3_add(H->pos[s], v3(0, 0.9f, 0));
        float len = cfg->cast_len + sp * 0.3f;
        ShapeHit rh;
        st->casts++;
        // A body-wide sweep (a thin ray lets separation slide the shoulders past corners).
        a = v3_add(H->pos[s], v3(0, 0.75f, 0));
        bool hit = phys_sphere_cast(w->phys, a, v3_scale(dir, len), 0.3f, PL_MASK(PL_STATIC) | PL_MASK(PL_MOVING), PHYS_INVALID_BODY, &rh);
        H->dbg_ray_a[s] = a;
        H->dbg_ray_b[s] = hit ? rh.point : v3_madd(a, dir, len);
        H->dbg_ray_hit[s] = hit;
        H->dbg_ray_tick[s] = w->tick;
        if (hit) {
            vec3 n = v3(rh.normal.x, 0, rh.normal.z);
            float nl = sqrtf(n.x * n.x + n.z * n.z);
            if (nl > 1e-3f) {
                n = v3_scale(n, 1.0f / nl);
                // Push off the wall and slide along it (towards the side the target is on).
                vec3 side = v3(-n.z, 0, n.x);
                if (H->has_target[s] && v3_dot(side, v3_sub(H->target[s], H->pos[s])) < 0) side = v3_neg(side);
                float k = 1.0f - fminf(1.0f, rh.distance / len);
                H->avoid[s] = v3_add(v3_scale(n, 0.6f * k + 0.2f), v3_scale(side, 0.8f));
                H->wall_n[s] = n;
                H->wall_t[s] = 0.5f;
            }
        } else {
            H->avoid[s] = v3_scale(H->avoid[s], 0.4f);
        }
    }
    double t4 = platform_time();
    // Mover: every tick (cheap), dormant zombies once a second. Ground from a cached height,
    // re-cast every ground_interval ticks (more often near players); wall slide; edges stop.
    for (int s = 0; s < H->n; s++) {
        if (!H->active[s]) continue;
        EntityId e = H->ent[s];
        int i = (int)ENT_INDEX(e);
        CharacterInst *c = char_get(w, e);
        if (!c) continue;
        if (!H->alive[s]) { H->vel[s] = v3(0, 0, 0); continue; }
        int tier = H->tier[s];
        float mdt = dt;
        if (tier == HT_DORMANT) {
            if (!H->due[s]) continue;
            mdt = dt * (float)cfg->tier_interval[HT_DORMANT];
        }
        vec3 p = H->pos[s];
        vec3 v = H->vel[s];
        if (H->wall_t[s] > 0) {
            H->wall_t[s] -= mdt;
            float into = v3_dot(v, H->wall_n[s]);
            if (into < 0) v = v3_sub(v, v3_scale(H->wall_n[s], into));
        }
        vec3 np = v3_add(p, v3_scale(v, mdt));
        int gi = tier == HT_NEAR ? DG_MAX(1, cfg->ground_interval / 2) : cfg->ground_interval * (tier == HT_FAR ? 2 : 1);
        bool moved = fabsf(v.x) + fabsf(v.z) > 0.01f;
        float dgx = np.x - H->ground_at_pos[s].x, dgz = np.z - H->ground_at_pos[s].z;
        bool far_from_cast = dgx * dgx + dgz * dgz > 0.35f * 0.35f;   // off an edge / onto a step: never float
        if (moved && (far_from_cast || ((w->tick + (u32)s * 3u) % (u32)gi) == 0 || tier == HT_DORMANT)) {
            H->ground_at_pos[s] = np;
            vec3 g, n;
            st->ground_casts++;
            RayHit rh;
            if (phys_raycast(w->phys, v3_add(np, v3(0, cfg->step_height + 0.4f, 0)), v3(0, -(cfg->step_height + 2.4f), 0), PL_MASK(PL_STATIC), PHYS_INVALID_BODY, &rh)) {
                g = rh.point;
                n = rh.normal;
                bool step_ok = g.y - p.y <= cfg->step_height + 0.05f;
                if (H->water && H->water(H->water_user, np.x, np.z) > g.y + 1.0f) step_ok = false;   // deep water: an edge
                bool slope_ok = n.y >= cosf(cfg->max_slope_deg * DG_DEG2RAD);
                if (step_ok && slope_ok) H->ground_y[s] = g.y;
                else { np = p; H->vel[s] = v3_scale(v, -0.2f); }
            } else {
                // Edge / hole: don't walk off it.
                np = p;
                H->vel[s] = v3_scale(v, -0.3f);
            }
        }
        np.y = H->ground_y[s];
        // Never inside a player: they crowd up to arm's length (the attack range does the rest).
        for (int k = 0; k < np_count; k++) {
            float dx = np.x - ppos[k].x, dz = np.z - ppos[k].z;
            float d2 = dx * dx + dz * dz, minr = 0.62f;
            if (d2 < minr * minr) {
                float d = sqrtf(d2);
                if (d < 1e-3f) { dx = sinf(H->yaw[s]); dz = cosf(H->yaw[s]); d = 1.0f; }
                np.x = ppos[k].x + dx / d * minr;
                np.z = ppos[k].z + dz / d * minr;
            }
        }
        H->pos[s] = np;
        w->xf[i].pos = np;
        w->xf[i].rot = quat_yaw(H->yaw[s]);
        c->ai.yaw = H->yaw[s];
        c->ai.speed = sqrtf(H->vel[s].x * H->vel[s].x + H->vel[s].z * H->vel[s].z);
        c->ai.mode = ZM_WANDER;
    }
    double t5 = platform_time();
    // Physics tier (+ lag-compensation history) and attacks.
    for (int s = 0; s < H->n; s++) {
        if (!H->active[s]) continue;
        EntityId e = H->ent[s];
        int i = (int)ENT_INDEX(e);
        CharacterInst *c = char_get(w, e);
        if (!c) continue;
        float d = H->dist[s];
        bool want = H->alive[s] && (H->phys[s] ? d < cfg->physics_dist + cfg->physics_hyst : d < cfg->physics_dist);
        BodyId b = w->body[i].body;
        if (b != PHYS_INVALID_BODY && want != (H->phys[s] != 0)) {
            // Promote: place the capsule first (a kinematic move from far away would sweep props).
            if (want) phys_set_transform(w->phys, b, v3_add(H->pos[s], v3(0, 0.9f, 0)), quat_identity(), false);
            phys_set_body_enabled(w->phys, b, want);
        }
        H->phys[s] = want;
        if (want) {
            st->bodies++;
            chars_ensure_history(c);
            if (b != PHYS_INVALID_BODY) phys_move_kinematic(w->phys, b, v3_add(H->pos[s], v3(0, 0.9f, 0)), quat_identity(), dt);
        }
        // Attack: in range of the target player, roughly facing it, off cooldown.
        if (H->atk_cd[s] > 0) H->atk_cd[s] -= dt;
        int ts = H->target_slot[s];
        if (H->target_mode == HTARGET_PLAYER) ts = H->target_player;
        Player *tp = (ts >= 0 && ts < MAX_PLAYERS && w->players[ts].active) ? &w->players[ts] : NULL;
        if (H->atk_hit[s] > 0) {
            H->atk_hit[s] -= dt;
            if (H->atk_hit[s] <= 0 && tp && H->alive[s] && tp->sim.move.mode != MOVE_DEAD) {
                vec3 to = v3_sub(tp->sim.move.pos, H->pos[s]);
                if (to.x * to.x + to.z * to.z < (cfg->attack_range + 0.45f) * (cfg->attack_range + 0.45f) && fabsf(to.y) < 1.6f) {
                    vec3 dir = v3_norm(v3(to.x, 0, to.z));
                    apply_damage(w, tp->ent, cfg->attack_damage, DMG_MELEE, e, v3_add(tp->sim.move.pos, v3(0, 1.3f, 0)), dir, false);
                    st->hits++;
                }
            }
        }
        if (H->target_mode != HTARGET_PLAYERS && H->target_mode != HTARGET_PLAYER) continue;
        if (!tp || !H->alive[s] || H->tier[s] != HT_NEAR || H->atk_cd[s] > 0 || c->ai.attack_t > 0 || tp->sim.move.mode == MOVE_DEAD) continue;
        vec3 to = v3_sub(tp->sim.move.pos, H->pos[s]);
        float d2 = to.x * to.x + to.z * to.z;
        if (d2 > cfg->attack_range * cfg->attack_range || fabsf(to.y) > 1.6f) continue;
        float face = atan2f(-to.x, -to.z);
        float diff = fabsf(wrap_angle(face - H->yaw[s]));
        if (diff > cfg->attack_facing_deg * DG_DEG2RAD) continue;
        float hit = chars_npc_melee(w, c);   // the chosen clip's contact time
        H->atk_hit[s] = hit > 0 ? hit : cfg->attack_hit_time;
        H->atk_cd[s] = cfg->attack_cooldown * (0.85f + 0.3f * (float)(hh((u32)s + w->tick) & 255u) / 255.0f);
        st->attacks++;
    }
    double t6 = platform_time();
    st->pending = H->q_count;
    st->ms_grid = (float)((t2 - t1) * 1000.0);
    st->ms_steer = (float)((t3 - t2) * 1000.0);
    st->ms_casts = (float)((t4 - t3) * 1000.0);
    st->ms_move = (float)((t5 - t4) * 1000.0);
    st->ms_phys = (float)((t6 - t5) * 1000.0);
    st->ms_total = (float)((t6 - t0) * 1000.0);
}

static void test_tick(World *w);

void horde_post_tick(World *w) {
    if (!H || !w || !w->authority) return;
    int evals = 0;
    float ms = 0;
    chars_host_stats(&evals, &ms);
    HordeStats *st = &H->st;
    st->anim_evals = evals;
    st->ms_anim = ms;
    double total = st->ms_total + ms;
    H->ms_hist[H->ms_hist_head] = total;
    H->ms_hist_head = (H->ms_hist_head + 1) % 120;
    if (H->ms_hist_n < 120) H->ms_hist_n++;
    double sum = 0, mx = 0;
    for (int k = 0; k < H->ms_hist_n; k++) { sum += H->ms_hist[k]; if (H->ms_hist[k] > mx) mx = H->ms_hist[k]; }
    st->ms_avg = (float)(sum / DG_MAX(1, H->ms_hist_n));
    st->ms_max = (float)mx;
    test_tick(w);
}

// ------------------------------------------------------------------ debug

void horde_debug_draw(World *w) {
    if (!H || !H->debug->i || !w) return;
    static const u32 tc[HT_COUNT] = { DBG_RED, DBG_YELLOW, DBG_GREEN, DBG_GREY };
    for (int s = 0; s < H->n; s++) {
        if (!H->active[s]) continue;
        vec3 p = H->pos[s];
        u32 col = H->alive[s] ? tc[H->tier[s]] : DBG_WHITE;
        dbg_line(p, v3_add(p, v3(0, 2.0f, 0)), col, 0, false);
        if (H->debug->i >= 1) dbg_line(v3_add(p, v3(0, 0.1f, 0)), v3_add(p, v3_add(v3(0, 0.1f, 0), H->vel[s])), DBG_CYAN, 0, false);
        if (H->phys[s]) dbg_circle(v3_add(p, v3(0, 0.05f, 0)), v3(0, 1, 0), 0.3f, DBG_MAGENTA, 0);
        if (H->debug->i >= 2 && w->tick - H->dbg_ray_tick[s] < 8)
            dbg_line(H->dbg_ray_a[s], H->dbg_ray_b[s], H->dbg_ray_hit[s] ? DBG_ORANGE : DBG_BLUE, 0, true);
        if (H->debug->i >= 3) {
            float cs = H->grid.cell;
            float x0 = floorf(p.x / cs) * cs, z0 = floorf(p.z / cs) * cs, y = p.y + 0.03f;
            dbg_line(v3(x0, y, z0), v3(x0 + cs, y, z0), DBG_GREY, 0, false);
            dbg_line(v3(x0 + cs, y, z0), v3(x0 + cs, y, z0 + cs), DBG_GREY, 0, false);
            dbg_line(v3(x0 + cs, y, z0 + cs), v3(x0, y, z0 + cs), DBG_GREY, 0, false);
            dbg_line(v3(x0, y, z0 + cs), v3(x0, y, z0), DBG_GREY, 0, false);
        }
    }
}

// ------------------------------------------------------------------ commands

static Player *local_player(World **wo) {
    World *w = game_world();
    *wo = w;
    return game_local_player();
}

static void cmd_spawn_horde(int argc, const char **argv) {
    World *w;
    Player *p = local_player(&w);
    if (!w || !w->authority || !p) { log_warn("spawn_horde: host only"); return; }
    int count = argc > 1 ? atoi(argv[1]) : 100;
    float radius = argc > 2 ? (float)atof(argv[2]) : H->cfg.spawn_radius;
    int mode = HSPAWN_RING;
    if (argc > 3 && !strcmp(argv[3], "random")) mode = HSPAWN_RANDOM;
    if (argc > 3 && !strcmp(argv[3], "cluster")) mode = HSPAWN_CLUSTER;
    const char *ch = argc > 4 ? argv[4] : "mixed";
    int q = horde_spawn(w, p->sim.move.pos, DG_CLAMP(count, 1, HMAX), radius, mode, ch);
    log_info("spawn_horde: %d queued (%d per tick), %d positions rejected", q, H->cfg.spawn_per_tick, DG_MAX(0, count - q));
}

static void cmd_despawn_horde(int argc, const char **argv) {
    (void)argc; (void)argv;
    World *w;
    local_player(&w);
    horde_despawn(w);
    log_info("despawn_horde: done");
}

static void cmd_horde_stats(int argc, const char **argv) {
    (void)argc; (void)argv;
    const HordeStats *s = &H->st;
    log_info("horde: %d zombies (+%d pending, %d spawn rejects) | tiers near %d mid %d far %d dormant %d | bodies %d | tick %.3f ms avg %.3f max (grid %.3f steer %.3f casts %.3f move %.3f phys+attack %.3f anim %.3f, %d evals) | steered %d casts %d ground %d | attacks %d hits %d",
             s->count, s->pending, s->failed_spawns, s->tier_count[0], s->tier_count[1], s->tier_count[2], s->tier_count[3], s->bodies, s->ms_avg, s->ms_max, s->ms_grid,
             s->ms_steer, s->ms_casts, s->ms_move, s->ms_phys, s->ms_anim, s->anim_evals, s->steered, s->casts, s->ground_casts, s->attacks, s->hits);
}

static void cmd_horde_target(int argc, const char **argv) {
    World *w;
    Player *p = local_player(&w);
    const char *m = argc > 1 ? argv[1] : "players";
    if (!strcmp(m, "none")) H->target_mode = HTARGET_NONE;
    else if (!strcmp(m, "point")) {
        H->target_mode = HTARGET_POINT;
        if (argc > 4) H->target_point = v3((float)atof(argv[2]), (float)atof(argv[3]), (float)atof(argv[4]));
        else if (p) H->target_point = p->sim.move.pos;
    } else if (!strcmp(m, "player")) {
        H->target_mode = HTARGET_PLAYER;
        H->target_player = argc > 2 ? atoi(argv[2]) : (p ? (int)(p - w->players) : 0);
    } else {
        H->target_mode = HTARGET_PLAYERS;
    }
    log_info("horde_target: %s", m);
}

static void cmd_horde_reload(int argc, const char **argv) {
    (void)argc; (void)argv;
    load_config();
    log_info("horde: assets/ai/zombie.json reloaded");
}

// horde_test [size] [quit]: spawn, let them come, measure the host tick (horde + their character
// evaluation), check placement; logs PASS / FAIL.
#define TCHECK(cond, ...)                                    \
    do {                                                     \
        H->test_checks++;                                    \
        if (!(cond)) {                                       \
            H->test_fail++;                                  \
            char m_[256];                                    \
            snprintf(m_, sizeof(m_), __VA_ARGS__);           \
            log_warn("horde_test FAIL: %s", m_);             \
        }                                                    \
    } while (0)

static void cmd_horde_test(int argc, const char **argv) {
    World *w;
    Player *p = local_player(&w);
    if (!w || !w->authority || !p) { log_warn("horde_test: host only"); return; }
    H->test_size = 500;
    H->test_quit = false;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "quit")) H->test_quit = true;
        else if (atoi(argv[k]) > 0) H->test_size = atoi(argv[k]);
    }
    horde_despawn(w);
    H->test_state = 1;
    H->test_frames = H->test_fail = H->test_checks = 0;
    H->test_ms_sum = H->test_ms_max = 0;
    H->test_ms_n = 0;
    H->st.failed_spawns = 0;
    int q = horde_spawn(w, p->sim.move.pos, H->test_size, H->cfg.spawn_radius, HSPAWN_RING, "mixed");
    // They converge on the player's spot without killing the player (the measurement needs a
    // living target the whole time).
    H->target_mode = HTARGET_POINT;
    H->target_point = p->sim.move.pos;
    log_info("horde_test: %d zombies queued", q);
}

static void test_tick(World *w) {
    if (!H->test_state) return;
    H->test_frames++;
    if (H->test_state == 1) {
        if (H->q_count == 0 && H->test_frames > 30) {
            // Placement: on the ground, not in geometry.
            int bad_air = 0, bad_geo = 0;
            for (int s = 0; s < H->n; s++) {
                if (!H->active[s]) continue;
                vec3 g, n;
                vec3 p = H->pos[s];
                // Supported if any part of the footprint (centre + 4 points at the capsule radius) is.
                float best = -1e9f;
                for (int k = 0; k < 5; k++) {
                    vec3 o = k == 0 ? v3(0, 0, 0) : v3(k == 1 ? 0.3f : (k == 2 ? -0.3f : 0), 0, k == 3 ? 0.3f : (k == 4 ? -0.3f : 0));
                    if (ground_at(w, v3_add(p, o), p.y + 1.0f, &g, &n) && fabsf(g.y - p.y) < fabsf(best - p.y)) best = g.y;
                }
                g.y = best;
                if (fabsf(best - p.y) > 0.35f) {
                    bad_air++;
                    if (bad_air <= 5) log_info("horde_test: zombie %d at (%.2f %.2f %.2f) ground %.2f tier %d speed %.2f", s, p.x, p.y, p.z, g.y, H->tier[s], v3_len(H->vel[s]));
                }
                // The zombie's own capsule is on the character layer, so world-only overlap.
                if (phys_capsule_overlap(w->phys, v3_add(p, v3(0, 0.15f, 0)), 0.25f, 1.5f, PL_MASK(PL_STATIC), PHYS_INVALID_BODY)) bad_geo++;
            }
            TCHECK(H->st.count >= H->test_size * 9 / 10, "spawned %d of %d", H->st.count, H->test_size);
            TCHECK(bad_air == 0, "%d zombies not on the ground", bad_air);
            TCHECK(bad_geo == 0, "%d zombies inside geometry", bad_geo);
            log_info("horde_test: %d spawned (%d rejected), placement air %d geometry %d", H->st.count, H->st.failed_spawns, bad_air, bad_geo);
            H->test_state = 2;
            H->test_frames = 0;
        }
    } else if (H->test_state == 2) {
        // Measure over 600 ticks while they close in.
        double t = H->st.ms_total + H->st.ms_anim;
        if (H->test_frames % 120 == 0)
            log_info("horde_test: tick %.3f ms (sync %.3f grid %.3f steer %.3f casts %.3f move %.3f phys %.3f | anim %.3f, %d evals) players %d, tiers %d/%d/%d/%d",
                     t, H->st.ms_sync, H->st.ms_grid, H->st.ms_steer, H->st.ms_casts, H->st.ms_move, H->st.ms_phys, H->st.ms_anim, H->st.anim_evals, H->st.players,
                     H->st.tier_count[0], H->st.tier_count[1], H->st.tier_count[2], H->st.tier_count[3]);
        if (H->test_frames > 60) {
            H->test_ms_sum += t;
            H->test_ms_n++;
            if (t > H->test_ms_max) H->test_ms_max = t;
        }
        if (H->test_frames > 660) {
            double avg = H->test_ms_sum / DG_MAX(1, H->test_ms_n);
            float budget = H->test_size <= 500 ? 2.0f : 4.0f * (float)H->test_size / 1000.0f;
            TCHECK(avg <= budget, "%d zombies: %.3f ms average host tick (budget %.1f)", H->test_size, avg, budget);
            log_info("horde_test %s: %d zombies, host tick %.3f ms avg / %.3f max (budget %.1f), tiers near %d mid %d far %d dormant %d, bodies %d, %d checks %d failed",
                     H->test_fail ? "FAIL" : "PASS", H->st.count, avg, H->test_ms_max, budget, H->st.tier_count[0], H->st.tier_count[1], H->st.tier_count[2],
                     H->st.tier_count[3], H->st.bodies, H->test_checks, H->test_fail);
            H->test_state = 0;
            H->target_mode = HTARGET_PLAYERS;
            if (H->test_quit) cbuf_add("quit");
        }
    }
}

void horde_init(void) {
    H = (Horde *)calloc(1, sizeof(Horde));
    load_config();
    hgrid_init(&H->grid, HMAX, 2.0f);
    H->debug = cvar_reg_int("horde_debug", 0, 0, 3, 0, "horde debug: 1 tiers + velocity + bodies, 2 + wall casts, 3 + grid cells");
    H->enable = cvar_reg_bool("horde_enable", true, 0, "run the horde simulation (host)");
    cvar_reg_bool("horde_jobs", true, 0, "horde zombies: steering and animation in parallel jobs");
    cmd_register("spawn_horde", cmd_spawn_horde, "spawn_horde <count> [radius] [ring|random|cluster] [character_id|mixed] (host)");
    cmd_register("despawn_horde", cmd_despawn_horde, "remove every horde zombie (host)");
    cmd_register("horde_stats", cmd_horde_stats, "horde count, tiers, per-phase tick cost");
    cmd_register("horde_target", cmd_horde_target, "horde_target <players|player [slot]|point [x y z]|none>");
    cmd_register("horde_reload", cmd_horde_reload, "reload assets/ai/zombie.json");
    cmd_register("horde_test", cmd_horde_test, "horde_test [count] [quit]: spawn, measure the host tick, check placement");
}
