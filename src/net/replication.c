#include "replication.h"
#include "protocol.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "game/game_internal.h"
#include "gameplay/characters.h"
#include "core/log.h"
#include "core/engine.h"
#include "gameplay/zombies/horde.h"
#include "net_prof.h"
#include <stdlib.h>

void snap_history_init(SnapHistory *h) {
    for (int i = 0; i < SNAP_HISTORY; i++) {
        h->frames[i].ents = (NetEnt *)calloc(MAX_SNAP_ENTS, sizeof(NetEnt));
        h->frames[i].tick = 0;
        h->frames[i].count = 0;
    }
}

void snap_history_free(SnapHistory *h) {
    for (int i = 0; i < SNAP_HISTORY; i++) { free(h->frames[i].ents); h->frames[i].ents = NULL; }
}

SnapFrame *snap_history_slot(SnapHistory *h, u32 tick) { return &h->frames[tick % SNAP_HISTORY]; }

SnapFrame *snap_history_find(SnapHistory *h, u32 tick) {
    if (!tick) return NULL;
    SnapFrame *f = &h->frames[tick % SNAP_HISTORY];
    return (f->tick == tick && f->ents) ? f : NULL;
}

// ------------------------------------------------------------------ capture

// Corpses and fabricators too: a dead player anywhere follows the recovery of its body (docs/REVIVE.md).
static bool always_relevant(EntityType t) { return t == ET_LIGHT || t == ET_GATE || t == ET_PLAYER || t == ET_VEHICLE || t == ET_CORPSE || t == ET_FABRICATOR; }

static void capture_one(World *w, int i, NetEnt *e) {
    memset(e, 0, sizeof(*e));
    e->id = ENT_MAKE(i, w->gen[i]);
    e->type = w->type[i];
    e->slot = w->player_slot[i];
    e->flags = w->flags[i];
    e->pos = w->xf[i].pos;
    e->rot = w->xf[i].rot;
    e->anim = w->anim[i];
    e->item = w->item[i];
    memcpy(e->attach, w->attach[i], sizeof(e->attach));
    if (w->type[i] == ET_PICKUP) e->count = w->pickup[i].count;
    if (w->type[i] == ET_CORPSE || w->type[i] == ET_FABRICATOR) { e->count = w->aux_count[i]; e->extra = w->aux_extra[i]; }
    if (w->comps[i] & C_HEALTH) {
        e->hp = w->health[i].hp;
        e->max_hp = w->health[i].max_hp;
        e->armor = w->health[i].armor;
        e->max_armor = w->health[i].max_armor;
    }
    e->shape = w->vis[i].shape;
    e->model = w->vis[i].model;
    e->emissive = w->vis[i].emissive;
    e->half = w->vis[i].half;
    e->color = w->vis[i].color;
    dg_strcpy(e->name, sizeof(e->name), w->names[i]);
    if (w->comps[i] & C_LIGHT) {
        e->light_color = w->light[i].color;
        e->light_radius = w->light[i].radius;
        e->light_intensity = w->light[i].intensity;
        e->light_group = w->light[i].group;
    }
    if (w->type[i] == ET_DOOR) e->extra = w->door[i].knob_local;
    const AnimNetState *as = chars_net_state(w, i);
    if (as) {
        e->anim_state = *as;
        e->has_anim = true;
        if (w->type[i] == ET_ZOMBIE) {
            // Hordes: clients derive locomotion from the motion (characters.c client_one); only
            // the discrete state (states, flags, limbs, weapon, character, death) goes out.
            e->anim_state.speed = 0;
            e->anim_state.move_x = e->anim_state.move_y = 0;
            e->anim_state.aim_yaw = e->anim_state.body_yaw = 0;
            e->anim_state.aim_pitch = 0;
            e->anim_state.phase = 0;
            e->anim_state.state_start = 0;
        }
    }
    if (w->type[i] == ET_SWITCH) e->extra.x = (float)w->lever[i].light_group;
    // z: the Unity-style Shadow Type and Strength (0 = engine default; else 1 + type x 2 + strength)
    if (w->type[i] == ET_LIGHT)
        e->extra = v3(w->light[i].flicker, w->light[i].scheduled ? 1.0f : 0.0f, w->light[i].shadow_given ? 1.0f + 2.0f * (float)w->light[i].shadow.type + w->light[i].shadow.strength : 0.0f);
}

void repl_capture(World *w, vec3 center, float radius, SnapFrame *out) {
    out->count = 0;
    float r2 = radius * radius;
    for (int i = 1; i < w->max_used && i < CLIENT_ENTITY_BASE; i++) {
        if (!w->alive[i]) continue;
        EntityType t = (EntityType)w->type[i];
        if (t == ET_NONE) continue;
        if (!always_relevant(t) && v3_len2(v3_sub(w->xf[i].pos, center)) > r2) continue;
        if (out->count >= MAX_SNAP_ENTS) { log_warn("repl: snapshot entity limit reached"); break; }
        capture_one(w, i, &out->ents[out->count++]);
    }
}

// ------------------------------------------------------------------ delta encoding

static u32 diff_mask(const NetEnt *a, const NetEnt *b) {
    if (!a) {
        u32 all = NF_ALL;
        u8 zero[sizeof(b->attach)] = { 0 };
        if (!memcmp(b->attach, zero, sizeof(zero))) all &= ~(u32)NF_ATTACH;   // nothing attached: skip
        return all;
    }
    u32 m = 0;
    if (a->type != b->type || a->slot != b->slot) m |= NF_TYPE;
    if (a->flags != b->flags) m |= NF_FLAGS;
    if (!v3_eq(a->pos, b->pos)) m |= NF_POS;
    if (memcmp(&a->rot, &b->rot, sizeof(quat)) != 0) m |= NF_ROT;
    if (a->anim != b->anim) m |= NF_ANIM;
    if (a->item != b->item || a->count != b->count) m |= NF_ITEM;
    if (memcmp(a->attach, b->attach, sizeof(a->attach)) != 0) m |= NF_ATTACH;
    if (a->hp != b->hp || a->max_hp != b->max_hp || a->armor != b->armor) m |= NF_HP;
    if (a->shape != b->shape || a->model != b->model || a->emissive != b->emissive || !v3_eq(a->half, b->half) || a->color != b->color) m |= NF_VIS;
    if (strcmp(a->name, b->name) != 0) m |= NF_NAME;
    if (!v3_eq(a->light_color, b->light_color) || a->light_radius != b->light_radius || a->light_intensity != b->light_intensity ||
        a->light_group != b->light_group) m |= NF_LIGHT;
    if (!v3_eq(a->extra, b->extra)) m |= NF_EXTRA;
    if (b->has_anim && (!a->has_anim || anim_net_diff(&a->anim_state, &b->anim_state))) m |= NF_ANIMST;
    return m;
}

// The anim sub-mask needs the baseline: written by repl_write_delta via this hook.
static const NetEnt *g_write_base;

static i16 g_dpos[3];   // NF_DPOS payload (set by repl_write_delta)
static u32 g_field_count[16], g_bytes_by_type[32];
void repl_debug_dump(void) {
    char b[512];
    int n = snprintf(b, sizeof(b), "repl fields:");
    for (int k = 0; k < 14; k++) n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%u", k, g_field_count[k]);
    n += snprintf(b + n, sizeof(b) - (size_t)n, " | bytes by type:");
    for (int t = 0; t < 32; t++) if (g_bytes_by_type[t]) n += snprintf(b + n, sizeof(b) - (size_t)n, " %d:%u", t, g_bytes_by_type[t]);
    log_info("%s", b);
    memset(g_field_count, 0, sizeof(g_field_count));
    memset(g_bytes_by_type, 0, sizeof(g_bytes_by_type));
}

static void write_fields(Writer *w, const NetEnt *e, u32 m) {
    if (m & NF_TYPE) { w_u8(w, e->type); w_u8(w, e->slot); }
    if (m & NF_FLAGS) w_u32(w, e->flags);
    if (m & NF_POS) w_vec3(w, e->pos);
    if (m & NF_DPOS) w_bytes(w, g_dpos, sizeof(g_dpos));
    if (m & NF_DPOS8) { i8 q[3] = { (i8)g_dpos[0], (i8)g_dpos[1], (i8)g_dpos[2] }; w_bytes(w, q, sizeof(q)); }
    if (m & NF_ROT) w_quat(w, e->rot);
    if (m & NF_ANIM) w_f32(w, e->anim);
    if (m & NF_ITEM) { w_u16(w, e->item); w_u16(w, e->count); }
    if (m & NF_HP) { w_f32(w, e->hp); w_f32(w, e->max_hp); w_f32(w, e->armor); w_f32(w, e->max_armor); }
    if (m & NF_VIS) { w_u8(w, e->shape); w_u16(w, e->model); w_u8(w, e->emissive); w_vec3(w, e->half); w_u32(w, e->color); }
    if (m & NF_NAME) w_str(w, e->name);
    if (m & NF_LIGHT) { w_vec3(w, e->light_color); w_f32(w, e->light_radius); w_f32(w, e->light_intensity); w_u32(w, e->light_group); }
    if (m & NF_EXTRA) w_vec3(w, e->extra);
    if (m & NF_ATTACH) {
        // Per-slot delta against the baseline: a 5-bit mask, then only the changed ids.
        u8 sub = 0;
        for (int k = 0; k < (int)sizeof(e->attach); k++)
            if (!g_write_base || g_write_base->attach[k] != e->attach[k]) sub |= (u8)(1u << k);
        w_u8(w, sub);
        for (int k = 0; k < (int)sizeof(e->attach); k++)
            if (sub & (1u << k)) w_u8(w, e->attach[k]);
    }
    if (m & NF_ANIMST) {
        u16 f = (g_write_base && g_write_base->has_anim) ? anim_net_diff(&g_write_base->anim_state, &e->anim_state) : anim_net_diff(NULL, &e->anim_state);
        anim_net_write(w, &e->anim_state, f);
    }
}

static void read_fields(Reader *r, NetEnt *e, u32 m) {
    if (m & NF_TYPE) { e->type = r_u8(r); e->slot = r_u8(r); }
    if (m & NF_FLAGS) e->flags = r_u32(r);
    if (m & NF_POS) e->pos = r_vec3(r);
    if (m & NF_DPOS8) {
        i8 q[3];
        r_bytes(r, q, sizeof(q));
        e->pos = v3(e->pos.x + (float)q[0] * (1.0f / 64.0f), e->pos.y + (float)q[1] * (1.0f / 64.0f), e->pos.z + (float)q[2] * (1.0f / 64.0f));
    }
    if (m & NF_DPOS) {
        i16 q[3];
        r_bytes(r, q, sizeof(q));
        e->pos = v3(e->pos.x + (float)q[0] * (1.0f / 256.0f), e->pos.y + (float)q[1] * (1.0f / 256.0f), e->pos.z + (float)q[2] * (1.0f / 256.0f));
    }
    if (m & NF_ROT) e->rot = r_quat(r);
    if (m & NF_ANIM) e->anim = r_f32(r);
    if (m & NF_ITEM) { e->item = r_u16(r); e->count = r_u16(r); }
    if (m & NF_HP) { e->hp = r_f32(r); e->max_hp = r_f32(r); e->armor = r_f32(r); e->max_armor = r_f32(r); }
    if (m & NF_VIS) { e->shape = r_u8(r); e->model = r_u16(r); e->emissive = r_u8(r); e->half = r_vec3(r); e->color = r_u32(r); }
    if (m & NF_NAME) r_str(r, e->name, sizeof(e->name));
    if (m & NF_LIGHT) { e->light_color = r_vec3(r); e->light_radius = r_f32(r); e->light_intensity = r_f32(r); e->light_group = r_u32(r); }
    if (m & NF_EXTRA) e->extra = r_vec3(r);
    if (m & NF_ATTACH) {
        u8 sub = r_u8(r);
        for (int k = 0; k < (int)sizeof(e->attach); k++)
            if (sub & (1u << k)) e->attach[k] = r_u8(r);
    }
    if (m & NF_ANIMST) { anim_net_read(r, &e->anim_state, NULL); e->has_anim = true; }
}

// The quantized rotation round-trips lossy; compare/store what the client will decode so
// unchanged rotations don't get resent every snapshot.
static quat quantize_rot(quat q) {
    u8 buf[4];
    Writer w = writer_make(buf, 4);
    w_quat(&w, q);
    Reader r = reader_make(buf, 4);
    return r_quat(&r);
}

void repl_write_delta(Writer *wr, const SnapFrame *base, SnapFrame *cur) {
    // Changes: walk both sorted lists.
    u32 count_pos = wr->len;
    w_u16(wr, 0);
    u16 changes = 0;
    int bi = 0;
    for (int ci = 0; ci < cur->count; ci++) {
        NetEnt e = cur->ents[ci];
        e.rot = quantize_rot(e.rot);
        cur->ents[ci].rot = e.rot;   // the baseline must hold what the client decodes, or every rotation looks changed
        u32 idx = ENT_INDEX(e.id);
        while (base && bi < base->count && ENT_INDEX(base->ents[bi].id) < idx) bi++;
        const NetEnt *b = (base && bi < base->count && base->ents[bi].id == e.id) ? &base->ents[bi] : NULL;
        u32 m = diff_mask(b, &e);
        if (!m) continue;
        if ((m & NF_POS) && b && e.type == ET_ZOMBIE) {
            // Walking zombies: a 3-byte delta (1/64 m) when it fits.
            float qx = roundf((e.pos.x - b->pos.x) * 64.0f), qy = roundf((e.pos.y - b->pos.y) * 64.0f), qz = roundf((e.pos.z - b->pos.z) * 64.0f);
            if (fabsf(qx) <= 127.0f && fabsf(qy) <= 127.0f && fabsf(qz) <= 127.0f) {
                g_dpos[0] = (i16)qx;
                g_dpos[1] = (i16)qy;
                g_dpos[2] = (i16)qz;
                e.pos = v3(b->pos.x + (float)(i8)g_dpos[0] * (1.0f / 64.0f), b->pos.y + (float)(i8)g_dpos[1] * (1.0f / 64.0f), b->pos.z + (float)(i8)g_dpos[2] * (1.0f / 64.0f));
                cur->ents[ci].pos = e.pos;
                m = (m & ~(u32)NF_POS) | NF_DPOS8;
                if (v3_eq(e.pos, b->pos)) m &= ~(u32)NF_DPOS8;
                if (!m) continue;
            }
        }
        if ((m & NF_POS) && b) {
            // Small moves: 6-byte delta from what the client has, stored back exactly as decoded.
            float qx = roundf((e.pos.x - b->pos.x) * 256.0f), qy = roundf((e.pos.y - b->pos.y) * 256.0f), qz = roundf((e.pos.z - b->pos.z) * 256.0f);
            if (fabsf(qx) < 32767.0f && fabsf(qy) < 32767.0f && fabsf(qz) < 32767.0f) {
                g_dpos[0] = (i16)qx;
                g_dpos[1] = (i16)qy;
                g_dpos[2] = (i16)qz;
                e.pos = v3(b->pos.x + (float)g_dpos[0] * (1.0f / 256.0f), b->pos.y + (float)g_dpos[1] * (1.0f / 256.0f), b->pos.z + (float)g_dpos[2] * (1.0f / 256.0f));
                cur->ents[ci].pos = e.pos;
                m = (m & ~(u32)NF_POS) | NF_DPOS;
                if (v3_eq(e.pos, b->pos)) m &= ~(u32)NF_DPOS;   // moved less than the step
                if (!m) continue;
            }
        }
        u32 before = wr->len;
        w_u32(wr, e.id);
        w_u16(wr, (u16)m);
        g_write_base = b;
        write_fields(wr, &e, m);
        g_write_base = NULL;
        changes++;
        for (int k = 0; k < 16; k++) if (m & (1u << k)) g_field_count[k]++;
        netprof_entity(b, &e, m, wr->len - before);
        g_bytes_by_type[DG_MIN(e.type, 31)] += wr->len - before;
    }
    memcpy(wr->buf + count_pos, &changes, 2);
    netprof_entities_considered(cur->count);
    // Removals: baseline entities not present (by id) in the current frame.
    u32 rem_pos = wr->len;
    w_u16(wr, 0);
    u16 removals = 0;
    if (base) {
        int ci = 0;
        for (int b = 0; b < base->count; b++) {
            u32 id = base->ents[b].id;
            while (ci < cur->count && ENT_INDEX(cur->ents[ci].id) < ENT_INDEX(id)) ci++;
            if (ci < cur->count && cur->ents[ci].id == id) continue;
            w_u32(wr, id);
            removals++;
        }
    }
    memcpy(wr->buf + rem_pos, &removals, 2);
}

// ------------------------------------------------------------------ horde update rates

typedef struct RateCand { int ci; u32 prio; } RateCand;
static int cmp_cand(const void *a, const void *b) {
    const RateCand *x = (const RateCand *)a, *y = (const RateCand *)b;
    if (x->prio != y->prio) return x->prio > y->prio ? -1 : 1;
    return x->ci < y->ci ? -1 : (x->ci > y->ci ? 1 : 0);
}

int repl_rate_limit(const SnapFrame *base, SnapFrame *cur, vec3 viewer, u32 snap_index, u8 *age, int budget_bytes) {
    if (!base) return 0;
    const HordeConfig *hc = horde_config();
    static RateCand cand[MAX_SNAP_ENTS];
    int nc = 0, held = 0;
    int bi = 0;
    const int cost = 14;   // bytes of a typical zombie update (measured: header 6, delta position 3, rotation 4; locomotion is derived on the client)
    for (int ci = 0; ci < cur->count; ci++) {
        NetEnt *e = &cur->ents[ci];
        if (e->type != ET_ZOMBIE) continue;
        u32 idx = ENT_INDEX(e->id);
        while (bi < base->count && ENT_INDEX(base->ents[bi].id) < idx) bi++;
        const NetEnt *b = (bi < base->count && base->ents[bi].id == e->id) ? &base->ents[bi] : NULL;
        if (!b) { age[idx] = 0; continue; }   // new to this client: full state now
        float d = v3_len(v3_sub(e->pos, viewer));
        int band = d < hc->net_near ? 0 : (d < hc->net_mid ? 1 : 2);
        int iv = hc ? hc->net_interval[band] : 1;
        bool changed = !v3_eq(e->pos, b->pos) || memcmp(&e->rot, &b->rot, sizeof(quat)) || (e->has_anim && anim_net_diff(&b->anim_state, &e->anim_state));
        bool due = iv > 0 && ((snap_index + idx) % (u32)iv) == 0;
        // Big events (death, health) are never held; only the transform / animation fields.
        if (!changed) { age[idx] = 0; continue; }
        if (!due) {
            e->pos = b->pos;
            e->rot = b->rot;
            e->anim = b->anim;
            if (b->has_anim) e->anim_state = b->anim_state;
            if (age[idx] < 255) age[idx]++;
            held++;
            continue;
        }
        static const u32 weight[3] = { 4, 2, 1 };
        cand[nc].ci = ci;
        cand[nc].prio = ((u32)age[idx] + 1u) * weight[band];
        nc++;
    }
    // Byte budget: the highest priorities go out, the rest wait (and gain priority).
    int room = budget_bytes > 0 ? budget_bytes / cost : nc;
    if (nc > room) {
        qsort(cand, (size_t)nc, sizeof(RateCand), cmp_cand);
        bi = 0;
        for (int k = room; k < nc; k++) {
            NetEnt *e = &cur->ents[cand[k].ci];
            int at = -1;
            // Baseline lookup (binary search by index).
            int lo = 0, hi = base->count - 1;
            u32 idx = ENT_INDEX(e->id);
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                u32 mi = ENT_INDEX(base->ents[mid].id);
                if (mi == idx) { at = base->ents[mid].id == e->id ? mid : -1; break; }
                if (mi < idx) lo = mid + 1; else hi = mid - 1;
            }
            if (at < 0) continue;
            const NetEnt *b = &base->ents[at];
            e->pos = b->pos;
            e->rot = b->rot;
            e->anim = b->anim;
            if (b->has_anim) e->anim_state = b->anim_state;
            if (age[idx] < 255) age[idx]++;
            held++;
        }
        nc = room;
    }
    for (int k = 0; k < nc; k++) age[ENT_INDEX(cur->ents[cand[k].ci].id)] = 0;
    return held;
}

static int find_ent(const SnapFrame *f, u32 id) {
    // Binary search by index (frames are sorted).
    int lo = 0, hi = f->count - 1;
    u32 idx = ENT_INDEX(id);
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        u32 mi = ENT_INDEX(f->ents[mid].id);
        if (mi == idx) return f->ents[mid].id == id ? mid : -1;
        if (mi < idx) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int cmp_ent(const void *a, const void *b) {
    u32 ia = ENT_INDEX(((const NetEnt *)a)->id), ib = ENT_INDEX(((const NetEnt *)b)->id);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

bool repl_read_delta(Reader *rd, const SnapFrame *base, SnapFrame *out) {
    out->count = 0;
    if (base) {
        memcpy(out->ents, base->ents, sizeof(NetEnt) * (size_t)base->count);
        out->count = base->count;
    }
    u16 changes = r_u16(rd);
    bool sorted = true;
    for (int c = 0; c < changes && !rd->overflow; c++) {
        u32 id = r_u32(rd);
        u32 m = r_u16(rd);
        int at = find_ent(out, id);
        if (at < 0) {
            // New entity (or new generation in a reused slot): drop any stale one at that index.
            for (int k = 0; k < out->count; k++) {
                if (ENT_INDEX(out->ents[k].id) == ENT_INDEX(id)) { out->ents[k] = out->ents[--out->count]; sorted = false; break; }
            }
            if (out->count >= MAX_SNAP_ENTS) return false;
            at = out->count++;
            memset(&out->ents[at], 0, sizeof(NetEnt));
            out->ents[at].id = id;
            out->ents[at].rot = quat_identity();
            sorted = false;
        }
        read_fields(rd, &out->ents[at], m);
    }
    u16 removals = r_u16(rd);
    for (int k = 0; k < removals && !rd->overflow; k++) {
        u32 id = r_u32(rd);
        for (int e = 0; e < out->count; e++) {
            if (out->ents[e].id == id) { out->ents[e] = out->ents[--out->count]; sorted = false; break; }
        }
    }
    if (!sorted) qsort(out->ents, (size_t)out->count, sizeof(NetEnt), cmp_ent);
    return !rd->overflow;
}

// ------------------------------------------------------------------ client apply

static void push_sample(InterpBuffer *b, u32 id, u32 tick, vec3 pos, quat rot, float anim) {
    if (b->id != id) { b->id = id; b->count = 0; b->head = 0; }
    if (b->count > 0) {
        const InterpSample *last = &b->s[(b->head + INTERP_SAMPLES - 1) % INTERP_SAMPLES];
        if (last->tick >= tick) return;
    }
    InterpSample *s = &b->s[b->head];
    s->tick = tick;
    s->pos = pos;
    s->rot = rot;
    s->anim = anim;
    b->head = (b->head + 1) % INTERP_SAMPLES;
    if (b->count < INTERP_SAMPLES) b->count++;
}

static void setup_replica(World *w, const NetEnt *e, int local_slot) {
    int i = (int)ENT_INDEX(e->id);
    w->vis[i].shape = e->shape;
    w->vis[i].model = e->model;
    w->vis[i].emissive = e->emissive;
    w->vis[i].half = e->half;
    w->vis[i].color = e->color;
    w->comps[i] |= C_VISUAL;
    w->xf[i].pos = w->xf[i].prev_pos = e->pos;
    w->xf[i].rot = w->xf[i].prev_rot = e->rot;
    w->flags[i] = e->flags;
    dg_strcpy(w->names[i], sizeof(w->names[0]), e->name);
    EntityType t = (EntityType)e->type;
    if (t == ET_PLAYER || t == ET_DUMMY || t == ET_ZOMBIE) w->comps[i] |= C_HEALTH;
    if (t == ET_LIGHT) w->comps[i] |= C_LIGHT;
    if (t == ET_DOOR || t == ET_GATE) w->comps[i] |= C_DOOR;
    if (t == ET_PICKUP) w->comps[i] |= C_PICKUP;
    if (t == ET_CONTAINER) w->comps[i] |= C_CONTAINER;
    if (t == ET_DOOR) w->door[i].knob_local = e->extra;
    if (t == ET_CORPSE || t == ET_FABRICATOR) {
        // entity_setup reads these (the corpse's player, the fabricator's queue).
        w->item[i] = e->item;
        w->aux_count[i] = e->count;
        w->aux_extra[i] = e->extra;
    }
    if (t == ET_PLAYER) {
        int slot = e->slot;
        w->player_slot[i] = (u8)slot;
        w->comps[i] |= C_PLAYER;
        if (slot < MAX_PLAYERS) {
            Player *p = &w->players[slot];
            bool fresh = !p->active || p->ent != e->id;
            p->active = true;
            p->ent = e->id;
            p->local = slot == local_slot;
            dg_strcpy(p->name, sizeof(p->name), e->name);
            if (p->local) {
                if (fresh || !p->character) {
                    game_create_player_character(w, slot, e->pos);
                    player_move_reset(&p->sim, e->pos, quat_get_yaw(e->rot));
                }
            } else {
                // Remote player: kinematic capsule so the local player collides with it.
                w->vis[i].shape = VIS_CAPSULE;
                w->vis[i].half = v3(PLAYER_RADIUS, PLAYER_HEIGHT * 0.5f - PLAYER_RADIUS, PLAYER_RADIUS);
                vec3 center_pos = w->xf[i].pos;
                (void)center_pos;
            }
        }
    }
    entity_setup(w, e->id);
    if (t == ET_PLAYER && e->slot != local_slot) repl_player_body(w, i, true);
}

void repl_player_body(World *w, int i, bool on) {
    if (!on) {
        if (w->body[i].body != PHYS_INVALID_BODY && (w->comps[i] & C_BODY)) phys_remove_body(w->phys, w->body[i].body);
        w->body[i].body = PHYS_INVALID_BODY;
        w->comps[i] &= ~(u32)C_BODY;
        return;
    }
    if ((w->comps[i] & C_BODY) && w->body[i].body != PHYS_INVALID_BODY) return;
    // Body for the remote player (entity_setup gives players only a hitbox).
    BodyDesc d;
    memset(&d, 0, sizeof(d));
    d.shape.kind = SHAPE_CAPSULE;
    d.shape.radius = PLAYER_RADIUS * 0.95f;
    d.shape.half_height = PLAYER_HEIGHT * 0.5f - PLAYER_RADIUS;
    d.pos = v3_add(w->xf[i].pos, v3(0, PLAYER_HEIGHT * 0.5f, 0));
    d.rot = quat_identity();
    d.motion = PM_KINEMATIC;
    d.layer = PL_CHARACTER;
    d.user = ENT_MAKE(i, w->gen[i]);
    w->body[i].body = phys_add_body(w->phys, &d);
    w->body[i].motion = PM_KINEMATIC;
    w->comps[i] |= C_BODY;
}

void repl_apply(World *w, const SnapFrame *f, int local_slot, InterpBuffer *interp) {
    // Destroy replicated entities that are gone (or out of relevance).
    for (int i = 1; i < w->max_used && i < CLIENT_ENTITY_BASE; i++) {
        if (!w->alive[i]) continue;
        EntityId id = ENT_MAKE(i, w->gen[i]);
        if (find_ent(f, id) >= 0) continue;
        if (w->type[i] == ET_PLAYER) {
            int slot = w->player_slot[i];
            if (slot < MAX_PLAYERS && slot != local_slot) {
                memset(&w->players[slot], 0, sizeof(Player));
                w->players[slot].peer = -1;
            }
            if (slot == local_slot) continue; // keep our own entity until the host says otherwise
        }
        ent_destroy(w, id);
    }
    for (int k = 0; k < f->count; k++) {
        const NetEnt *e = &f->ents[k];
        int i = (int)ENT_INDEX(e->id);
        if (!ent_valid(w, e->id)) {
            ent_spawn_at(w, e->id, (EntityType)e->type);
            if (e->has_anim) w->char_id[i] = e->anim_state.character_id;
            setup_replica(w, e, local_slot);
        }
        if (e->has_anim) chars_client_receive(w, e->id, &e->anim_state, f->tick);
        w->flags[i] = e->flags;
        w->item[i] = e->item;
        memcpy(w->attach[i], e->attach, sizeof(w->attach[i]));
        if (e->type == ET_PICKUP) memcpy(w->pickup[i].att, e->attach, sizeof(w->pickup[i].att));
        if (e->type == ET_PICKUP) { w->pickup[i].item = e->item; w->pickup[i].count = e->count; }
        if (e->type == ET_DOOR) w->door[i].key_item = e->item;
        if (e->type == ET_CORPSE || e->type == ET_FABRICATOR) { w->aux_count[i] = e->count; w->aux_extra[i] = e->extra; }
        if (w->comps[i] & C_HEALTH) {
            w->health[i].hp = e->hp;
            w->health[i].max_hp = e->max_hp;
            w->health[i].armor = e->armor;
            w->health[i].max_armor = e->max_armor;
        }
        if (e->type == ET_LIGHT) {
            w->light[i].color = e->light_color;
            w->light[i].radius = e->light_radius;
            w->light[i].intensity = e->light_intensity;
            w->light[i].group = e->light_group;
            w->light[i].flicker = e->extra.x;
            w->light[i].scheduled = e->extra.y > 0.5f;
            if (!w->authority) {
                bool given = e->extra.z > 0.5f;
                if (given && !w->light[i].shadow_given) shs_light_default(&w->light[i].shadow, SHADOW_LIGHT_POINT);
                w->light[i].shadow_given = given;
                if (given) {
                    float v = e->extra.z - 1.0f;
                    int t = DG_CLAMP((int)floorf(v * 0.5f), 0, SHADOW_TYPE_COUNT - 1);
                    w->light[i].shadow.type = (ShadowType)t;
                    w->light[i].shadow.strength = clampf(v - 2.0f * (float)t, 0.0f, 1.0f);
                }
            }
        }
        if (e->type == ET_CONTAINER) w->cont[i].opened = (e->flags & EF_OPEN) != 0;
        if (w->comps[i] & C_INTERACT) w->inter[i].enabled = !(e->flags & EF_HIDDEN);
        w->vis[i].color = e->color;
        w->vis[i].emissive = e->emissive;
        dg_strcpy(w->names[i], sizeof(w->names[0]), e->name);
        bool is_local = e->type == ET_PLAYER && e->slot == local_slot;
        if (!is_local) {
            // Zombies update at reduced rates: a sample only when they moved (an unchanged copy
            // would make them stop and jump); interpolation / extrapolation fills the gaps.
            InterpBuffer *ib = &interp[i];
            const InterpSample *last = (ib->id == e->id && ib->count) ? &ib->s[(ib->head + INTERP_SAMPLES - 1) % INTERP_SAMPLES] : NULL;
            bool skip = e->type == ET_ZOMBIE && last && v3_eq(last->pos, e->pos) && !memcmp(&last->rot, &e->rot, sizeof(quat)) && f->tick - last->tick < 60;
            if (!skip) push_sample(ib, e->id, f->tick, e->pos, e->rot, e->anim);
        }
    }
}

// Client: zombie motion smoothness (mean change of the per-frame velocity, m/s) - low = smooth,
// high = stop-and-go between reduced-rate updates. repl_zombie_jitter() resets the window.
static vec3 g_zprev[MAX_ENTITIES], g_zvel[MAX_ENTITIES];
static double g_zjit_sum, g_zspeed_sum;
static int g_zjit_n;
static double g_zlast_rt;
float repl_zombie_jitter(float *mean_speed) {
    float j = g_zjit_n ? (float)(g_zjit_sum / g_zjit_n) : 0.0f;
    if (mean_speed) *mean_speed = g_zjit_n ? (float)(g_zspeed_sum / g_zjit_n) : 0.0f;
    g_zjit_sum = g_zspeed_sum = 0;
    g_zjit_n = 0;
    return j;
}

static bool (*g_repl_owned[4])(World *w, int index);
static int g_repl_owned_count;
void repl_set_owned_hook(bool (*fn)(World *w, int index)) {
    for (int k = 0; k < g_repl_owned_count; k++) if (g_repl_owned[k] == fn) return;
    if (g_repl_owned_count < 4) g_repl_owned[g_repl_owned_count++] = fn;
}

static bool repl_owned(World *w, int i) {
    for (int k = 0; k < g_repl_owned_count; k++) if (g_repl_owned[k](w, i)) return true;
    return false;
}

void repl_interpolate(World *w, InterpBuffer *interp, double rt, int local_slot) {
    double frame_dt = (rt - g_zlast_rt) / (double)SIM_HZ;
    g_zlast_rt = rt;
    for (int i = 1; i < w->max_used && i < CLIENT_ENTITY_BASE; i++) {
        if (!w->alive[i]) continue;
        if (w->type[i] == ET_PLAYER && w->player_slot[i] == local_slot) continue;
        if (repl_owned(w, i)) continue;   // predicted locally (the driven vehicle, the spectated player)
        InterpBuffer *b = &interp[i];
        if (!b->count || b->id != ENT_MAKE(i, w->gen[i])) continue;
        // Samples are in ring order; walk from oldest.
        int oldest = (b->head - b->count + INTERP_SAMPLES) % INTERP_SAMPLES;
        const InterpSample *a = &b->s[oldest], *c = a;
        for (int k = 0; k < b->count; k++) {
            const InterpSample *s = &b->s[(oldest + k) % INTERP_SAMPLES];
            if ((double)s->tick <= rt) a = s;
            if ((double)s->tick >= rt) { c = s; break; }
            c = s;
        }
        vec3 pos;
        quat rot;
        float anim;
        if (rt > (double)c->tick && w->type[i] == ET_ZOMBIE && b->count >= 2) {
            // Past the newest sample (a reduced-rate zombie): keep walking along its last
            // velocity for up to 0.3 s instead of stopping.
            const InterpSample *pv = &b->s[(b->head + INTERP_SAMPLES - 2) % INTERP_SAMPLES];
            const InterpSample *nw = &b->s[(b->head + INTERP_SAMPLES - 1) % INTERP_SAMPLES];
            float span = (float)(nw->tick - pv->tick);
            float ex = fminf((float)(rt - (double)nw->tick), 0.3f * SIM_HZ);
            pos = nw->pos;
            if (span > 0 && span < 30.0f) pos = v3_add(nw->pos, v3_scale(v3_sub(nw->pos, pv->pos), ex / span));
            rot = nw->rot;
            anim = nw->anim;
        } else if (c->tick == a->tick || rt >= (double)c->tick) {
            pos = c->pos; rot = c->rot; anim = c->anim;
        } else if (rt <= (double)a->tick) {
            pos = a->pos; rot = a->rot; anim = a->anim;
        } else {
            float t = (float)((rt - (double)a->tick) / (double)(c->tick - a->tick));
            pos = v3_lerp(a->pos, c->pos, t);
            rot = quat_nlerp(a->rot, c->rot, t);
            anim = lerpf(a->anim, c->anim, t);
        }
        if (w->type[i] == ET_ZOMBIE && frame_dt > 0 && frame_dt < 0.25 && v3_dist(g_zprev[i], pos) < 3.0f) {
            // Reduced-rate zombies: the drawn position follows the network one smoothly (corrections
            // blend over ~0.1 s instead of snapping).
            float k = 1.0f - expf(-(float)frame_dt / 0.1f);
            pos = v3_lerp(g_zprev[i], pos, k);
        }
        if (w->type[i] == ET_ZOMBIE && frame_dt > 1e-4 && frame_dt < 0.1) {
            vec3 v = v3_scale(v3_sub(pos, g_zprev[i]), (float)(1.0 / frame_dt));
            if (v3_len(v) < 20.0f) {
                g_zjit_sum += v3_len(v3_sub(v, g_zvel[i]));
                g_zspeed_sum += v3_len(v);
                g_zjit_n++;
            }
            g_zvel[i] = v;
        }
        g_zprev[i] = pos;
        w->xf[i].pos = w->xf[i].prev_pos = pos;
        w->xf[i].rot = w->xf[i].prev_rot = rot;
        w->anim[i] = w->anim_prev[i] = anim;
        // Remote players: keep their movement state roughly in sync for hand rigs / hitboxes.
        if (w->type[i] == ET_PLAYER) {
            int slot = w->player_slot[i];
            if (slot < MAX_PLAYERS) {
                Player *p = &w->players[slot];
                p->sim.move.pos = pos;
                p->sim.move.yaw = quat_get_yaw(rot);
                p->sim.move.pitch = anim;
                p->sim.move.crouched = (w->flags[i] & EF_CROUCHED) != 0;
            }
        }
    }
}
