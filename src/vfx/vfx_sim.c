#include "vfx_sim.h"
#include "vfx_curve.h"
#include "core/profiler.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

VfxSpawnParams vfx_spawn_params(vec3 pos) {
    VfxSpawnParams p;
    memset(&p, 0, sizeof(p));
    p.pos = pos;
    p.rot = quat_identity();
    p.normal = v3(0, 1, 0);
    p.incoming = v3(0, 0, -1);
    p.scale = p.count_mul = p.energy = p.intensity = 1.0f;
    p.material = SURF_DEFAULT;
    return p;
}

static void decal_index_reset(VfxSystem *s) {
    s->decal_free_n = 0;
    for (int i = VFX_MAX_DECALS_HARD - 1; i >= 0; i--) { s->decal_free[s->decal_free_n++] = i; s->decal_live_pos[i] = -1; s->skin_pos[i] = -1; }
    s->decal_live_n = s->skin_n = 0;
    memset(s->pool_count, 0, sizeof(s->pool_count));
    memset(s->pool_ring_head, 0, sizeof(s->pool_ring_head));
    memset(s->pool_ring_n, 0, sizeof(s->pool_ring_n));
}

void vfx_sys_init(VfxSystem *s, const VfxLibrary *lib) {
    memset(s, 0, sizeof(*s));
    decal_index_reset(s);
    s->lib = lib;
    s->cfg.quality_mul = 1.0f;
    s->cfg.decal_max = 256;
    s->cfg.blood_decal_max = 256;
    s->cfg.max_cpu = VFX_MAX_CPU_PARTICLES;
    s->cfg.timescale = 1.0f;
    s->cfg.lighting = true;
    s->local_seed = 0x1234567u;
    for (int e = 0; e < VFX_MAX_EFFECTS; e++)
        for (int k = 0; k < VFX_MAX_EMITTERS_PER_EFFECT; k++) s->last_variant[e][k] = -1;
}

void vfx_sys_clear(VfxSystem *s) {
    const VfxLibrary *lib = s->lib;
    VfxConfig cfg = s->cfg;
    VfxRaycastFn rc = s->raycast;
    VfxSoundFn sn = s->sound;
    void *ctx = s->cb_ctx;
    vfx_sys_init(s, lib);
    s->cfg = cfg;
    s->raycast = rc;
    s->sound = sn;
    s->cb_ctx = ctx;
}

// ------------------------------------------------------------------ helpers

static vec3 rand_unit(u32 *st) {
    float z = vfx_rand01(st) * 2.0f - 1.0f, a = vfx_rand01(st) * 6.2831853f;
    float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
    return v3(r * cosf(a), r * sinf(a), z);
}

static void basis(vec3 n, vec3 *t, vec3 *b) {
    vec3 up = fabsf(n.y) < 0.95f ? v3(0, 1, 0) : v3(1, 0, 0);
    *t = v3_norm(v3_cross(up, n));
    *b = v3_cross(n, *t);
}

// Random direction within a cone of half angle (deg) around axis.
static vec3 cone_dir(vec3 axis, float angle_deg, u32 *st) {
    if (angle_deg <= 0.0f) return axis;
    float ca = cosf(fminf(angle_deg, 180.0f) * DG_DEG2RAD);
    float z = 1.0f - vfx_rand01(st) * (1.0f - ca);
    float phi = vfx_rand01(st) * 6.2831853f;
    float r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
    vec3 t, b;
    basis(axis, &t, &b);
    return v3_norm(v3_add(v3_scale(axis, z), v3_add(v3_scale(t, r * cosf(phi)), v3_scale(b, r * sinf(phi)))));
}

static vec3 reflect3(vec3 d, vec3 n) { return v3_sub(d, v3_scale(n, 2.0f * v3_dot(d, n))); }

vec3 vfx_base_dir(const VfxSpawnDef *s, const VfxSpawnParams *p, u32 *st, u32 counter);
vec3 vfx_base_dir(const VfxSpawnDef *s, const VfxSpawnParams *p, u32 *st, u32 counter) {
    vec3 fwd = quat_rotate(p->rot, v3(0, 0, -1));
    vec3 n = v3_len2(p->normal) > 1e-6f ? v3_norm(p->normal) : v3(0, 1, 0);
    vec3 in = v3_len2(p->incoming) > 1e-6f ? v3_norm(p->incoming) : v3(0, 0, -1);
    switch (s->dir) {
    case VFX_DIR_NORMAL: return n;
    case VFX_DIR_REFLECT: {
        vec3 r = reflect3(in, n);
        return v3_norm(v3_add(v3_scale(r, 1.0f - s->dir_bias), v3_scale(n, s->dir_bias)));
    }
    case VFX_DIR_INCOMING: return in;
    case VFX_DIR_UP: return v3(0, 1, 0);
    case VFX_DIR_RANDOM: return rand_unit(st);
    case VFX_DIR_SIDE: {
        vec3 right = quat_rotate(p->rot, v3(1, 0, 0)), up = quat_rotate(p->rot, v3(0, 1, 0));
        // Compensator / LMG vents: alternate left / right, slightly up and forward.
        vec3 side = (counter & 1) ? right : v3_neg(right);
        return v3_norm(v3_add(side, v3_add(v3_scale(up, 0.35f), v3_scale(fwd, 0.15f))));
    }
    case VFX_DIR_SURFACE: {
        // Along the surface in the incoming direction (grazing hits), lifted by the bias.
        vec3 along = v3_sub(in, v3_scale(n, v3_dot(in, n)));
        if (v3_len2(along) < 1e-6f) { vec3 t, b; basis(n, &t, &b); along = t; }
        along = v3_norm(along);
        return v3_norm(v3_add(v3_scale(along, 1.0f - s->dir_bias), v3_scale(n, s->dir_bias)));
    }
    default: return fwd;
    }
}

static vec3 shape_offset(const VfxSpawnDef *s, vec3 axis, u32 *st) {
    switch (s->shape) {
    case VFX_SHAPE_SPHERE: return v3_scale(rand_unit(st), s->radius * cbrtf(vfx_rand01(st)));
    case VFX_SHAPE_BOX: return v3((vfx_rand01(st) * 2 - 1) * s->box.x, (vfx_rand01(st) * 2 - 1) * s->box.y, (vfx_rand01(st) * 2 - 1) * s->box.z);
    case VFX_SHAPE_DISC:
    case VFX_SHAPE_SURFACE: {
        vec3 t, b;
        basis(axis, &t, &b);
        float r = s->radius * sqrtf(vfx_rand01(st)), a = vfx_rand01(st) * 6.2831853f;
        return v3_add(v3_scale(t, r * cosf(a)), v3_scale(b, r * sinf(a)));
    }
    case VFX_SHAPE_CONE: return v3_scale(axis, vfx_rand01(st) * s->radius);
    default: return v3(0, 0, 0);
    }
}

static float effect_duration(const VfxLibrary *lib, const VfxEffectDef *fx) {
    if (fx->duration > 0) return fx->duration;
    float d = 0;
    for (int k = 0; k < fx->emitter_count; k++) {
        const VfxSpawnDef *s = &lib->emitters[fx->emitters[k]].spawn;
        float e = s->delay + fmaxf(s->duration, (float)(s->bursts - 1) * s->burst_interval);
        if (s->mode != VFX_SPAWN_BURST && s->duration <= 0) e = s->delay + 1.0f;
        // Socket-parented (local space) particles need the instance (its transform slot) alive.
        if (s->local) e += s->life.max;
        d = fmaxf(d, e);
    }
    return d + 1e-3f;
}

static int texture_variants(const VfxLibrary *lib, int tex) {
    if (tex < 0 || tex >= lib->texture_count) return 1;
    const VfxTextureMeta *m = &lib->textures[tex];
    return m->variants > 1 ? m->variants : 1;
}

// ------------------------------------------------------------------ spawning

u32 vfx_sys_spawn(VfxSystem *s, int effect, const VfxSpawnParams *p) {
    const VfxLibrary *lib = s->lib;
    if (!lib || effect < 0 || effect >= lib->effect_count) return 0;
    const VfxEffectDef *fx = &lib->effects[effect];
    float dist = v3_dist(p->pos, s->cfg.camera);
    if (dist > fx->cull_distance && fx->priority < 3 && !p->viewmodel) { s->stats.culled++; return 0; }
    // Per-effect instance cap: evict the oldest of the same effect.
    int same = 0, oldest = -1;
    float oldest_age = -1;
    int free_slot = -1;
    for (int i = 0; i < VFX_MAX_INSTANCES; i++) {
        VfxInstance *in = &s->inst[i];
        if (!in->active) { if (free_slot < 0) free_slot = i; continue; }
        if (in->effect == effect) {
            same++;
            if (in->age > oldest_age) { oldest_age = in->age; oldest = i; }
        }
    }
    if (fx->max_instances > 0 && same >= fx->max_instances && oldest >= 0) {
        s->inst[oldest].active = false;
        s->inst_live--;
        s->stats.evicted++;
        if (free_slot < 0) free_slot = oldest;
    }
    if (free_slot < 0) {
        // Global budget: evict the oldest lower-or-equal priority instance.
        float best = -1;
        for (int i = 0; i < VFX_MAX_INSTANCES; i++) {
            VfxInstance *in = &s->inst[i];
            if (lib->effects[in->effect].priority > fx->priority) continue;
            if (in->age > best) { best = in->age; free_slot = i; }
        }
        if (free_slot < 0) return 0;
        s->stats.evicted++;
        s->inst_live--;
    }
    VfxInstance *in = &s->inst[free_slot];
    memset(in, 0, sizeof(*in));
    in->active = true;
    in->effect = effect;
    in->p = *p;
    if (in->p.scale <= 0) in->p.scale = 1;
    if (in->p.count_mul <= 0) in->p.count_mul = 1;
    if (in->p.energy <= 0) in->p.energy = 1;
    if (in->p.intensity <= 0) in->p.intensity = 1;
    s->inst_gen++;
    in->handle = ((s->inst_gen & 0xFFFFFu) << 11) | (u32)free_slot;
    if (in->handle == 0) in->handle = 1u << 11 | (u32)free_slot;
    // Deterministic emitters use the replicated seed; everything else local RNG.
    in->seed = p->seed ? p->seed : vfx_hash(s->local_seed += 0x632be5abu);
    in->start_tick = s->tick;
    in->duration = effect_duration(lib, fx);
    in->last_pos = p->pos;
    in->dist = dist;
    in->lod_mul = 1.0f;
    if (s->cfg.lod_smooth) {
        // continuous: from 1 at lod_dist[0] / 2 through each step's multiplier at its distance (scaled)
        float ds = s->cfg.lod_dist_scale > 0 ? s->cfg.lod_dist_scale : 1.0f, d0 = fx->lod_dist[0] * ds * 0.5f, m0 = 1.0f;
        for (int k = 0; k < 3 && fx->lod_dist[k] < 1e8f; k++) {
            float d1 = fx->lod_dist[k] * ds;
            if (dist <= d0) break;
            if (dist < d1) { float t = logf(dist / d0) / logf(d1 / d0); in->lod_mul = m0 + (fx->lod_mul[k] - m0) * t; break; }
            in->lod_mul = m0 = fx->lod_mul[k];
            d0 = d1;
        }
    } else
        for (int k = 0; k < 3; k++) if (dist > fx->lod_dist[k]) in->lod_mul = fx->lod_mul[k];
    if (p->viewmodel) in->lod_mul = 1.0f;
    s->inst_live++;
    // Effect light (1-2 ticks for flashes).
    // Decal (impacts on static surfaces).
    if (fx->decal.enabled && p->decal_ok) {
        u32 st = vfx_hash(in->seed ^ 0xdeca1u);
        VfxDecalSpawn ds;
        memset(&ds, 0, sizeof(ds));
        ds.pos = p->pos;
        ds.normal = p->normal;
        u32 st2 = st;
        ds.size = vfx_range(fx->decal.size, &st2) * p->scale * (p->decal_scale > 0.0f ? p->decal_scale : 1.0f);
        ds.tint = p->tint;
        vfx_sys_add_decal_ex(s, &fx->decal, &ds, st);
    }
    return in->handle;
}

static VfxInstance *find(VfxSystem *s, u32 handle) {
    if (!handle) return NULL;
    u32 slot = handle & 2047u;
    if (slot >= VFX_MAX_INSTANCES) return NULL;
    VfxInstance *in = &s->inst[slot];
    return (in->active && in->handle == handle) ? in : NULL;
}

bool vfx_sys_alive(const VfxSystem *s, u32 handle) { return find((VfxSystem *)s, handle) != NULL; }

void vfx_sys_set_transform(VfxSystem *s, u32 handle, vec3 pos, quat rot, vec3 vel) {
    VfxInstance *in = find(s, handle);
    if (!in) return;
    in->p.pos = pos;
    in->p.rot = rot;
    in->p.velocity = vel;
}

void vfx_sys_stop(VfxSystem *s, u32 handle) {
    VfxInstance *in = find(s, handle);
    if (!in) return;
    in->duration = fminf(in->duration, in->age);   // stop spawning; CPU particles finish
}

void vfx_sys_add_decal(VfxSystem *s, const VfxDecalDef *d, vec3 pos, vec3 normal, float scale, u32 seed) {
    VfxDecalSpawn sp;
    memset(&sp, 0, sizeof(sp));
    sp.pos = pos;
    sp.normal = normal;
    u32 st = seed;
    sp.size = vfx_range(d->size, &st) * scale;
    vfx_sys_add_decal_ex(s, d, &sp, seed);
}

void vfx_sys_decal_release(VfxSystem *s, int slot) {
    if (slot < 0 || slot >= VFX_MAX_DECALS_HARD || !s->decals[slot].alive) return;
    VfxDecal *d = &s->decals[slot];
    d->alive = false;
    s->decal_count--;
    s->pool_count[d->pool]--;
    int lp = s->decal_live_pos[slot];
    if (lp >= 0) {
        int last = s->decal_live[--s->decal_live_n];
        s->decal_live[lp] = last;
        s->decal_live_pos[last] = lp;
        s->decal_live_pos[slot] = -1;
    }
    int sp = s->skin_pos[slot];
    if (sp >= 0) {
        int last = s->skin_slots[--s->skin_n];
        s->skin_slots[sp] = last;
        s->skin_pos[last] = sp;
        s->skin_pos[slot] = -1;
    }
    s->decal_free[s->decal_free_n++] = slot;
}

int vfx_sys_decal_count(const VfxSystem *s, int pool, u32 attach) {
    if (!attach) return s->pool_count[DG_CLAMP(pool, 0, VFX_DECAL_POOL_COUNT - 1)];
    int n = 0;
    for (int i = 0; i < s->skin_n; i++) {
        const VfxDecal *d = &s->decals[s->skin_slots[i]];
        if (d->pool == pool && d->attach == attach) n++;
    }
    return n;
}

void vfx_sys_kill_decals(VfxSystem *s, u32 attach) {
    if (!attach) return;
    for (int i = s->skin_n - 1; i >= 0; i--) {
        int slot = s->skin_slots[i];
        if (s->decals[slot].attach == attach) vfx_sys_decal_release(s, slot);
    }
}

static int cmp_ring(const void *a, const void *b) {
    u32 x = ((const u32 *)a)[1], y = ((const u32 *)b)[1];
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Rebuilds a pool's ring from the live decals (oldest first); returns its length.
static int pool_ring_rebuild(VfxSystem *s, int pool) {
    int n = 0;
    for (int i = 0; i < s->decal_live_n; i++) {
        int slot = s->decal_live[i];
        if (s->decals[slot].pool != pool) continue;
        s->pool_ring[pool][n].slot = slot;
        s->pool_ring[pool][n].serial = s->decals[slot].serial;
        n++;
    }
    qsort(s->pool_ring[pool], (size_t)n, sizeof(s->pool_ring[pool][0]), cmp_ring);
    s->pool_ring_head[pool] = 0;
    s->pool_ring_n[pool] = n;
    return n;
}

// Oldest live decal of a pool (ring in spawn order; entries of decals that died since are skipped).
static int pool_oldest(VfxSystem *s, int pool) {
    while (s->pool_ring_n[pool] > 0) {
        int h = s->pool_ring_head[pool];
        int slot = s->pool_ring[pool][h].slot;
        if (s->decals[slot].alive && s->decals[slot].serial == s->pool_ring[pool][h].serial) return slot;
        s->pool_ring_head[pool] = (h + 1) % VFX_MAX_DECALS_HARD;
        s->pool_ring_n[pool]--;
    }
    return -1;
}

int vfx_sys_add_decal_ex(VfxSystem *s, const VfxDecalDef *d, const VfxDecalSpawn *sp, u32 seed) {
    int pool = DG_CLAMP(d->pool, 0, VFX_DECAL_POOL_COUNT - 1);
    int max = pool == VFX_DECAL_POOL_BLOOD ? s->cfg.blood_decal_max : s->cfg.decal_max;
    if (max <= 0) return -1;
    // Budgets: the character's own for skinned decals, then the pool's (oldest recycled first).
    bool skinned = sp->kind == VFX_DECAL_SKINNED && sp->attach;
    if (skinned) {
        int in_char = 0, oldest = -1;
        u32 oc = 0xFFFFFFFFu;
        for (int i = 0; i < s->skin_n; i++) {
            const VfxDecal *e = &s->decals[s->skin_slots[i]];
            if (e->attach != sp->attach) continue;
            in_char++;
            if (e->serial < oc) { oc = e->serial; oldest = s->skin_slots[i]; }
        }
        if (in_char >= VFX_SKINNED_DECALS_PER_CHAR) vfx_sys_decal_release(s, oldest);
        else if (s->skin_n >= VFX_MAX_SKINNED_DECALS) return -1;
    }
    if (s->pool_count[pool] >= max) vfx_sys_decal_release(s, pool_oldest(s, pool));
    if (s->pool_count[pool] >= max || s->decal_free_n <= 0) return -1;
    int slot = s->decal_free[--s->decal_free_n];
    VfxDecal *dc = &s->decals[slot];
    s->decal_count++;
    s->pool_count[pool]++;
    s->decal_live_pos[slot] = s->decal_live_n;
    s->decal_live[s->decal_live_n++] = slot;
    if (skinned) { s->skin_pos[slot] = s->skin_n; s->skin_slots[s->skin_n++] = slot; }
    u32 st = seed;
    memset(dc, 0, sizeof(*dc));
    dc->alive = true;
    dc->pool = (u8)pool;
    dc->kind = (u8)sp->kind;
    dc->pos = sp->pos;
    dc->normal = v3_len2(sp->normal) > 1e-6f ? v3_norm(sp->normal) : v3(0, 1, 0);
    dc->angle = vfx_rand01(&st) * 6.2831853f;
    dc->tangent = sp->tangent;
    dc->size = sp->size > 0 ? sp->size : vfx_range(d->size, &st);
    dc->aspect = sp->aspect > 0 ? sp->aspect : 1.0f;
    dc->depth = d->depth > 0 ? d->depth : 0.05f;
    dc->life = sp->life > 0 ? sp->life : (d->lifetime > 0 ? d->lifetime : 60.0f);
    dc->texture = d->texture_id;
    dc->normal_tex = d->normal_id;
    dc->variants = d->variants > 0 ? d->variants : 1;
    dc->variant = (int)(vfx_rand01(&st) * (float)dc->variants) % dc->variants;
    dc->tint = d->tint;
    if (sp->tint.w > 0) dc->tint = v4(d->tint.x * sp->tint.x, d->tint.y * sp->tint.y, d->tint.z * sp->tint.z, d->tint.w * sp->tint.w);
    dc->attach = sp->attach;
    dc->bone = sp->bone;
    dc->lpos = sp->lpos;
    dc->lnormal = sp->lnormal;
    dc->ltangent = sp->ltangent;
    dc->grow_to = sp->grow_to;
    dc->grow_time = sp->grow_time;
    dc->drip_len = sp->drip_len;
    dc->drip_time = sp->drip_time;
    dc->drip_start = sp->pos;
    dc->roughness = d->roughness;
    dc->dry_time = d->dry_time;
    dc->dry_darken = d->dry_darken > 0 ? d->dry_darken : 1.0f;
    dc->serial = ++s->decal_serial;
    {
        int n = s->pool_ring_n[pool];
        if (n >= VFX_MAX_DECALS_HARD) n = pool_ring_rebuild(s, pool);   // full of stale entries: compact
        int at = (s->pool_ring_head[pool] + n) % VFX_MAX_DECALS_HARD;
        s->pool_ring[pool][at].slot = slot;
        s->pool_ring[pool][at].serial = dc->serial;
        s->pool_ring_n[pool] = n + 1;
    }
    s->decal_head = slot + 1;
    return slot;
}

static int cpu_alloc(VfxSystem *s) {
    int max = s->cfg.max_cpu < VFX_MAX_CPU_PARTICLES ? s->cfg.max_cpu : VFX_MAX_CPU_PARTICLES;
    for (int i = 0; i < max; i++) {
        if (!s->cpu[i].alive) {
            if (i >= s->cpu_count) s->cpu_count = i + 1;
            return i;
        }
    }
    // Pool full: recycle the oldest resting particle (casings on the ground).
    int best = -1;
    float age = -1;
    for (int i = 0; i < max; i++) if (s->cpu[i].resting && s->cpu[i].age > age) { age = s->cpu[i].age; best = i; }
    return best;
}

static void spawn_cpu(VfxSystem *s, VfxInstance *in, int slot_k, const VfxEmitterDef *e, int count, int variant) {
    const VfxSpawnDef *sp = &e->spawn;
    for (int n = 0; n < count; n++) {
        int i = cpu_alloc(s);
        if (i < 0) return;
        VfxCpuParticle *p = &s->cpu[i];
        u32 counter = in->spawn_counter[slot_k]++;
        u32 st = vfx_hash(in->seed ^ vfx_hash((u32)slot_k * 7919u + counter * 104729u));
        memset(p, 0, sizeof(*p));
        p->alive = true;
        p->emitter = (int)(e - s->lib->emitters);
        p->instance = (int)(in - s->inst);
        p->seed = st;
        vec3 axis = vfx_base_dir(sp, &in->p, &st, counter);
        vec3 dir = cone_dir(axis, sp->angle, &st);
        p->pos = v3_add(v3_add(in->p.pos, shape_offset(sp, axis, &st)), v3_scale(axis, sp->offset * in->p.scale));
        p->prev_pos = p->pos;
        float speed = vfx_range(sp->speed, &st) * in->p.energy;
        p->vel = v3_add(v3_scale(dir, speed), v3_scale(in->p.velocity, sp->inherit));
        p->life = fmaxf(1e-3f, vfx_range(sp->life, &st));
        p->size = vfx_range(sp->size, &st) * in->p.scale;
        p->rotation = vfx_range(sp->rotation, &st) * DG_DEG2RAD;
        p->spin = vfx_range(sp->spin, &st) * DG_DEG2RAD;
        float c = vfx_rand01(&st);
        p->color = v4(sp->color_a.x + (sp->color_b.x - sp->color_a.x) * c, sp->color_a.y + (sp->color_b.y - sp->color_a.y) * c,
                      sp->color_a.z + (sp->color_b.z - sp->color_a.z) * c, (sp->color_a.w + (sp->color_b.w - sp->color_a.w) * c) * in->p.intensity);
        if (in->p.tint.w > 0) { p->color.x *= in->p.tint.x; p->color.y *= in->p.tint.y; p->color.z *= in->p.tint.z; }
        p->frame = variant >= 0 ? (float)variant : (sp->frame_random ? floorf(vfx_rand01(&st) * 64.0f) : 0.0f);
        p->rot = quat_norm(quat_mul(in->p.rot, quat_axis_angle(rand_unit(&st), vfx_rand01(&st) * 6.283f)));
        p->ang_vel = v3_scale(rand_unit(&st), vfx_range(sp->spin, &st) * DG_DEG2RAD);
        p->material = in->p.material;
        p->trail[0] = p->pos;
        p->trail_count = 1;
        in->cpu_alive++;
    }
}

static void spawn_gpu(VfxSystem *s, VfxInstance *in, const VfxEmitterDef *e, int count, int variant, float time_offset) {
    if (s->spawn_count >= VFX_MAX_GPU_SPAWNS) return;
    VfxGpuSpawn *g = &s->spawns[s->spawn_count++];
    g->pos_scale = v4(in->p.pos.x, in->p.pos.y, in->p.pos.z, in->p.scale);
    g->rot = v4(in->p.rot.x, in->p.rot.y, in->p.rot.z, in->p.rot.w);
    g->normal_energy = v4(in->p.normal.x, in->p.normal.y, in->p.normal.z, in->p.energy);
    g->incoming_alpha = v4(in->p.incoming.x, in->p.incoming.y, in->p.incoming.z, in->p.intensity);
    g->vel_time = v4(in->p.velocity.x * e->spawn.inherit, in->p.velocity.y * e->spawn.inherit, in->p.velocity.z * e->spawn.inherit, time_offset);
    g->tint = in->p.tint.w > 0 ? v4(in->p.tint.x, in->p.tint.y, in->p.tint.z, 1) : v4(1, 1, 1, 1);
    u32 flags = 0;
    if (!s->cfg.lighting || in->lod_mul < 0.3f) flags |= VFX_SF_NO_LIGHTING;
    if (in->lod_mul < 0.5f) flags |= VFX_SF_NO_COLLIDE;
    if (in->p.viewmodel) flags |= VFX_SF_VIEWMODEL;
    g->info[0] = (u32)e->gpu_index;
    g->info[1] = vfx_hash(in->seed ^ (u32)(e - s->lib->emitters) * 2654435761u ^ vfx_hash(in->spawn_counter[0]++ + s->tick * 31u));
    g->info[2] = (u32)DG_MIN(count, 65535);
    // w: variant (12 bits, 0xFFF = random) | flags (4 bits) | instance slot (16 bits, local space)
    u32 slot = e->spawn.local ? (u32)(in - s->inst) : 0xFFFFu;
    g->info[3] = (u32)(variant >= 0 ? variant & 0xFFF : 0xFFF) | ((flags & 0xFu) << 12) | (slot << 16);
    s->stats.gpu_spawned_frame += count;
}

static int pick_variant(VfxSystem *s, VfxInstance *in, int k, const VfxEmitterDef *e) {
    if (!e->spawn.variant_no_repeat && !e->spawn.frame_random) return -1;
    int n = texture_variants(s->lib, e->render.texture_id);
    if (n <= 1) n = 16;
    u32 st = vfx_hash(in->seed ^ (u32)k * 977u);
    int v = (int)(vfx_rand01(&st) * (float)n) % n;
    if (e->spawn.variant_no_repeat) {
        int *last = &s->last_variant[in->effect][k];
        if (n > 1 && v == *last) v = (v + 1 + (int)(vfx_rand01(&st) * (float)(n - 1))) % n;
        if (v == *last) v = (v + 1) % n;
        *last = v;
    }
    return v;
}

static void emit_count(VfxSystem *s, VfxInstance *in, int k, const VfxEmitterDef *e, float countf, float time_offset) {
    // Stochastic rounding (deterministic from the instance seed).
    u32 st = vfx_hash(in->seed ^ (u32)k * 31337u ^ in->spawn_counter[k] * 7u ^ (u32)in->bursts_done[k]);
    int count = (int)floorf(countf);
    if (vfx_rand01(&st) < countf - (float)count) count++;
    if (count <= 0) return;
    int variant = pick_variant(s, in, k, e);
    if (e->tier == VFX_TIER_CPU || e->render.mode == VFX_RENDER_MESH || e->render.mode == VFX_RENDER_RIBBON) spawn_cpu(s, in, k, e, count, variant);
    else spawn_gpu(s, in, e, count, variant, time_offset);
    if (!strcmp(e->name, "core")) s->stats.flashes_spawned++;
}

static void tick_instance(VfxSystem *s, VfxInstance *in, float dt) {
    const VfxLibrary *lib = s->lib;
    const VfxEffectDef *fx = &lib->effects[in->effect];
    float t0 = in->age, t1 = in->age + dt;
    if (fx->loop && in->duration > 0) {
        // Looping effects restart their emitter schedule every duration.
        float d = in->duration;
        if (floorf(t1 / d) > floorf(t0 / d)) for (int k = 0; k < VFX_MAX_EMITTERS_PER_EFFECT; k++) in->bursts_done[k] = 0;
        t0 = fmodf(t0, d);
        t1 = t0 + dt;
    }
    bool spawning = fx->loop ? true : (in->age < in->duration);
    float moved = v3_dist(in->p.pos, in->last_pos);
    float mul = in->p.count_mul * in->lod_mul * s->cfg.quality_mul;
    for (int k = 0; k < fx->emitter_count && spawning; k++) {
        const VfxEmitterDef *e = &lib->emitters[fx->emitters[k]];
        if (!e->enabled) continue;
        if (e->lod_cull > 0 && in->dist > e->lod_cull && !in->p.viewmodel) continue;
        const VfxSpawnDef *sp = &e->spawn;
        u32 st = vfx_hash(in->seed ^ (u32)k * 1543u ^ (u32)in->bursts_done[k] * 17u);
        if (sp->mode == VFX_SPAWN_BURST) {
            while (in->bursts_done[k] < sp->bursts) {
                float bt = sp->delay + (float)in->bursts_done[k] * sp->burst_interval;
                if (bt >= t1 && !(bt == 0.0f && t0 == 0.0f)) break;
                if (bt < t0 && t0 > 0) { in->bursts_done[k]++; continue; }
                float c = vfx_range(sp->count, &st) * mul;
                emit_count(s, in, k, e, c, fmaxf(0.0f, t1 - bt));
                in->bursts_done[k]++;
            }
        } else {
            float start = sp->delay, end = sp->duration > 0 ? sp->delay + sp->duration : in->duration;
            float a = fmaxf(t0, start), b = fminf(t1, end);
            if (b <= a) continue;
            if (sp->mode == VFX_SPAWN_RATE) in->spawn_accum[k] += sp->rate * (b - a) * mul;
            else in->spawn_accum[k] += sp->per_meter * moved * mul;
            float whole = floorf(in->spawn_accum[k]);
            if (whole >= 1.0f) {
                in->spawn_accum[k] -= whole;
                emit_count(s, in, k, e, whole, 0.0f);
            }
        }
    }
    in->last_pos = in->p.pos;
    // Effect light.
    if (fx->light.enabled && in->age < fx->light.duration && s->light_count < VFX_MAX_LIGHTS) {
        VfxLight *l = &s->lights[s->light_count++];
        l->pos = v3_add(in->p.pos, v3_scale(in->p.normal, 0.08f));
        l->color = fx->light.color;
        l->radius = fx->light.radius * in->p.scale;
        l->intensity = fx->light.intensity * in->p.intensity;
        l->priority = fx->priority + (in->p.viewmodel ? 2 : 0);
        l->shadow = fx->light.shadow;
    }
    in->age += dt;
}

static void tick_cpu(VfxSystem *s, VfxCpuParticle *p, float dt) {
    const VfxEmitterDef *e = &s->lib->emitters[p->emitter];
    const VfxUpdateDef *u = &e->update;
    p->age += dt;
    if (p->age >= p->life) {
        p->alive = false;
        if (p->instance >= 0 && s->inst[p->instance].active) s->inst[p->instance].cpu_alive--;
        if (u->sub_on == VFX_SUB_DEATH && u->sub_effect_id >= 0) {
            VfxSpawnParams sp = vfx_spawn_params(p->pos);
            sp.seed = p->seed;
            sp.count_mul = (float)u->sub_count;
            vfx_sys_spawn(s, u->sub_effect_id, &sp);
        }
        return;
    }
    float tl = p->age / p->life;
    p->prev_pos = p->pos;
    if (!p->resting) {
        p->vel.y -= 9.81f * u->gravity * dt;
        if (u->drag > 0) p->vel = v3_scale(p->vel, expf(-u->drag * dt));
        if (u->wind != 0) p->vel = v3_add(p->vel, v3_scale(v3_sub(s->cfg.wind, p->vel), u->wind * dt));
        if (u->turbulence > 0) {
            // Cheap deterministic swirl (CPU tier): hash-based direction changing with time.
            u32 st = p->seed ^ (u32)(p->age * u->turb_freq * 8.0f);
            vec3 r = rand_unit(&st);
            p->vel = v3_add(p->vel, v3_scale(r, u->turbulence * dt));
        }
        float vmul = vfx_curve_eval1(&u->velocity_curve, tl);
        vec3 delta = v3_scale(p->vel, dt * vmul);
        vec3 hit, n;
        int mat = 0;
        if (u->collide != VFX_COLLIDE_NONE && s->raycast && v3_len2(delta) > 1e-10f && s->raycast(s->cb_ctx, p->pos, delta, &hit, &n, &mat)) {
            bool on_static = (mat & 0x100) == 0;
            mat &= 0xFF;
            if (u->kill_on_hit) { p->age = p->life; }
            p->pos = v3_madd(hit, n, 0.004f);
            float vn = v3_dot(p->vel, n);
            vec3 vt = v3_sub(p->vel, v3_scale(n, vn));
            p->vel = v3_add(v3_scale(vt, 1.0f - u->friction), v3_scale(n, -vn * u->bounce));
            p->ang_vel = v3_scale(p->ang_vel, 0.6f);
            if (fabsf(vn) > 0.6f && p->sound_played < 3 && s->sound) {
                char snd[64];
                snprintf(snd, sizeof(snd), "%s_%s", e->render.mesh[0] ? e->render.mesh : "debris", surface_name(mat));
                s->sound(s->cb_ctx, snd, hit, fminf(1.0f, fabsf(vn) * 0.25f));
                p->sound_played++;
            }
            if (u->sub_on == VFX_SUB_COLLISION && u->sub_effect_id >= 0 && p->sound_played <= 1) {
                VfxSpawnParams sp = vfx_spawn_params(hit);
                sp.decal_ok = on_static;   // physics hit on the level (blood drops leave splats)
                sp.normal = n;
                if (p->instance >= 0 && s->inst[p->instance].active) sp.tint = s->inst[p->instance].p.tint;
                sp.seed = p->seed;
                sp.material = mat;
                vfx_sys_spawn(s, u->sub_effect_id, &sp);
            }
            if (v3_len(p->vel) < 0.25f && n.y > 0.6f) {
                p->rest_time += dt;
                if (p->rest_time > 0.15f) { p->resting = true; p->vel = v3(0, 0, 0); p->ang_vel = v3(0, 0, 0); }
            }
        } else {
            p->pos = v3_add(p->pos, delta);
        }
        float av = v3_len(p->ang_vel);
        if (av > 1e-4f) p->rot = quat_norm(quat_mul(quat_axis_angle(v3_scale(p->ang_vel, 1.0f / av), av * dt), p->rot));
    }
    p->rotation += p->spin * vfx_curve_eval1(&u->spin_curve, tl) * dt;
    if (e->render.mode == VFX_RENDER_RIBBON) {
        if (p->trail_count < 8) p->trail[p->trail_count++] = p->pos;
        else { memmove(p->trail, p->trail + 1, sizeof(vec3) * 7); p->trail[7] = p->pos; }
    }
    // Particle lights (sparks, embers): budgeted in vfx_sys_collect_lights.
    if (e->render.light_intensity > 0 && s->light_count < VFX_MAX_LIGHTS) {
        VfxLight *l = &s->lights[s->light_count++];
        float fade = 1.0f - tl;
        l->pos = p->pos;
        l->color = e->render.light_color;
        l->radius = e->render.light_radius;
        l->intensity = e->render.light_intensity * fade;
        l->priority = 0;
        l->shadow = true;
    }
}

void vfx_sys_tick(VfxSystem *s, float dt) {
    s->light_count = 0;
    for (int i = 0; i < VFX_MAX_INSTANCES; i++) {
        VfxInstance *in = &s->inst[i];
        if (!in->active) continue;
        in->dist = v3_dist(in->p.pos, s->cfg.camera);
        tick_instance(s, in, dt);
    }
    int alive = 0;
    for (int i = 0; i < s->cpu_count; i++) {
        VfxCpuParticle *p = &s->cpu[i];
        if (!p->alive) continue;
        tick_cpu(s, p, dt);
        if (p->alive) alive++;
    }
    while (s->cpu_count > 0 && !s->cpu[s->cpu_count - 1].alive) s->cpu_count--;
    // Instances end when their schedule is over and their CPU particles are gone (GPU
    // particles live on independently).
    int live = 0;
    for (int i = 0; i < VFX_MAX_INSTANCES; i++) {
        VfxInstance *in = &s->inst[i];
        if (!in->active) continue;
        const VfxEffectDef *fx = &s->lib->effects[in->effect];
        bool light_on = fx->light.enabled && in->age < fx->light.duration;
        if (!fx->loop && in->age >= in->duration && in->cpu_alive <= 0 && !light_on) { in->active = false; continue; }
        live++;
    }
    s->inst_live = live;
    prof_count(PC_VFX_EMITTERS, live);
    for (int i = s->decal_live_n - 1; i >= 0; i--) {
        int slot = s->decal_live[i];
        VfxDecal *d = &s->decals[slot];
        d->age += dt;
        if (d->age >= d->life) vfx_sys_decal_release(s, slot);
    }
    s->stats.instances = live;
    s->stats.cpu_particles = alive;
    s->stats.decals = s->decal_count;
    s->tick++;
}

static int cmp_light(const void *a, const void *b) {
    float x = ((const VfxLight *)a)->score, y = ((const VfxLight *)b)->score;
    return x < y ? 1 : (x > y ? -1 : 0);
}

int vfx_sys_collect_lights(VfxSystem *s, VfxLight *out, int max) {
    for (int i = 0; i < s->light_count; i++) {
        VfxLight *l = &s->lights[i];
        float d = v3_dist(l->pos, s->cfg.camera);
        l->score = (float)(l->priority + 1) * l->intensity * l->radius / (1.0f + d * d * 0.05f);
    }
    qsort(s->lights, (size_t)s->light_count, sizeof(VfxLight), cmp_light);
    int n = s->light_count < max ? s->light_count : max;
    memcpy(out, s->lights, sizeof(VfxLight) * (size_t)n);
    s->stats.lights = n;
    return n;
}

u32 vfx_sys_cpu_hash(const VfxSystem *s) {
    u32 h = 2166136261u;
    for (int i = 0; i < s->cpu_count; i++) {
        const VfxCpuParticle *p = &s->cpu[i];
        if (!p->alive) continue;
        const float f[8] = { p->pos.x, p->pos.y, p->pos.z, p->vel.x, p->vel.y, p->vel.z, p->age, p->size };
        const u8 *b = (const u8 *)f;
        for (size_t k = 0; k < sizeof(f); k++) { h ^= b[k]; h *= 16777619u; }
    }
    return h;
}
