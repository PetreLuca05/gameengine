#include "replication.h"
#include "protocol.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "game/game_internal.h"
#include "gameplay/characters.h"
#include "core/log.h"
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

static bool always_relevant(EntityType t) { return t == ET_LIGHT || t == ET_GATE || t == ET_PLAYER; }

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
    if (w->comps[i] & C_HEALTH) {
        e->hp = w->health[i].hp;
        e->max_hp = w->health[i].max_hp;
        e->armor = w->health[i].armor;
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
    if (as) { e->anim_state = *as; e->has_anim = true; }
    if (w->type[i] == ET_SWITCH) e->extra.x = (float)w->lever[i].light_group;
    if (w->type[i] == ET_LIGHT) e->extra = v3(w->light[i].flicker, w->light[i].scheduled ? 1.0f : 0.0f, 0.0f);
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

static void write_fields(Writer *w, const NetEnt *e, u32 m) {
    if (m & NF_TYPE) { w_u8(w, e->type); w_u8(w, e->slot); }
    if (m & NF_FLAGS) w_u32(w, e->flags);
    if (m & NF_POS) w_vec3(w, e->pos);
    if (m & NF_ROT) w_quat(w, e->rot);
    if (m & NF_ANIM) w_f32(w, e->anim);
    if (m & NF_ITEM) { w_u16(w, e->item); w_u16(w, e->count); }
    if (m & NF_HP) { w_f32(w, e->hp); w_f32(w, e->max_hp); w_f32(w, e->armor); }
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
    if (m & NF_ROT) e->rot = r_quat(r);
    if (m & NF_ANIM) e->anim = r_f32(r);
    if (m & NF_ITEM) { e->item = r_u16(r); e->count = r_u16(r); }
    if (m & NF_HP) { e->hp = r_f32(r); e->max_hp = r_f32(r); e->armor = r_f32(r); }
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

void repl_write_delta(Writer *wr, const SnapFrame *base, const SnapFrame *cur) {
    // Changes: walk both sorted lists.
    u32 count_pos = wr->len;
    w_u16(wr, 0);
    u16 changes = 0;
    int bi = 0;
    for (int ci = 0; ci < cur->count; ci++) {
        NetEnt e = cur->ents[ci];
        e.rot = quantize_rot(e.rot);
        u32 idx = ENT_INDEX(e.id);
        while (base && bi < base->count && ENT_INDEX(base->ents[bi].id) < idx) bi++;
        const NetEnt *b = (base && bi < base->count && base->ents[bi].id == e.id) ? &base->ents[bi] : NULL;
        u32 m = diff_mask(b, &e);
        if (!m) continue;
        w_u32(wr, e.id);
        w_u16(wr, (u16)m);
        g_write_base = b;
        write_fields(wr, &e, m);
        g_write_base = NULL;
        changes++;
    }
    memcpy(wr->buf + count_pos, &changes, 2);
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
    if (t == ET_PLAYER && e->slot != local_slot) {
        // Body for the remote player (entity_setup gives players only a hitbox).
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_CAPSULE;
        d.shape.radius = PLAYER_RADIUS * 0.95f;
        d.shape.half_height = PLAYER_HEIGHT * 0.5f - PLAYER_RADIUS;
        d.pos = v3_add(e->pos, v3(0, PLAYER_HEIGHT * 0.5f, 0));
        d.rot = quat_identity();
        d.motion = PM_KINEMATIC;
        d.layer = PL_CHARACTER;
        d.user = e->id;
        w->body[i].body = phys_add_body(w->phys, &d);
        w->body[i].motion = PM_KINEMATIC;
        w->comps[i] |= C_BODY;
    }
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
        if (w->comps[i] & C_HEALTH) {
            w->health[i].hp = e->hp;
            w->health[i].max_hp = e->max_hp;
            w->health[i].armor = e->armor;
            if (w->health[i].max_armor <= 0) w->health[i].max_armor = 100;
        }
        if (e->type == ET_LIGHT) {
            w->light[i].color = e->light_color;
            w->light[i].radius = e->light_radius;
            w->light[i].intensity = e->light_intensity;
            w->light[i].group = e->light_group;
            w->light[i].flicker = e->extra.x;
            w->light[i].scheduled = e->extra.y > 0.5f;
        }
        if (e->type == ET_CONTAINER) w->cont[i].opened = (e->flags & EF_OPEN) != 0;
        if (w->comps[i] & C_INTERACT) w->inter[i].enabled = !(e->flags & EF_HIDDEN);
        w->vis[i].color = e->color;
        w->vis[i].emissive = e->emissive;
        dg_strcpy(w->names[i], sizeof(w->names[0]), e->name);
        bool is_local = e->type == ET_PLAYER && e->slot == local_slot;
        if (!is_local) push_sample(&interp[i], e->id, f->tick, e->pos, e->rot, e->anim);
    }
}

void repl_interpolate(World *w, InterpBuffer *interp, double rt, int local_slot) {
    for (int i = 1; i < w->max_used && i < CLIENT_ENTITY_BASE; i++) {
        if (!w->alive[i]) continue;
        if (w->type[i] == ET_PLAYER && w->player_slot[i] == local_slot) continue;
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
        if (c->tick == a->tick || rt >= (double)c->tick) {
            pos = c->pos; rot = c->rot; anim = c->anim;
        } else if (rt <= (double)a->tick) {
            pos = a->pos; rot = a->rot; anim = a->anim;
        } else {
            float t = (float)((rt - (double)a->tick) / (double)(c->tick - a->tick));
            pos = v3_lerp(a->pos, c->pos, t);
            rot = quat_nlerp(a->rot, c->rot, t);
            anim = lerpf(a->anim, c->anim, t);
        }
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
