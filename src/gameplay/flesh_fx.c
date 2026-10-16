#include "flesh_fx.h"
#include "characters.h"
#include "entities.h"
#include "effects.h"
#include "game/game.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "render/debug_draw.h"
#include "audio/audio.h"
#include "anim/anim_library.h"
#include "physics/physics.h"
#include "vfx/vfx.h"
#include "vfx/vfx_library.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define LIB (g_engine.anim)
#define FLESH_TRACK 64           // characters with recent wounds
#define FLESH_WOUNDS 8           // remembered per character (bleeding after death)
#define FLESH_BLEEDERS 48
#define FLESH_POOLS 16

typedef struct Wound { int bone; vec3 lpos, lnormal; float strength; } Wound;
typedef struct Track {
    u32 ent;
    float last_hit, last_stain, kill_t;
    bool killed, pool_done;
    Wound wounds[FLESH_WOUNDS];
    int wound_count, wound_head;
} Track;
typedef struct Bleeder { bool alive; u32 ent; int bone; vec3 lpos, lnormal; float age, rate, accum, decay; vec4 tint; } Bleeder;
typedef struct PendingPool { bool alive; u32 ent; int bone; float delay, size; vec4 tint; } PendingPool;

typedef struct FleshFx {
    FleshRules rules;
    Cvar *debug, *bleed;
    Track track[FLESH_TRACK];
    Bleeder bleed_list[FLESH_BLEEDERS];
    PendingPool pools[FLESH_POOLS];
    float time;
    FleshStats stats;
    FleshDebug last;
} FleshFx;

static FleshFx g_fx;
#define FF (&g_fx)

// ------------------------------------------------------------------ helpers

void flesh_pack_event(u8 *data, vec3 incoming, int part, bool kill, bool melee, float dist) {
    memcpy(data, &incoming, sizeof(incoming));
    data[12] = (u8)(part >= 0 && part < BP_COUNT ? part : BP_NONE);
    data[13] = (u8)((kill ? 1u : 0u) | (melee ? 2u : 0u));
    memcpy(data + 14, &dist, sizeof(dist));
}

void flesh_unpack_event(const u8 *data, vec3 *incoming, int *part, bool *kill, bool *melee, float *dist) {
    memcpy(incoming, data, sizeof(*incoming));
    *part = data[12] == BP_NONE ? -1 : data[12];
    *kill = (data[13] & 1u) != 0;
    *melee = (data[13] & 2u) != 0;
    memcpy(dist, data + 14, sizeof(*dist));
}

static int vfx_debug_level(void) { return FF->debug ? FF->debug->i : 0; }

static Track *track_get(u32 ent, bool create) {
    Track *oldest = NULL;
    for (int i = 0; i < FLESH_TRACK; i++) {
        Track *t = &FF->track[i];
        if (t->ent == ent && ent) return t;
        if (!oldest || t->last_hit < oldest->last_hit) oldest = t;
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->ent = ent;
    oldest->last_hit = FF->time;
    oldest->last_stain = -100.0f;
    oldest->kill_t = -100.0f;
    return oldest;
}

static bool bone_world(World *w, CharacterInst *c, int bone, vec3 *pos, quat *rot) {
    Xform x;
    int r = chars_bone_draw(w, c->ent, bone, &x);
    if (r < 0) return false;
    if (r == 0) {
        if (!c->have_pose) return false;
        x = c->pose_cur[bone];
    }
    *pos = x.t;
    *rot = x.r;
    return true;
}

// Skinned decals and bleeders: the bone as drawn this frame.
static bool vfx_bone_cb(u32 ent, int bone, vec3 *pos, quat *rot) {
    World *w = game_world();
    if (!w) return false;
    Xform x;
    if (chars_bone_draw(w, ent, bone, &x) != 1) {
        CharacterInst *c = char_get(w, ent);
        if (!c || !c->have_pose || bone < 0 || !LIB || bone >= LIB->skel.count) return false;
        x = c->pose_cur[bone];   // not drawn this frame (culled): it is invisible anyway
    }
    *pos = x.t;
    *rot = x.r;
    return true;
}

static bool ray_cb(void *ctx, vec3 from, vec3 delta, vec3 *hit, vec3 *normal) {
    World *w = (World *)ctx;
    RayHit rh;
    // Level geometry only: decals on props / doors would not follow them.
    if (!phys_raycast(w->phys, from, delta, PL_MASK_WORLD, PHYS_INVALID_BODY, &rh) || rh.user) return false;
    *hit = rh.point;
    *normal = rh.normal;
    return true;
}

// The hit capsule of a part nearest the point (hitboxes on this machine's pose).
static const HitCapsule *find_capsule(const CharacterInst *c, int part, vec3 p) {
    const HitCapsule *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < c->hit.count; i++) {
        const HitCapsule *k = &c->hit.c[i];
        vec3 ab = v3_sub(k->b, k->a);
        float t = v3_len2(ab) > 1e-8f ? clampf(v3_dot(v3_sub(p, k->a), ab) / v3_len2(ab), 0, 1) : 0;
        float d = v3_dist(p, v3_madd(k->a, ab, t)) - k->r;
        if (part >= 0 && k->part != part) d += 0.5f;   // prefer the reported part
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static vec3 capsule_normal(const HitCapsule *k, vec3 p, vec3 fallback) {
    vec3 ab = v3_sub(k->b, k->a);
    float t = v3_len2(ab) > 1e-8f ? clampf(v3_dot(v3_sub(p, k->a), ab) / v3_len2(ab), 0, 1) : 0;
    vec3 n = v3_sub(p, v3_madd(k->a, ab, t));
    return v3_len2(n) > 1e-8f ? v3_norm(n) : fallback;
}

static int capsule_bone(const HitCapsule *k) {
    if (!k || !LIB || k->body >= LIB->ragdoll.body_count) return -1;
    return LIB->ragdoll.bodies[k->body].bone;
}

static vec4 blood_tint(const CharacterDef *d) {
    // Effects are authored with the reference (human) blood colour; characters scale it.
    const vec3 ref = v3(0.30f, 0.02f, 0.015f);
    float dk = 1.0f - (d->blood.darkness - 0.55f);
    return v4(clampf(d->blood.color.x / ref.x * dk, 0, 3), clampf(d->blood.color.y / ref.y * dk, 0, 3), clampf(d->blood.color.z / ref.z * dk, 0, 3), 1);
}

// Effect ids by name, cached (the names are literals; a reload re-resolves them by name).
static int fx_id(const char *name) {
    static struct { const char *name; int id; } cache[48];
    VfxLibrary *lib = vfx_game_library();
    if (!lib) return -1;
    int free_i = -1;
    for (int i = 0; i < 48; i++) {
        if (!cache[i].name) { if (free_i < 0) free_i = i; continue; }
        if (cache[i].name != name) continue;
        int id = cache[i].id;
        if (id >= 0 && id < lib->effect_count && !strcmp(lib->effects[id].name, name)) return id;
        cache[i].id = vfx_lib_find_effect(lib, name);
        return cache[i].id;
    }
    int id = vfx_lib_find_effect(lib, name);
    if (free_i >= 0) { cache[free_i].name = name; cache[free_i].id = id; }
    return id;
}

static quat dir_rot(vec3 d) {
    d = v3_norm(d);
    return quat_euler(atan2f(-d.x, -d.z), asinf(clampf(d.y, -1, 1)), 0);
}

static void spawn(const char *name, vec3 pos, vec3 normal, vec3 incoming, float scale, float count, float energy, vec4 tint, bool decal_ok) {
    VfxSpawnParams sp = vfx_spawn_params(pos);
    sp.normal = v3_len2(normal) > 1e-8f ? v3_norm(normal) : v3(0, 1, 0);
    sp.incoming = v3_len2(incoming) > 1e-8f ? v3_norm(incoming) : v3_neg(sp.normal);
    sp.rot = dir_rot(sp.incoming);
    sp.scale = scale;
    sp.count_mul = count;
    sp.energy = energy;
    sp.material = SURF_FLESH;
    sp.tint = tint;
    sp.decal_ok = decal_ok;
    vfx_game_spawn_id(fx_id(name), &sp);
}

static const VfxDecalDef *decal_def(const char *effect) {
    VfxLibrary *lib = vfx_game_library();
    if (!lib) return NULL;
    int e = fx_id(effect);
    return e >= 0 && lib->effects[e].decal.enabled ? &lib->effects[e].decal : NULL;
}

static void add_static_decal(const char *effect, vec3 pos, vec3 normal, vec3 tangent, float size, float aspect, vec4 tint, u32 seed) {
    const VfxDecalDef *d = decal_def(effect);
    if (!d) return;
    VfxDecalSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.pos = pos;
    sp.normal = normal;
    sp.tangent = tangent;
    sp.size = size;
    sp.aspect = aspect;
    sp.kind = VFX_DECAL_STATIC;
    sp.tint = tint;
    vfx_sys_add_decal_ex(vfx_game_system(), d, &sp, seed);
}

static void add_skinned_decal(World *w, CharacterInst *c, int bone, const char *effect, vec3 pos, vec3 normal, float size, float grow_to,
                              float grow_time, vec4 tint, u32 seed) {
    const VfxDecalDef *d = decal_def(effect);
    vec3 bp;
    quat br;
    if (!d || bone < 0 || !bone_world(w, c, bone, &bp, &br)) return;
    // Budget: the sim recycles the character's oldest skinned decal past VFX_SKINNED_DECALS_PER_CHAR.
    if (FF->rules.skinned_max <= 0) return;
    quat ib = quat_conj(br);
    VfxDecalSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.pos = pos;
    sp.normal = normal;
    sp.size = size;
    sp.kind = VFX_DECAL_SKINNED;
    sp.attach = c->ent;
    sp.bone = bone;
    sp.lpos = quat_rotate(ib, v3_sub(pos, bp));
    sp.lnormal = quat_rotate(ib, normal);
    vec3 up = fabsf(normal.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
    sp.ltangent = quat_rotate(ib, v3_norm(v3_cross(up, normal)));
    sp.grow_to = grow_to;
    sp.grow_time = grow_time;
    sp.tint = tint;
    vfx_sys_add_decal_ex(vfx_game_system(), d, &sp, seed);
    FF->stats.wounds++;
}

static void remember_wound(Track *t, int bone, vec3 lpos, vec3 lnormal, float strength) {
    if (bone < 0) return;
    Wound *wd = &t->wounds[t->wound_head];
    t->wound_head = (t->wound_head + 1) % FLESH_WOUNDS;
    if (t->wound_count < FLESH_WOUNDS) t->wound_count++;
    wd->bone = bone;
    wd->lpos = lpos;
    wd->lnormal = lnormal;
    wd->strength = strength;
}

static void start_bleeder(CharacterInst *c, const Wound *wd, float rate, vec4 tint) {
    Bleeder *b = NULL;
    for (int i = 0; i < FLESH_BLEEDERS && !b; i++) if (!FF->bleed_list[i].alive) b = &FF->bleed_list[i];
    if (!b) {   // replace the weakest
        for (int i = 0; i < FLESH_BLEEDERS; i++) if (!b || FF->bleed_list[i].rate * expf(-FF->bleed_list[i].age * FF->bleed_list[i].decay) < b->rate * expf(-b->age * b->decay)) b = &FF->bleed_list[i];
    }
    memset(b, 0, sizeof(*b));
    b->alive = true;
    b->ent = c->ent;
    b->bone = wd->bone;
    b->lpos = wd->lpos;
    b->lnormal = wd->lnormal;
    b->rate = rate * wd->strength;
    b->decay = FF->rules.bleed_decay * fmaxf(c->def->blood.decay, 0.05f);
    b->tint = tint;
    FF->stats.bleeders++;
}

static void schedule_pool(CharacterInst *c, int bone, float size, vec4 tint) {
    for (int i = 0; i < FLESH_POOLS; i++) if (FF->pools[i].alive && FF->pools[i].ent == c->ent) return;
    PendingPool *p = NULL;
    for (int i = 0; i < FLESH_POOLS && !p; i++) if (!FF->pools[i].alive) p = &FF->pools[i];
    if (!p) p = &FF->pools[0];
    p->alive = true;
    p->ent = c->ent;
    p->bone = bone;
    p->delay = FF->rules.pool_delay;
    p->size = size;
    p->tint = tint;
}

// Torso bone for the pool (chest / spine), falls back to the wound bone.
static int torso_bone(int fallback) {
    if (!LIB) return fallback;
    for (int i = 0; i < LIB->ragdoll.body_count; i++)
        if (LIB->ragdoll.bodies[i].part == BP_STOMACH || LIB->ragdoll.bodies[i].part == BP_CHEST) return LIB->ragdoll.bodies[i].bone;
    return fallback;
}

static void on_death(World *w, CharacterInst *c, vec3 pos, vec3 dir, int part, float scale) {
    Track *t = track_get(c->ent, true);
    if (t->killed) return;
    t->killed = true;
    t->kill_t = FF->time;
    int gore = vfx_game_gore();
    if (gore == 0) return;
    vec4 tint = blood_tint(c->def);
    const FleshRules *r = &FF->rules;
    // Wounds keep bleeding on the ragdoll, slower and slower.
    if (FF->bleed->i)
        for (int i = 0; i < t->wound_count; i++) start_bleeder(c, &t->wounds[i], r->bleed_rate * (gore == 1 ? 0.5f : 1.0f) * c->def->blood.amount, tint);
    int bone = torso_bone(t->wound_count ? t->wounds[(t->wound_head + FLESH_WOUNDS - 1) % FLESH_WOUNDS].bone : -1);
    if (bone >= 0) schedule_pool(c, bone, r->pool_size[1] * c->def->blood.amount * (gore == 1 ? 0.6f : 1.0f) * fminf(scale, 1.3f), tint);
    (void)pos; (void)dir; (void)part;
}

static void life_hook(World *w, CharacterInst *c, bool died) {
    if (!died) {
        // Revived / respawned: clean body, nothing keeps bleeding.
        vfx_sys_kill_decals(vfx_game_system(), c->ent);
        Track *t = track_get(c->ent, false);
        if (t) memset(t, 0, sizeof(*t));
        for (int i = 0; i < FLESH_BLEEDERS; i++) if (FF->bleed_list[i].ent == c->ent) FF->bleed_list[i].alive = false;
        for (int i = 0; i < FLESH_POOLS; i++) if (FF->pools[i].ent == c->ent) FF->pools[i].alive = false;
        return;
    }
    // Deaths without a flesh hit (explosions, tools) still bleed out from what wounds there are.
    Track *t = track_get(c->ent, true);
    if (!t->wound_count && LIB) {
        int b = torso_bone(-1);
        if (b >= 0) { Wound wd = { b, v3(0, 0, 0), v3(0, 0, 1), 0.5f }; t->wounds[0] = wd; t->wound_count = 1; t->wound_head = 1; }
    }
    on_death(w, c, c->root.t, v3(0, 0, -1), c->death_part, 1.0f);
}

// ------------------------------------------------------------------ public

void flesh_fx_init(void) {
    memset(FF, 0, sizeof(*FF));
    FF->debug = cvar_find("vfx_debug");
    FF->bleed = cvar_reg_bool("gore_bleed", true, CVAR_ARCHIVE, "corpse wounds keep bleeding (drops, decaying rate)");
    flesh_fx_reload();
    vfx_game_set_bone_fn(vfx_bone_cb);
    chars_set_life_hook(life_hook);
}

void flesh_fx_reload(void) {
    size_t n = 0;
    char *text = platform_read_file("assets/vfx/flesh_rules.json", &n);
    char err[128] = { 0 };
    if (!text || !flesh_rules_parse(&FF->rules, text, err, sizeof(err))) {
        if (text) log_warn("%s", err); else log_warn("flesh: assets/vfx/flesh_rules.json missing, defaults");
        flesh_rules_defaults(&FF->rules);
    }
    free(text);
}

void flesh_fx_clear(void) {
    memset(FF->track, 0, sizeof(FF->track));
    memset(FF->bleed_list, 0, sizeof(FF->bleed_list));
    memset(FF->pools, 0, sizeof(FF->pools));
}

const FleshRules *flesh_fx_rules(void) { return &FF->rules; }
const FleshStats *flesh_fx_stats(void) { return &FF->stats; }
void flesh_fx_reset_stats(void) { memset(&FF->stats, 0, sizeof(FF->stats)); }
const FleshDebug *flesh_fx_last(void) { return &FF->last; }

static void splatter(World *w, const FleshHit *h, vec3 origin, vec3 dir, float scale, int rays, float range, float cone, vec4 tint, u32 seed, bool record) {
    FleshSplat sp[16];
    rays = DG_MIN(rays, 16);
    int n = flesh_plan_splatter(&FF->rules, origin, dir, scale, rays, range, cone, ray_cb, w, seed, sp, 16);
    int hits = 0;
    int dbg = vfx_debug_level();
    for (int i = 0; i < n; i++) {
        const FleshSplat *s = &sp[i];
        if (dbg) dbg_line(s->from, s->to, s->hit ? DBG_GREEN : DBG_RED, 6.0f, true);
        if (!s->hit) continue;
        hits++;
        vec4 t = v4(tint.x, tint.y, tint.z, s->alpha);
        add_static_decal("blood_splatter", s->pos, s->normal, s->tangent, s->size, s->aspect, t, flesh_hash(seed + (u32)i * 7919u));
        FF->stats.splats++;
        if (s->drip) {
            const VfxDecalDef *d = decal_def("blood_drip");
            if (d) {
                VfxDecalSpawn ds;
                memset(&ds, 0, sizeof(ds));
                ds.pos = s->pos;
                ds.normal = s->normal;
                ds.size = fmaxf(0.03f, s->size * 0.22f);
                ds.kind = VFX_DECAL_DRIP;
                ds.drip_len = s->drip_len;
                ds.drip_time = FF->rules.drip_time;
                ds.tint = t;
                vfx_sys_add_decal_ex(vfx_game_system(), d, &ds, flesh_hash(seed ^ (u32)i));
                FF->stats.drips++;
            }
        }
        if (dbg) {
            dbg_circle(s->pos, s->normal, s->size * 0.5f, DBG_YELLOW, 6.0f);
            if (v3_len2(s->tangent) > 0) dbg_line(s->pos, v3_madd(s->pos, s->tangent, s->size * 0.5f * s->aspect), DBG_ORANGE, 6.0f, false);
        }
    }
    FF->stats.last_rays = n;
    FF->stats.last_ray_hits = hits;
    if (record) {
        FF->last.rays = n;
        memcpy(FF->last.splats, sp, sizeof(FleshSplat) * (size_t)n);
    }
    (void)h;
}

void flesh_fx_hit(World *w, const FleshHit *h) {
    if (!w || !vfx_game_active()) return;
    double t0 = platform_time();
    FF->stats.hits++;
    CharacterInst *c = char_get(w, h->target);
    vec3 dir = v3_len2(h->dir) > 1e-8f ? v3_norm(h->dir) : v3_neg(h->normal);
    if (!c) {
        fx_impact_ex(h->pos, h->normal, dir, SURF_FLESH, h->caliber, FX_IMPACT_FLESH | ((h->flags & FLESH_HEAD) ? FX_IMPACT_HEAD : 0u));
        return;
    }
    const FleshRules *r = &FF->rules;
    const CharacterDef *def = c->def;
    int gore = vfx_game_gore();
    int part = h->part >= 0 && h->part < BP_COUNT ? h->part : BP_CHEST;
    bool head = (h->flags & FLESH_HEAD) || part == BP_HEAD;
    bool melee = (h->flags & FLESH_MELEE) || h->caliber == VFX_CAL_MELEE;
    bool kill = (h->flags & FLESH_KILL) != 0;
    bool entry = !(h->flags & FLESH_NO_ENTRY);
    bool rest = !(h->flags & FLESH_ENTRY_ONLY);
    if (rest) { FF->last.valid = false; FF->last.exited = false; FF->last.thickness = 0; FF->last.rays = 0; }
    int cal = DG_CLAMP(h->caliber, 0, VFX_CAL_COUNT - 1);
    const FleshCaliberRule *cr = &r->cal[cal];
    float scale = flesh_hit_scale(r, cal, h->damage, h->dist);
    if (head) scale *= 1.15f;
    vec4 tint = blood_tint(def);
    // Aerosol: the character's darkness only (its hue would turn the lit mist brown like dust).
    float mdk = clampf(1.0f - (def->blood.darkness - 0.55f) * 0.6f, 0.5f, 1.2f);
    vec4 mtint = v4(mdk, mdk, mdk, 1);
    Track *tr = track_get(c->ent, true);
    tr->last_hit = FF->time;
    const HitCapsule *cap = find_capsule(c, part, h->pos);
    vec3 n = cap ? capsule_normal(cap, h->pos, h->normal) : h->normal;
    int bone = capsule_bone(cap);
    u32 seed = h->seed ? h->seed : flesh_hash((u32)(h->pos.x * 1000.0f) ^ flesh_hash((u32)(h->pos.y * 1000.0f) ^ (u32)(h->pos.z * 1000.0f) * 31u));

    // Armour (helmets): sparks, no blood until it breaks. Cosmetic counter on each machine,
    // advanced once per hit (the entry half: prediction or the full hit).
    if (c->armor_left[part] > 0 && !melee) {
        if (entry) {
            c->armor_left[part]--;
            bool broke = c->armor_left[part] == 0;
            spawn(broke ? "flesh_helmet_break" : "flesh_helmet", h->pos, n, dir, fmaxf(scale, 0.8f), 1, 1, v4(0, 0, 0, 0), false);
            audio_play(broke ? "impact_metal" : "impact_metal", h->pos, broke ? 0.9f : 0.7f, SND_SFX);
        }
        FF->stats.ms_last_entry = (float)((platform_time() - t0) * 1000.0);
        return;
    }
    if (gore == 0) {
        if (entry) spawn("gore_dust", h->pos, n, dir, scale, 1, 1, v4(0, 0, 0, 0), false);
        if (kill && rest) on_death(w, c, h->pos, dir, part, scale);
        return;
    }
    float low = gore == 1 ? 0.5f : 1.0f;
    float amount = def->blood.amount;
    float mist = def->blood.mist * cr->mist;
    int surface = def->part_surface[part];
    vec3 bp;
    quat br;
    bool have_bone = bone >= 0 && bone_world(w, c, bone, &bp, &br);

    // ---- entry: mist + droplets (+ fibres on clothes), entry wound, stain soaking around it.
    if (entry) {
        double te = platform_time();
        const char *fx = melee ? "melee_flesh" : (surface == CSURF_CLOTH ? "flesh_entry_cloth" : "flesh_entry");
        float drops = cr->droplets * amount * low * (body_part_is_arm(part) || body_part_is_leg(part) ? r->limb_droplets : 1.0f);
        spawn(fx, h->pos, n, melee ? dir : v3_neg(dir), scale, drops, 1, tint, false);
        if (!melee) audio_play("impact_flesh", h->pos, 0.55f * fminf(1.2f, scale), SND_SFX);
        if (mist > 0.05f && !melee) spawn("flesh_entry_mist", h->pos, n, v3_neg(dir), scale * (0.8f + 0.2f * mist), mist * low, 1, mtint, false);
        if (have_bone) {
            float ws = (melee ? 0.09f : r->wound_entry) * (cal == VFX_CAL_SHOTGUN ? 0.7f : 1.0f) * fmaxf(0.8f, scale);
            add_skinned_decal(w, c, bone, melee ? "wound_slash" : "wound_entry", v3_madd(h->pos, n, 0.005f), n, ws, 0, 0, v4(1, 1, 1, 1), seed);
            // One stain per character per second (shotguns: one for the whole cluster).
            if (FF->time - tr->last_stain > 1.0f) {
                tr->last_stain = FF->time;
                float s0 = r->stain_size[0] + (r->stain_size[1] - r->stain_size[0]) * flesh_rand01(&seed);
                add_skinned_decal(w, c, bone, surface == CSURF_CLOTH ? "blood_stain_cloth" : "blood_stain", h->pos, n, s0 * scale,
                                  r->stain_grow * amount * low * scale, r->stain_grow_time, tint, seed ^ 0x57a1u);
            }
            quat ib = quat_conj(br);
            remember_wound(tr, bone, quat_rotate(ib, v3_sub(h->pos, bp)), quat_rotate(ib, n), 1.0f);
        }
        FF->stats.ms_last_entry = (float)((platform_time() - te) * 1000.0);
        FF->stats.ms_max_entry = fmaxf(FF->stats.ms_max_entry, FF->stats.ms_last_entry);
        FF->stats.ms_sum_entry += FF->stats.ms_last_entry;
        FF->stats.n_entry++;
    }
    if (!rest) return;

    // ---- exit: through the hit capsule along the shot; rules decide (caliber, thickness, range).
    FF->last.valid = true;
    FF->last.exited = false;
    FF->last.entry = h->pos;
    FF->last.dir = dir;
    FF->last.rays = 0;
    vec3 ex = h->pos;
    float thick = 0;
    bool exited = false;
    if (!melee && cap && flesh_capsule_exit(cap->a, cap->b, cap->r, h->pos, dir, &ex, &thick))
        exited = flesh_should_exit(r, cal, part, thick, h->dist, seed ^ 0xe817u);
    FF->last.exit = ex;
    FF->last.thickness = thick;
    FF->last.exited = exited;
    int dbg = vfx_debug_level();
    if (dbg && cap) {
        dbg_cross(h->pos, 0.03f, DBG_CYAN, 6.0f);
        dbg_line(h->pos, ex, exited ? DBG_MAGENTA : DBG_GREY, 6.0f, false);
        if (exited) dbg_cross(ex, 0.03f, DBG_MAGENTA, 6.0f);
    }
    float hs = head ? r->head_scale : 1.0f;
    if (exited) {
        FF->stats.exits++;
        spawn(head ? "flesh_exit_head" : "flesh_exit", ex, dir, dir, scale * hs, amount * low * hs, 1, tint, false);
        if (mist > 0.05f) spawn("flesh_mist", ex, dir, dir, scale * hs, mist * low, 1, mtint, false);
        if (have_bone) {
            vec3 en = cap ? capsule_normal(cap, ex, dir) : dir;
            add_skinned_decal(w, c, bone, "wound_exit", v3_madd(ex, en, 0.005f), en, r->wound_exit * scale * hs, 0, 0, v4(1, 1, 1, 1), seed ^ 0xe1u);
            quat ib = quat_conj(br);
            remember_wound(tr, bone, quat_rotate(ib, v3_sub(ex, bp)), quat_rotate(ib, en), 1.5f);
        }
        int rays = (int)ceilf((float)cr->splatter_rays * low * hs * (kill ? 1.3f : 1.0f));
        splatter(w, h, ex, dir, scale * amount * hs, rays, cr->splatter_range * fminf(scale, 1.5f), cr->cone_deg, tint, seed ^ 0x5b1u, true);
    } else if (melee) {
        // Slash: spray along the swing, a little on the ground below.
        splatter(w, h, h->pos, v3_norm(v3_add(dir, v3(0, -0.6f, 0))), scale * amount, (int)ceilf(3 * low), cr->splatter_range, cr->cone_deg, tint, seed ^ 0x3e1u, true);
    } else if (mist > 0.05f) {
        spawn("flesh_mist", h->pos, n, v3_neg(dir), scale * 0.7f, mist * low * 0.6f, 1, mtint, false);
    }
    // Leg hits splash the ground under the target.
    if (body_part_is_leg(part) && !melee)
        splatter(w, h, v3_add(h->pos, v3(0, 0.05f, 0)), v3_norm(v3_add(v3_scale(dir, 0.5f), v3(0, -1, 0))), scale * amount * 0.7f, (int)ceilf(3 * low), 1.6f, 35.0f,
                 tint, seed ^ 0x1e9u, !exited);

    // ---- headshot kills: bone fragments.
    if (head && kill && !melee && gore >= 2) spawn("flesh_bone", exited ? ex : h->pos, dir, dir, scale, 1, 1, v4(0, 0, 0, 0), false);

    // ---- kill: a larger burst, then the pool and the bleeding wounds.
    if (kill) {
        double tk = platform_time();
        spawn("flesh_kill", exited ? ex : h->pos, dir, dir, scale * r->kill_scale, amount * low, 1, tint, false);
        on_death(w, c, h->pos, dir, part, scale);
        (void)tk;
    }
    FF->stats.ms_last_full = (float)((platform_time() - t0) * 1000.0);
    FF->stats.ms_sum_full += FF->stats.ms_last_full;
    FF->stats.n_full++;
    if (kill) {   // the whole killing hit: entry, exit, splatter, burst, pool + bleeders
        FF->stats.ms_last_kill = FF->stats.ms_last_full;
        FF->stats.ms_max_kill = fmaxf(FF->stats.ms_max_kill, FF->stats.ms_last_kill);
    }
}

void flesh_fx_update(World *w, float dt) {
    if (!w) return;
    float ts = 1.0f;
    Cvar *tsc = cvar_find("vfx_timescale");
    if (tsc) ts = tsc->f;
    dt *= ts;
    FF->time += dt;
    if (!vfx_game_active()) return;
    int gore = vfx_game_gore();
    // Bleeding corpses: drops from each remembered wound, the rate decaying.
    for (int i = 0; i < FLESH_BLEEDERS; i++) {
        Bleeder *b = &FF->bleed_list[i];
        if (!b->alive) continue;
        b->age += dt;
        float rate = b->rate * expf(-b->age * b->decay);
        CharacterInst *c = char_get(w, b->ent);
        if (!c || gore == 0 || rate < 0.08f || b->age > FF->rules.bleed_time) { b->alive = false; continue; }
        b->accum += rate * dt;
        if (b->accum < 1.0f) continue;
        b->accum -= floorf(b->accum);
        vec3 bp;
        quat br;
        if (!bone_world(w, c, b->bone, &bp, &br)) { b->alive = false; continue; }
        vec3 p = v3_add(bp, quat_rotate(br, b->lpos));
        vec3 n = v3_norm(quat_rotate(br, b->lnormal));
        spawn("flesh_bleed", v3_madd(p, n, 0.01f), n, v3(0, -1, 0), 1, 1, 1, b->tint, false);
    }
    // Pools: under the torso once the ragdoll has fallen.
    for (int i = 0; i < FLESH_POOLS; i++) {
        PendingPool *p = &FF->pools[i];
        if (!p->alive) continue;
        p->delay -= dt;
        if (p->delay > 0) continue;
        p->alive = false;
        CharacterInst *c = char_get(w, p->ent);
        vec3 bp;
        quat br;
        if (!c || gore == 0 || !bone_world(w, c, p->bone, &bp, &br)) continue;
        vec3 hit, n;
        if (!ray_cb(w, v3_add(bp, v3(0, 0.3f, 0)), v3(0, -2.0f, 0), &hit, &n) || n.y < 0.5f) continue;
        const VfxDecalDef *d = decal_def("blood_pool");
        if (!d) continue;
        VfxDecalSpawn ds;
        memset(&ds, 0, sizeof(ds));
        ds.pos = hit;
        ds.normal = n;
        ds.size = FF->rules.pool_size[0];
        ds.grow_to = fmaxf(p->size, FF->rules.pool_size[0]);
        ds.grow_time = FF->rules.pool_grow_time;
        ds.tint = p->tint;
        vfx_sys_add_decal_ex(vfx_game_system(), d, &ds, flesh_hash(p->ent * 2654435761u));
        FF->stats.pools++;
        if (vfx_debug_level()) dbg_circle(hit, n, ds.grow_to * 0.5f, DBG_RED, 10.0f);
    }
}
