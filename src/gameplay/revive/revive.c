// Downed / revive / corpse / DNA / fabricator integration (docs/REVIVE.md).
#include "revive.h"
#include "gameplay/health.h"
#include "gameplay/entities.h"
#include "gameplay/inventory.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "gameplay/characters.h"
#include "gameplay/ping.h"
#include "player/player.h"
#include "physics/physics.h"
#include "net/spectate.h"
#include "game/game_internal.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CORPSES 32
#define MAX_FABS 8
#define CORPSE_LENGTH 0.85f          // pelvis -> head (m)
#define FAB_CHAMBER_RADIUS 0.55f

typedef struct CorpseRec {
    EntityId ent;
    CorpseState st;                  // what the player carried is the corpse entity's world inventory (docs/INVENTORY.md)
} CorpseRec;

typedef struct FabRec {
    EntityId ent;
    FabMachine m;
} FabRec;

static struct {
    World *world;
    ReviveRules rules;
    DnaRegistry dna;
    CorpseRec corpses[MAX_CORPSES];
    FabRec fabs[MAX_FABS];
    EntityId downed_by[MAX_PLAYERS];
    bool hurt[MAX_PLAYERS];          // host: damaged since the last action step
    DnaSample sent[DNA_MAX];         // host: registry as last broadcast
    u16 syringe, dna_item;
    float recon_preview;
    Cvar *debug;
} RV = { .recon_preview = -1.0f };

static void load_rules(void) {
    size_t n = 0;
    char *text = platform_read_file("assets/gameplay/revive_rules.json", &n);
    char err[128] = { 0 };
    if (!text || !revive_rules_parse(&RV.rules, text, err, sizeof(err))) {
        revive_rules_defaults(&RV.rules);
        log_warn("revive: assets/gameplay/revive_rules.json %s, defaults", text ? err : "missing");
    }
    free(text);
}

const ReviveRules *revive_rules(void) { return &RV.rules; }
const DnaRegistry *revive_dna(void) { return &RV.dna; }
float revive_recon_preview(void) { return RV.recon_preview; }

static void ensure_items(const World *w) {
    if (!w || !w->items) return;
    if (!RV.syringe) RV.syringe = item_find(w->items, "syringe");
    if (!RV.dna_item) RV.dna_item = item_find(w->items, "dna_sample");
}

void revive_world_reset(World *w) {
    RV.world = w;
    dna_reset(&RV.dna);
    memset(RV.corpses, 0, sizeof(RV.corpses));
    memset(RV.fabs, 0, sizeof(RV.fabs));
    memset(RV.downed_by, 0, sizeof(RV.downed_by));
    memset(RV.hurt, 0, sizeof(RV.hurt));
    memset(RV.sent, 0, sizeof(RV.sent));
    RV.syringe = RV.dna_item = 0;
    ensure_items(w);
}

static void check_world(World *w) {
    if (w != RV.world) revive_world_reset(w);
}

// ------------------------------------------------------------------ lookups

static CorpseRec *corpse_rec(EntityId e) {
    for (int k = 0; k < MAX_CORPSES; k++) if (RV.corpses[k].ent == e && e) return &RV.corpses[k];
    return NULL;
}

static FabRec *fab_rec(EntityId e) {
    for (int k = 0; k < MAX_FABS; k++) if (RV.fabs[k].ent == e && e) return &RV.fabs[k];
    return NULL;
}

static bool is_type(const World *w, EntityId e, EntityType t) { return ent_valid(w, e) && w->type[ENT_INDEX(e)] == t; }

void revive_corpse_axis(const World *w, EntityId corpse, vec3 *pelvis, vec3 *head) {
    int i = (int)ENT_INDEX(corpse);
    *pelvis = w->xf[i].pos;
    *head = v3_madd(*pelvis, forward_from_yaw(quat_get_yaw(w->xf[i].rot)), CORPSE_LENGTH);
}

static int corpse_owner(const World *w, int i) { return w->item[i] < MAX_PLAYERS ? (int)w->item[i] : -1; }

typedef struct FabSocket { const char *name; vec3 local; } FabSocket;
// Local frame: -Z = the front. The chamber stands at the origin, the control column to its right.
static const FabSocket k_fab_sockets[] = {
    { "chamber_center", { 0.0f, 1.15f, 0.0f } },
    { "spawn_point", { 0.0f, 0.06f, 0.0f } },
    { "panel", { 0.85f, 1.38f, -0.37f } },
    { "sample_slot", { 0.85f, 1.06f, -0.37f } },
};

bool revive_fab_socket(const World *w, EntityId fab, const char *name, vec3 *pos, quat *rot) {
    if (!is_type(w, fab, ET_FABRICATOR)) return false;
    int i = (int)ENT_INDEX(fab);
    for (int k = 0; k < ARRAY_COUNT(k_fab_sockets); k++) {
        if (strcmp(k_fab_sockets[k].name, name)) continue;
        if (pos) *pos = v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, k_fab_sockets[k].local));
        if (rot) *rot = w->xf[i].rot;
        return true;
    }
    return false;
}

bool revive_fab_view(const World *w, EntityId fab, FabView *out) {
    if (!is_type(w, fab, ET_FABRICATOR)) return false;
    int i = (int)ENT_INDEX(fab);
    memset(out, 0, sizeof(*out));
    FabMachine *m = &out->m;
    m->state = (u8)(w->aux_count[i] & 0xFF);
    m->qn = (u8)DG_MIN(w->aux_count[i] >> 8, FAB_QUEUE_MAX);
    m->current = (u8)w->item[i];
    memcpy(m->queue, w->attach[i], FAB_QUEUE_MAX);
    m->state_tick = (u32)w->aux_extra[i].x;
    m->powered = w->aux_extra[i].y > 0.5f;
    out->current_player = (int)w->aux_extra[i].z;
    dg_strcpy(out->current_name, sizeof(out->current_name), w->names[i]);
    out->progress = fab_progress(m, &RV.rules, w->tick);
    out->remaining = fab_remaining(m, &RV.rules, w->tick);
    return true;
}

static bool has_dna(const Player *p) { return RV.dna_item && inv_has(&p->inv, RV.dna_item); }

static InvItem *find_dna(Player *p) {
    if (!RV.dna_item) return NULL;
    // The equipped hotbar slot first, then anywhere in the inventory.
    int eq = p->sim.wpn.slot;
    if (eq < INV_HOTBAR && inv_hot(&p->inv, eq)->item == RV.dna_item) return inv_item_of(&p->inv, inv_hot(&p->inv, eq));
    return inv_find(&p->inv, RV.dna_item);
}

// ------------------------------------------------------------------ action affordance (shared)

// What target t offers to player slot right now, from replicated state only (so the client
// predicts the same decision the host makes). dur = ticks for a start.
static ActKind afford(const World *w, int slot, EntityId t, u16 *dur) {
    const ReviveRules *r = &RV.rules;
    const Player *p = &w->players[slot];
    *dur = 0;
    if (!ent_valid(w, t) || p->sim.down.downed || p->sim.move.mode != MOVE_WALK) return RACT_NONE;
    int i = (int)ENT_INDEX(t);
    vec3 feet = p->sim.move.pos;
    switch (w->type[i]) {
    case ET_PLAYER: {
        if (t == p->ent || !(w->flags[i] & EF_DOWNED) || (w->flags[i] & EF_DEAD)) return RACT_NONE;
        vec3 d = v3_sub(w->xf[i].pos, feet);
        if (d.x * d.x + d.z * d.z > r->revive_range * r->revive_range || fabsf(d.y) > 1.2f) return RACT_NONE;
        *dur = revive_ticks(r->revive_time);
        return RACT_REVIVE;
    }
    case ET_CORPSE: {
        if (!(w->flags[i] & EF_DNA) || corpse_owner(w, i) == slot) return RACT_NONE;
        vec3 a, b;
        revive_corpse_axis(w, t, &a, &b);
        if (!corpse_in_range(r, feet, a, b)) return RACT_NONE;
        *dur = revive_ticks(r->extract_time);
        return RACT_EXTRACT;
    }
    case ET_FABRICATOR: {
        if (!has_dna(p)) return RACT_NONE;
        FabView fv;
        revive_fab_view(w, t, &fv);
        if (fv.m.qn >= r->queue_max || (r->require_power && !fv.m.powered)) return RACT_NONE;
        vec3 slot_pos;
        revive_fab_socket(w, t, "sample_slot", &slot_pos, NULL);
        vec3 d = v3_sub(slot_pos, feet);
        if (d.x * d.x + d.z * d.z > r->insert_range * r->insert_range) return RACT_NONE;
        *dur = revive_ticks(r->insert_time);
        return RACT_INSERT;
    }
    default:
        return RACT_NONE;
    }
}

u16 revive_held_override(const World *w, int slot) {
    const ActionState *a = &w->players[slot].sim.act;
    if (a->phase != ACTP_RUNNING) return 0;
    if (a->kind == RACT_EXTRACT) return RV.syringe;
    if (a->kind == RACT_INSERT) return RV.dna_item;
    return 0;
}

// ------------------------------------------------------------------ events

static void push_event(World *w, int kind, EntityId ent, EntityId ent2, float value, vec3 pos, const char *text, int target) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_REVIVE;
    ev.target = (u8)target;
    ev.except = EVENT_ALL;
    ev.id = (u32)kind;
    ev.ent = ent;
    ev.ent2 = ent2;
    ev.value = value;
    ev.pos = pos;
    if (text) dg_strcpy(ev.text, sizeof(ev.text), text);
    ev_push(w, &ev);
}

static void send_dna(World *w, int id, int target) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_DNA;
    ev.target = (u8)target;
    ev.except = EVENT_ALL;
    ev.id = (u32)id;
    memcpy(ev.data, &RV.dna.s[id - 1], sizeof(DnaSample));
    ev_push(w, &ev);
}

// Host: broadcast registry entries that changed since the last call.
static void sync_dna(World *w) {
    for (int k = 0; k < DNA_MAX; k++) {
        if (!memcmp(&RV.sent[k], &RV.dna.s[k], sizeof(DnaSample))) continue;
        RV.sent[k] = RV.dna.s[k];
        send_dna(w, k + 1, EVENT_ALL);
    }
}

// ------------------------------------------------------------------ host effects

static void set_flag(World *w, EntityId e, u32 f, bool on) {
    int i = (int)ENT_INDEX(e);
    if (on) w->flags[i] |= f;
    else w->flags[i] &= ~f;
}

static void drop_slot(World *w, int slot, InvItem *it) {
    Player *p = &w->players[slot];
    if (!it || !inv_alive(it)) return;
    vec3 at = v3_add(v3_madd(p->sim.move.pos, forward_from_yaw(p->sim.move.yaw), 0.5f), v3(0, 0.6f, 0));
    spawn_pickup_tree(w, at, &p->inv.t, it->uid);   // whole: a weapon with its attachments (a container with its contents)
    inv_free_subtree(&p->inv.t, it->uid);
    p->inv.revision++;
}

static bool is_sidearm(const World *w, u16 item) {
    const WeaponDef *wd = item ? item_weapon(w->items, item) : NULL;
    return wd && !strcmp(wd->hold_style, "pistol");
}

bool revive_sv_down(World *w, int slot, EntityId attacker) {
    check_world(w);
    Player *p = &w->players[slot];
    if (!p->active || p->sim.move.mode == MOVE_DEAD || p->sim.down.downed) return false;
    const ReviveRules *r = &RV.rules;
    int i = (int)ENT_INDEX(p->ent);
    down_enter(&p->sim.down, r);
    act_cancel(&p->sim.act, ACTC_HOST);
    p->hold_target = ENT_NONE;
    p->hold_progress = 0;
    if (p->sim.move.mode == MOVE_HANG || p->sim.move.mode == MOVE_MANTLE || p->sim.move.mode == MOVE_VAULT) p->sim.move.mode = MOVE_WALK;
    w->health[i].hp = fmaxf(1.0f, r->downed_hp);
    w->health[i].armor = 0;
    w->flags[i] |= EF_DOWNED;
    RV.downed_by[slot] = attacker;
    // The held item drops, unless it is a sidearm the rules let the player keep.
    int eq = p->sim.wpn.slot;
    if (eq < INV_HOTBAR && inv_hot(&p->inv, eq)->item && !(r->keep_sidearm && is_sidearm(w, inv_hot(&p->inv, eq)->item)))
        drop_slot(w, slot, inv_item_of(&p->inv, inv_hot(&p->inv, eq)));
    push_event(w, RVE_DOWNED, p->ent, attacker, RV.rules.bleed_time, p->sim.move.pos, p->name, EVENT_ALL);
    ev_sound(w, "downed", p->sim.move.pos, 0.9f, EVENT_ALL);
    if (r->auto_ping_down) game_sv_auto_ping(w, slot, PING_HELP, v3_add(p->sim.move.pos, v3(0, 0.9f, 0)));
    log_info("revive: %s is down (bleed-out %.0f s)", p->name, (float)p->sim.down.bleed_total / REVIVE_HZ);
    return true;
}

bool revive_sv_try_down(World *w, int slot, u32 dmg_flags, float overkill, EntityId attacker) {
    check_world(w);
    Player *p = &w->players[slot];
    if (!down_should_knock(&RV.rules, &p->sim.down, (dmg_flags & DMGF_KILL) != 0, overkill)) return false;
    return revive_sv_down(w, slot, attacker);
}

bool revive_sv_revive(World *w, int slot, int reviver) {
    Player *p = &w->players[slot];
    if (!p->active || !p->sim.down.downed) return false;
    int i = (int)ENT_INDEX(p->ent);
    down_revived(&p->sim.down, &RV.rules);
    p->sim.move.crouch_toggled = 0;
    w->flags[i] &= ~(u32)EF_DOWNED;
    w->health[i].hp = fminf(w->health[i].max_hp, fmaxf(1.0f, RV.rules.revive_hp));
    EntityId by = reviver >= 0 && reviver < MAX_PLAYERS ? w->players[reviver].ent : ENT_NONE;
    push_event(w, RVE_REVIVED, p->ent, by, 0, p->sim.move.pos, reviver >= 0 ? w->players[reviver].name : "", EVENT_ALL);
    ev_sound(w, "revived", p->sim.move.pos, 0.8f, EVENT_ALL);
    log_info("revive: %s revived%s%s", p->name, reviver >= 0 ? " by " : "", reviver >= 0 ? w->players[reviver].name : "");
    return true;
}

void revive_sv_on_damage(World *w, int slot) {
    if (slot >= 0 && slot < MAX_PLAYERS) RV.hurt[slot] = true;
    (void)w;
}

EntityId revive_spawn_corpse(World *w, int slot, vec3 pos, float yaw) {
    check_world(w);
    CorpseRec *rec = NULL;
    for (int k = 0; k < MAX_CORPSES && !rec; k++) if (!RV.corpses[k].ent || !ent_valid(w, RV.corpses[k].ent)) rec = &RV.corpses[k];
    if (!rec) {
        // Full: the oldest body goes.
        rec = &RV.corpses[0];
        for (int k = 1; k < MAX_CORPSES; k++) if (RV.corpses[k].st.death_tick < rec->st.death_tick) rec = &RV.corpses[k];
        if (ent_valid(w, rec->ent)) ent_destroy(w, rec->ent);
    }
    memset(rec, 0, sizeof(*rec));
    EntityId e = ent_spawn(w, ET_CORPSE);
    if (!e) return ENT_NONE;
    int i = (int)ENT_INDEX(e);
    const Player *p = slot >= 0 && slot < MAX_PLAYERS ? &w->players[slot] : NULL;
    w->xf[i].pos = w->xf[i].prev_pos = pos;
    w->xf[i].rot = w->xf[i].prev_rot = quat_yaw(yaw);
    w->vis[i].shape = VIS_NONE;
    w->vis[i].half = v3(0.25f, 0.15f, 0.45f);
    w->comps[i] |= C_VISUAL;
    w->item[i] = (u16)(slot >= 0 ? slot : 0);
    w->flags[i] = EF_DEAD | EF_DNA;
    if (p && p->active) {
        dg_strcpy(w->names[i], sizeof(w->names[0]), p->name);
        w->char_id[i] = w->char_id[ENT_INDEX(p->ent)];
    } else {
        snprintf(w->names[i], sizeof(w->names[0]), "Player %d", slot + 1);
    }
    rec->ent = e;
    corpse_init(&rec->st, &RV.rules, slot, w->tick);
    w->aux_count[i] = rec->st.samples_left;
    w->aux_extra[i] = v3((float)rec->st.death_tick, 0.0f, -1.0f);
    entity_setup(w, e);
    return e;
}

void revive_sv_on_death(World *w, int slot) {
    check_world(w);
    Player *p = &w->players[slot];
    const ReviveRules *r = &RV.rules;
    int i = (int)ENT_INDEX(p->ent);
    bool was_downed = p->sim.down.downed;
    down_clear(&p->sim.down);
    p->sim.down.downs = 0;
    memset(&p->sim.act, 0, sizeof(p->sim.act));
    w->flags[i] &= ~(u32)EF_DOWNED;
    // Someone reviving this player stops.
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *o = &w->players[s];
        if (o->active && o->sim.act.phase == ACTP_RUNNING && o->sim.act.target == p->ent) act_cancel(&o->sim.act, ACTC_TARGET);
    }
    // Carried DNA samples (rule): dropped where the carrier fell, lost, or kept on the body.
    for (int s = 0; s < INV_MAX_ITEMS && RV.dna_item; s++) {
        InvItem *it = &p->inv.t.items[s];
        if (!inv_alive(it) || it->s.item != RV.dna_item) continue;
        if (inv_is_ancestor(&p->inv.t, inv_equipped(&p->inv.t, EQ_SECURE) ? inv_equipped(&p->inv.t, EQ_SECURE)->uid : 0xFFFF, it->uid)) continue;   // safe
        DnaSample *ds = dna_get(&RV.dna, it->s.ammo);
        if (r->sample_on_death == SAMPLE_LOST) {
            if (ds) dna_free(&RV.dna, it->s.ammo);
            memset(it, 0, sizeof(*it));
            p->inv.revision++;
        } else if (r->sample_on_death == SAMPLE_DROP) {
            drop_slot(w, slot, it);
            if (ds) { ds->status = DNA_DROPPED; ds->carrier = REVIVE_NOBODY; }
        }
    }
    float yaw = p->sim.move.body_yaw;
    EntityId c = revive_spawn_corpse(w, slot, p->sim.move.pos, yaw);
    // What the player carried stays on the body (lootable, docs/INVENTORY.md), except the secure
    // container and its contents.
    if (ent_valid(w, c)) {
        container_set_root(w, c, "body");
        InvTree *ct = container_tree(w, c, true);
        if (ct) {
            inv_strip_to_corpse(&p->inv.t, p->inv.quick, INV_HOTBAR, ct, w->items);
            w->cont[ENT_INDEX(c)].revision++;
            p->inv.revision++;
        }
    }
    if (r->auto_ping_death && ent_valid(w, c)) game_sv_auto_ping(w, slot, PING_HELP, v3_add(p->sim.move.pos, v3(0, 0.3f, 0)));
    if (was_downed) push_event(w, RVE_BLED_OUT, p->ent, ENT_NONE, 0, p->sim.move.pos, p->name, EVENT_ALL);
    sync_dna(w);
}

void revive_sv_on_respawn(World *w, int slot) {
    check_world(w);
    Player *p = &w->players[slot];
    down_clear(&p->sim.down);
    memset(&p->sim.act, 0, sizeof(p->sim.act));
    p->sim.frozen_ticks = 0;
    if (ent_valid(w, p->ent)) w->flags[ENT_INDEX(p->ent)] &= ~(u32)EF_DOWNED;
    RV.downed_by[slot] = ENT_NONE;
}

bool revive_sv_timer_respawn_allowed(const World *w, int slot) {
    (void)w;
    (void)slot;
    return RV.rules.respawn_mode != RESPAWN_FABRICATOR;
}

void revive_sv_player_joined(World *w, int slot) {
    check_world(w);
    for (int k = 0; k < DNA_MAX; k++) if (RV.dna.s[k].status != DNA_FREE) send_dna(w, k + 1, slot);
}

void revive_sv_player_left(World *w, int slot) {
    check_world(w);
    // Samples of a player who left can never be reconstructed: dropped from queues on the next tick.
    (void)slot;
}

static void kill_downed(World *w, int slot) {
    Player *p = &w->players[slot];
    EntityId by = RV.downed_by[slot];
    apply_damage_ex(w, p->ent, 100000.0f, DMG_GENERIC, ent_valid(w, by) ? by : p->ent, v3_add(p->sim.move.pos, v3(0, 0.5f, 0)), v3(0, -1, 0), -1, 0.0f, DMGF_KILL);
}

static void action_done(World *w, int slot, const ActionState *a) {
    Player *p = &w->players[slot];
    const ReviveRules *r = &RV.rules;
    switch ((ActKind)a->kind) {
    case RACT_REVIVE: {
        int ts = world_player_by_ent(w, a->target);
        if (ts >= 0) revive_sv_revive(w, ts, slot);
        break;
    }
    case RACT_EXTRACT: {
        CorpseRec *rec = corpse_rec(a->target);
        if (!rec || !corpse_extract(&rec->st, slot, w->tick)) break;
        int ci = (int)ENT_INDEX(a->target);
        int owner = rec->st.owner_slot;
        u32 avatar = owner < MAX_PLAYERS && w->players[owner].active ? (u32)w->char_id[ENT_INDEX(w->players[owner].ent)] : 0;
        int id = dna_alloc(&RV.dna, owner, w->names[ci], slot, avatar, a->target);
        w->aux_count[ci] = rec->st.samples_left;
        w->aux_extra[ci] = v3((float)rec->st.death_tick, (float)rec->st.extract_tick, (float)slot);
        if (!rec->st.samples_left) w->flags[ci] &= ~(u32)EF_DNA;
        if (!id) { ev_hint(w, slot, "Too many DNA samples"); break; }
        int left = inv_add(&p->inv, w->items, RV.dna_item, 1, (i16)id);
        p->inv.revision++;
        if (left) {
            spawn_pickup(w, v3_add(p->sim.move.pos, v3(0, 0.3f, 0)), RV.dna_item, 1, (i16)id, 0);
            dna_get(&RV.dna, id)->status = DNA_DROPPED;
            dna_get(&RV.dna, id)->carrier = REVIVE_NOBODY;
            ev_hint(w, slot, "Inventory full: the sample is on the ground");
        }
        push_event(w, RVE_EXTRACTED, a->target, p->ent, (float)id, w->xf[ci].pos, p->name, EVENT_ALL);
        ev_sound(w, "syringe", w->xf[ci].pos, 0.7f, EVENT_ALL);
        log_info("revive: %s extracted DNA of %s (sample %d)", p->name, w->names[ci], id);
        break;
    }
    case RACT_INSERT: {
        FabRec *fr = fab_rec(a->target);
        InvItem *dit = find_dna(p);
        if (!fr || !dit) break;
        int id = dit->s.ammo;
        DnaSample *ds = dna_get(&RV.dna, id);
        if (!ds || !fab_enqueue(&fr->m, r, id)) { ev_hint(w, slot, "The fabricator can't take it"); break; }
        inv_free_subtree(&p->inv.t, dit->uid);
        p->inv.revision++;
        ds->status = DNA_IN_FABRICATOR;
        ds->carrier = REVIVE_NOBODY;
        vec3 at;
        revive_fab_socket(w, a->target, "sample_slot", &at, NULL);
        push_event(w, RVE_INSERTED, a->target, p->ent, (float)id, at, ds->name, EVENT_ALL);
        ev_sound(w, "fab_insert", at, 0.8f, EVENT_ALL);
        log_info("revive: %s inserted the sample of %s (queue %d)", p->name, ds->name, fr->m.qn);
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------ shared simulation

static void force_crouch(PlayerSim *sim, UserCmd *c) {
    if (player_tuning()->crouch_toggle->i) {
        sim->move.crouch_toggled = 1;
        c->buttons &= (u16)~BTN_CROUCH;
    } else {
        c->buttons |= BTN_CROUCH;
    }
}

void revive_sim_pre(World *w, int slot, UserCmd *c, u16 pressed, float dt, bool first) {
    (void)dt;
    (void)first;
    check_world(w);
    ensure_items(w);
    Player *p = &w->players[slot];
    PlayerSim *sim = &p->sim;
    const ReviveRules *r = &RV.rules;
    if (sim->move.mode == MOVE_DEAD) {
        memset(&sim->act, 0, sizeof(sim->act));
        sim->frozen_ticks = 0;
        return;
    }
    // Forced idle after a fabricator respawn (the chamber opens).
    if (sim->frozen_ticks) {
        sim->frozen_ticks--;
        c->move_x = c->move_y = 0;
        c->buttons &= (u16)(BTN_FLASHLIGHT | BTN_PHONE | BTN_VEH_CAM);
    }
    // ---- timed action
    ActionState *a = &sim->act;
    ActInput in;
    memset(&in, 0, sizeof(in));
    in.use_down = (c->buttons & BTN_USE) != 0;
    in.use_pressed = (pressed & BTN_USE) != 0;
    in.target = c->use_target;
    in.afford = afford(w, slot, (EntityId)c->use_target, &in.dur);
    u16 dur_locked = 0;
    in.target_ok = a->phase == ACTP_RUNNING ? afford(w, slot, a->target, &dur_locked) == (ActKind)a->kind : in.afford != RACT_NONE;
    in.pos = sim->move.pos;
    in.move_tolerance = r->move_tolerance;
    if (w->authority) {
        in.damaged = RV.hurt[slot] && a->phase == ACTP_RUNNING;
        RV.hurt[slot] = false;
    }
    u8 was_kind = a->kind;
    EntityId was_target = a->target;
    ActResult res = act_step(a, &in);
    if (w->authority) {
        // The downed player's ring follows the reviver's progress (both HUDs).
        if (a->kind == RACT_REVIVE || was_kind == RACT_REVIVE) {
            int ts = world_player_by_ent(w, a->kind == RACT_REVIVE ? a->target : was_target);
            if (ts >= 0) {
                DownState *td = &w->players[ts].sim.down;
                if (a->phase == ACTP_RUNNING) {
                    td->reviver = (u8)slot;
                    td->revive_ticks = a->ticks;
                    td->revive_total = a->dur;
                } else if (td->reviver == slot) {
                    td->reviver = REVIVE_NOBODY;
                    td->revive_ticks = td->revive_total = 0;
                }
            }
        }
        if (res == ACTR_DONE) action_done(w, slot, a);
        if (res == ACTR_CANCELLED && a->cancel != ACTC_RELEASE) push_event(w, RVE_CANCELLED, p->ent, a->target, (float)a->cancel, sim->move.pos, act_cancel_name((ActCancel)a->cancel), slot);
    }
    if (a->phase == ACTP_RUNNING) {
        // Busy hands: no weapon, no jumping or sprinting; extracting needs a crouch.
        c->buttons &= (u16)~(BTN_ATTACK | BTN_AIM | BTN_RELOAD | BTN_JUMP | BTN_SPRINT | BTN_FIREMODE);
        if (a->kind == RACT_EXTRACT && r->extract_needs_crouch) force_crouch(sim, c);
    }
    // ---- downed: crawl, look around, maybe a sidearm, no interaction
    DownState *d = &sim->down;
    if (d->downed) {
        c->buttons &= (u16)~(BTN_JUMP | BTN_SPRINT | BTN_USE | BTN_VEHICLE | BTN_NOCLIP);
        c->use_target = 0;
        const InvSlot *eq = weapon_equipped(w, slot);
        bool sidearm = eq && is_sidearm(w, eq->item) && r->keep_sidearm && r->downed_can_shoot;
        if (!sidearm) c->buttons &= (u16)~(BTN_ATTACK | BTN_AIM | BTN_RELOAD);
        force_crouch(sim, c);
        float crouch = player_tuning()->crouch_speed->f;
        float k = crouch > 0.01f ? DG_CLAMP(r->downed_move_speed / crouch, 0.0f, 1.0f) : 0.5f;
        c->move_x = (i8)lrintf((float)c->move_x * k);
        c->move_y = (i8)lrintf((float)c->move_y * k);
    }
}

// ------------------------------------------------------------------ host tick

static void fab_respawn(World *w, FabRec *fr, int slot) {
    Player *p = &w->players[slot];
    const ReviveRules *r = &RV.rules;
    vec3 pos;
    quat rot;
    revive_fab_socket(w, fr->ent, "spawn_point", &pos, &rot);
    // The old inventory went onto the body at death (the corpse's world inventory); the secure
    // container stays; the new body gets the rule items.
    for (int k = 0; k < r->respawn_item_n; k++) {
        u16 it = item_find(w->items, r->respawn_items[k]);
        if (it) inv_add(&p->inv, w->items, it, r->respawn_item_counts[k], 0);
    }
    p->inv.revision++;
    game_respawn_player_at(w, slot, pos, quat_get_yaw(rot), r->respawn_hp);
    p->sim.frozen_ticks = revive_ticks(r->open_time);
    push_event(w, RVE_RECON_DONE, fr->ent, p->ent, 0, pos, p->name, EVENT_ALL);
    ev_sound(w, "fab_done", pos, 1.0f, EVENT_ALL);
    log_info("revive: %s reconstructed", p->name);
}

static bool sample_valid(World *w, int id) {
    const DnaSample *ds = dna_get(&RV.dna, id);
    if (!ds) return false;
    const Player *p = ds->player_slot < MAX_PLAYERS ? &w->players[ds->player_slot] : NULL;
    return p && p->active && p->sim.move.mode == MOVE_DEAD;
}

static void mirror_fab(World *w, FabRec *fr) {
    int i = (int)ENT_INDEX(fr->ent);
    w->aux_count[i] = (u16)(fr->m.state | (fr->m.qn << 8));
    w->item[i] = fr->m.current;
    memcpy(w->attach[i], fr->m.queue, FAB_QUEUE_MAX);
    const DnaSample *ds = fr->m.current ? dna_get(&RV.dna, fr->m.current) : NULL;
    w->aux_extra[i] = v3((float)fr->m.state_tick, fr->m.powered ? 1.0f : 0.0f, ds ? (float)ds->player_slot : -1.0f);
    dg_strcpy(w->names[i], sizeof(w->names[0]), ds ? ds->name : "");
    set_flag(w, fr->ent, EF_ON, fr->m.powered != 0);
}

static void tick_fab(World *w, FabRec *fr) {
    const ReviveRules *r = &RV.rules;
    // Samples whose player is back (timed respawn) or gone are dropped from the queue.
    for (int q = fr->m.qn - 1; q >= 0; q--) {
        int id = fr->m.queue[q];
        if (sample_valid(w, id)) continue;
        fab_remove(&fr->m, id);
        dna_free(&RV.dna, id);
    }
    if (fr->m.state == FAB_RECONSTRUCTING && !sample_valid(w, fr->m.current)) {
        dna_free(&RV.dna, fr->m.current);
        fab_abort(&fr->m, w->tick);
    }
    int sample = fr->m.current;
    FabEvent e = fab_step(&fr->m, r, w->tick);
    vec3 chamber;
    revive_fab_socket(w, fr->ent, "chamber_center", &chamber, NULL);
    switch (e) {
    case FABE_RECON_START: {
        DnaSample *ds = dna_get(&RV.dna, fr->m.current);
        if (ds) ds->status = DNA_RECONSTRUCTING;
        push_event(w, RVE_RECON_START, fr->ent, ENT_NONE, (float)fr->m.current, chamber, ds ? ds->name : "", EVENT_ALL);
        ev_sound(w, "fab_start", chamber, 1.0f, EVENT_ALL);
        break;
    }
    case FABE_RESPAWN: {
        const DnaSample *ds = dna_get(&RV.dna, sample);
        int ps = ds ? ds->player_slot : -1;
        if (ps >= 0) fab_respawn(w, fr, ps);
        dna_free(&RV.dna, sample);
        break;
    }
    default:
        break;
    }
    mirror_fab(w, fr);
}

static void tick_corpse(World *w, CorpseRec *rec) {
    int i = (int)ENT_INDEX(rec->ent);
    if (corpse_should_despawn(&rec->st, &RV.rules, w->tick)) {
        ent_destroy(w, rec->ent);
        memset(rec, 0, sizeof(*rec));
        return;
    }
    // Follow the body: the owner's ragdoll while it is dead, the adopted one afterwards.
    vec3 pelvis, head;
    int owner = rec->st.owner_slot;
    bool got = chars_body_axis(w, rec->ent, &pelvis, &head);
    if (!got && owner < MAX_PLAYERS && w->players[owner].active && w->players[owner].sim.move.mode == MOVE_DEAD)
        got = chars_body_axis(w, w->players[owner].ent, &pelvis, &head);
    if (got) {
        vec3 d = v3_sub(head, pelvis);
        float yaw = atan2f(-d.x, -d.z);
        if (v3_dist(pelvis, w->xf[i].pos) > 0.02f || fabsf(wrap_angle(yaw - quat_get_yaw(w->xf[i].rot))) > 0.05f) {
            w->xf[i].pos = pelvis;
            w->xf[i].rot = quat_yaw(yaw);
        }
    }
}

// Host: who carries which sample (inventories, pickups, containers), every quarter second.
static void refresh_carriers(World *w) {
    if (!RV.dna_item) return;
    u8 seen[DNA_MAX];
    memset(seen, 0, sizeof(seen));
    for (int s = 0; s < MAX_PLAYERS; s++) {
        const Player *p = &w->players[s];
        if (!p->active) continue;
        for (int k = 0; k < INV_MAX_ITEMS; k++) {
            const InvSlot *is = &p->inv.t.items[k].s;
            if (!inv_alive(&p->inv.t.items[k]) || is->item != RV.dna_item) continue;
            DnaSample *ds = dna_get(&RV.dna, is->ammo);
            if (!ds || seen[is->ammo - 1]) continue;
            seen[is->ammo - 1] = 1;
            ds->status = DNA_CARRIED;
            ds->carrier = (u8)s;
        }
    }
    ENT_FOREACH(w, i) {
        if (w->type[i] == ET_PICKUP && w->pickup[i].item == RV.dna_item) {
            DnaSample *ds = dna_get(&RV.dna, w->pickup[i].ammo);
            if (ds && !seen[w->pickup[i].ammo - 1]) { seen[w->pickup[i].ammo - 1] = 1; ds->status = DNA_DROPPED; ds->carrier = REVIVE_NOBODY; }
        }
        InvTree *ct = container_tree(w, ent_from_index(w, i), false);   // crates, corpses, trunks
        if (ct) {
            for (int k = 0; k < INV_MAX_ITEMS; k++) {
                const InvSlot *is = &ct->items[k].s;
                DnaSample *ds = inv_alive(&ct->items[k]) && is->item == RV.dna_item ? dna_get(&RV.dna, is->ammo) : NULL;
                if (ds && !seen[is->ammo - 1]) { seen[is->ammo - 1] = 1; ds->status = DNA_DROPPED; ds->carrier = REVIVE_NOBODY; }
            }
        }
    }
    // Carried / dropped samples nobody holds any more (used, destroyed with a pickup) are freed.
    for (int k = 0; k < DNA_MAX; k++) {
        DnaSample *ds = &RV.dna.s[k];
        if ((ds->status == DNA_CARRIED || ds->status == DNA_DROPPED) && !seen[k]) dna_free(&RV.dna, k + 1);
    }
}

void revive_host_tick(World *w) {
    check_world(w);
    ensure_items(w);
    // Bleed-out runs on the host clock (a stalled client still bleeds; owners see it in PlayerSim).
    for (int s = 0; s < MAX_PLAYERS; s++) {
        Player *p = &w->players[s];
        if (p->active && p->sim.down.downed && p->sim.move.mode != MOVE_DEAD && down_step(&p->sim.down, &RV.rules)) kill_downed(w, s);
    }
    for (int k = 0; k < MAX_CORPSES; k++) {
        CorpseRec *c = &RV.corpses[k];
        if (!c->ent) continue;
        if (!ent_valid(w, c->ent)) { memset(c, 0, sizeof(*c)); continue; }
        tick_corpse(w, c);
    }
    for (int k = 0; k < MAX_FABS; k++) {
        FabRec *f = &RV.fabs[k];
        if (!f->ent) continue;
        if (!ent_valid(w, f->ent)) { memset(f, 0, sizeof(*f)); continue; }
        tick_fab(w, f);
    }
    if (w->tick % 15 == 0) refresh_carriers(w);
    sync_dna(w);
}

void revive_frame(World *w) {
    check_world(w);
    ensure_items(w);
    ENT_FOREACH(w, i) {
        if (!(w->comps[i] & C_INTERACT)) continue;
        // Players are interactable only while knocked down (the revive prompt).
        if (w->type[i] == ET_PLAYER) w->inter[i].enabled = (w->flags[i] & EF_DOWNED) && !(w->flags[i] & (EF_DEAD | EF_HIDDEN));
    }
}

// ------------------------------------------------------------------ entities

static bool fab_power(World *w, FabRec *fr, bool on) {
    u8 ej[FAB_QUEUE_MAX + 1];
    int n = fab_set_power(&fr->m, &RV.rules, on, w->tick, ej, ARRAY_COUNT(ej));
    vec3 at;
    revive_fab_socket(w, fr->ent, "sample_slot", &at, NULL);
    for (int k = 0; k < n; k++) {
        DnaSample *ds = dna_get(&RV.dna, ej[k]);
        if (!ds) continue;
        // Ejected: the vial pops out of the slot.
        spawn_pickup(w, v3_add(at, v3(0, 0.1f, 0)), RV.dna_item, 1, (i16)ej[k], 0);
        ds->status = DNA_DROPPED;
        push_event(w, RVE_EJECTED, fr->ent, ENT_NONE, (float)ej[k], at, ds->name, EVENT_ALL);
    }
    mirror_fab(w, fr);
    return n > 0;
}

EntityId revive_spawn_fabricator(World *w, vec3 pos, float yaw, bool powered) {
    check_world(w);
    FabRec *fr = NULL;
    for (int k = 0; k < MAX_FABS && !fr; k++) if (!RV.fabs[k].ent || !ent_valid(w, RV.fabs[k].ent)) fr = &RV.fabs[k];
    if (!fr) { log_warn("revive: fabricator limit (%d)", MAX_FABS); return ENT_NONE; }
    EntityId e = ent_spawn(w, ET_FABRICATOR);
    if (!e) return ENT_NONE;
    int i = (int)ENT_INDEX(e);
    w->xf[i].pos = w->xf[i].prev_pos = pos;
    w->xf[i].rot = w->xf[i].prev_rot = quat_yaw(yaw);
    w->vis[i].shape = VIS_NONE;   // drawn by revive_render (pod, glass, panel)
    w->vis[i].half = v3(0.9f, 1.15f, 0.7f);
    w->comps[i] |= C_VISUAL;
    memset(fr, 0, sizeof(*fr));
    fr->ent = e;
    fab_init(&fr->m, powered);
    mirror_fab(w, fr);
    entity_setup(w, e);
    return e;
}

// Replicas and the host: the control column body (focus raycasts hit it, players bump into it).
void revive_entity_setup(World *w, EntityId e) {
    int i = (int)ENT_INDEX(e);
    if (w->type[i] == ET_FABRICATOR) {
        BodyDesc d;
        memset(&d, 0, sizeof(d));
        d.shape.kind = SHAPE_BOX;
        d.shape.half = v3(0.25f, 0.95f, 0.35f);
        d.pos = v3_add(w->xf[i].pos, quat_rotate(w->xf[i].rot, v3(0.85f, 0.95f, 0.0f)));
        d.rot = w->xf[i].rot;
        d.motion = PM_STATIC;
        d.layer = PL_STATIC;
        d.user = e;
        w->body[i].body = phys_add_body(w->phys, &d);
        w->body[i].motion = PM_STATIC;
        w->comps[i] |= C_BODY;
        w->inter[i].range = 2.4f;
        w->inter[i].focus_offset = k_fab_sockets[3].local;
    }
    if (w->type[i] == ET_CORPSE) {
        w->inter[i].range = 2.3f;
        w->inter[i].focus_offset = v3(0.0f, 0.1f, -CORPSE_LENGTH * 0.45f);
        w->vis[i].half = v3(0.25f, 0.15f, 0.45f);
    }
}

// ------------------------------------------------------------------ interactables

static void noop_use(World *w, EntityId self, int player) { (void)w; (void)self; (void)player; }

static void revive_focus(World *w, EntityId self, int player, char *prompt, int size) {
    u16 dur;
    int i = (int)ENT_INDEX(self);
    if (afford(w, player, self, &dur) == RACT_REVIVE) snprintf(prompt, (size_t)size, "Revive %s (hold)", w->names[i]);
    else if (w->players[player].sim.down.downed) prompt[0] = 0;
    else snprintf(prompt, (size_t)size, "%s is down: get closer", w->names[i]);
}

// A body is searched with a tap of use (its world inventory: what the player carried, docs/INVENTORY.md)
// whenever the DNA extraction isn't on offer (taken, your own body, too far for the syringe).
static void corpse_use(World *w, EntityId self, int player) {
    u16 dur;
    if (!w->authority || afford(w, player, self, &dur) == RACT_EXTRACT) return;   // host; the timed action runs instead
    InvTree *t = container_tree(w, self, false);
    if (t && inv_live_count(t)) container_send(w, self, player);
}

static void corpse_focus(World *w, EntityId self, int player, char *prompt, int size) {
    int i = (int)ENT_INDEX(self);
    u16 dur;
    InvTree *t = container_tree(w, self, false);
    bool loot = t && inv_live_count(t) > 0;
    if (!(w->flags[i] & EF_DNA) || corpse_owner(w, i) == player) {
        if (loot) snprintf(prompt, (size_t)size, "Search %s", w->names[i]);
        else if (corpse_owner(w, i) == player) prompt[0] = 0;
        else snprintf(prompt, (size_t)size, "%s (DNA taken)", w->names[i]);
        return;
    }
    if (afford(w, player, self, &dur) == RACT_EXTRACT) snprintf(prompt, (size_t)size, "Extract DNA: %s (hold)", w->names[i]);
    else snprintf(prompt, (size_t)size, "Extract DNA: get closer");
}

static void fab_focus(World *w, EntityId self, int player, char *prompt, int size) {
    FabView fv;
    revive_fab_view(w, self, &fv);
    u16 dur;
    const char *st = fab_state_name((FabStateId)fv.m.state);
    if (afford(w, player, self, &dur) == RACT_INSERT) { snprintf(prompt, (size_t)size, "Insert DNA (hold 5 s)"); return; }
    if (fv.m.state == FAB_RECONSTRUCTING) snprintf(prompt, (size_t)size, "Fabricator: %s %.0f s", fv.current_name, fv.remaining);
    else if (has_dna(&w->players[player])) snprintf(prompt, (size_t)size, "Fabricator (%s): get closer", st);
    else snprintf(prompt, (size_t)size, "Fabricator (%s): needs a DNA sample", st);
}

static bool revive_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    // Kneeling assist: both hands on the chest of the downed player.
    int i = (int)ENT_INDEX(self);
    vec3 fwd = forward_from_yaw(quat_get_yaw(w->xf[i].rot));
    vec3 chest = v3_add(w->xf[i].pos, v3(0, 0.75f, 0));
    vec3 side = right_from_yaw(w->players[player].sim.move.yaw);
    *pos = v3_add(v3_madd(chest, fwd, 0.12f), v3_scale(side, hand ? 0.09f : -0.09f));
    *rot = quat_mul(quat_yaw(w->players[player].sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.3f));
    return true;
}

static bool corpse_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    // extract_blood: the free (left) hand steadies the body, the syringe hand reaches the arm / neck.
    vec3 bp[2];
    if (!chars_extract_points(w, self, bp)) {
        vec3 a, b;
        revive_corpse_axis(w, self, &a, &b);
        bp[0] = v3_lerp(a, b, 0.55f);
        bp[1] = v3_lerp(a, b, 0.85f);
    }
    *pos = hand ? bp[1] : bp[0];
    *rot = quat_mul(quat_yaw(w->players[player].sim.move.yaw), quat_axis_angle(v3(1, 0, 0), -1.4f));
    return true;
}

static bool fab_hand(World *w, EntityId self, int player, int hand, vec3 *pos, quat *rot) {
    // fabricator_insert: the vial hand pushes into the slot, the other rests on the panel.
    (void)player;
    revive_fab_socket(w, self, hand ? "sample_slot" : "panel", pos, rot);
    *rot = quat_mul(*rot, quat_axis_angle(v3(1, 0, 0), -1.4f));
    return true;
}

static const InteractDef k_revive_def = { "revive_assist", revive_focus, NULL, noop_use, NULL, revive_hand, 0.0f, HANDS_BOTH, RACT_REVIVE };
static const InteractDef k_corpse_def = { "extract_blood", corpse_focus, NULL, corpse_use, NULL, corpse_hand, 0.0f, HANDS_BOTH, RACT_EXTRACT };
static const InteractDef k_fab_def = { "fabricator_insert", fab_focus, NULL, noop_use, NULL, fab_hand, 0.0f, HANDS_BOTH, RACT_INSERT };

// ------------------------------------------------------------------ status (HUD / spectator bar)

bool revive_recovery_status(const World *w, int slot, RecoveryStatus *out) {
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= MAX_PLAYERS) return false;
    // A sample in a fabricator beats the body.
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_FABRICATOR) continue;
        EntityId fe = ENT_MAKE(i, w->gen[i]);
        FabView fv;
        revive_fab_view(w, fe, &fv);
        if (fv.current_player == slot && (fv.m.state == FAB_RECONSTRUCTING || fv.m.state == FAB_OPENING)) {
            out->stage = fv.m.state == FAB_RECONSTRUCTING ? REC_RECONSTRUCTING : REC_OPENING;
            out->fabricator = fe;
            out->remaining = fv.remaining;
            out->progress = fv.progress;
            return true;
        }
        for (int q = 0; q < fv.m.qn; q++) {
            const DnaSample *ds = dna_get_c(&RV.dna, fv.m.queue[q]);
            if (ds && ds->player_slot == slot) {
                out->stage = REC_QUEUED;
                out->fabricator = fe;
                out->queue_pos = q + 1 + (fv.m.state == FAB_RECONSTRUCTING ? 1 : 0);
                return true;
            }
        }
    }
    int id = dna_find_player(&RV.dna, slot);
    const DnaSample *ds = id ? dna_get_c(&RV.dna, id) : NULL;
    if (ds && (ds->status == DNA_CARRIED || ds->status == DNA_DROPPED)) {
        out->stage = REC_EXTRACTED;
        int who = ds->status == DNA_CARRIED ? ds->carrier : ds->extractor;
        if (who < MAX_PLAYERS && w->players[who].active) dg_strcpy(out->by, sizeof(out->by), w->players[who].name);
        if (ds->status == DNA_DROPPED) dg_strcpy(out->by, sizeof(out->by), "(dropped)");
    }
    ENT_FOREACH(w, i) {
        if (w->type[i] != ET_CORPSE || corpse_owner(w, i) != slot) continue;
        if (out->stage == REC_NONE) out->stage = REC_BODY;
        out->corpse = ENT_MAKE(i, w->gen[i]);
        out->corpse_pos = w->xf[i].pos;
    }
    return out->stage != REC_NONE;
}

const char *revive_item_name(const ItemDB *db, u16 item, i16 ammo) {
    static char ring[8][48];
    static int at;
    const ItemDef *d = item_def(db, item);
    if (!RV.dna_item || item != RV.dna_item) return d->name;
    const DnaSample *ds = dna_get_c(&RV.dna, ammo);
    char *b = ring[at++ & 7];
    if (ds) snprintf(b, sizeof(ring[0]), "DNA: %s #%u", ds->name, ds->uid);
    else snprintf(b, sizeof(ring[0]), "%s", d->name);
    return b;
}

// ------------------------------------------------------------------ client events

void revive_handle_event(const GameEvent *ev) {
    World *w = game_world();
    if (!w) return;
    check_world(w);
    if (ev->type == EV_DNA) {
        if (ev->id >= 1 && ev->id <= DNA_MAX && !w->authority) memcpy(&RV.dna.s[ev->id - 1], ev->data, sizeof(DnaSample));
        return;
    }
    char t[160];
    int self = game() ? game()->local_slot : -1;
    int subj = world_player_by_ent(w, ev->ent);
    switch ((ReviveEvent)ev->id) {
    case RVE_DOWNED:
        if (subj == self) game_add_hint("You are down: crawl to cover or wait for help");
        else { snprintf(t, sizeof(t), "%s is down!", ev->text); game_add_hint(t); }
        break;
    case RVE_REVIVED:
        if (subj == self) { snprintf(t, sizeof(t), "Revived by %s", ev->text); game_add_hint(t); }
        else if (world_player_by_ent(w, ev->ent2) == self) game_add_hint("Revived");
        break;
    case RVE_BLED_OUT:
        snprintf(t, sizeof(t), "%s bled out", ev->text);
        game_add_hint(t);
        break;
    case RVE_EXTRACTED: {
        int corpse_owner_slot = ent_valid(w, ev->ent) ? (int)w->item[ENT_INDEX(ev->ent)] : -1;
        if (world_player_by_ent(w, ev->ent2) == self) game_add_hint("DNA sample extracted");
        else if (corpse_owner_slot == self) { snprintf(t, sizeof(t), "DNA extracted by %s", ev->text); game_add_hint(t); }
        break;
    }
    case RVE_INSERTED:
        snprintf(t, sizeof(t), "Fabricator: %s queued", ev->text);
        game_add_hint(t);
        break;
    case RVE_RECON_START:
        snprintf(t, sizeof(t), "Reconstructing %s", ev->text);
        game_add_hint(t);
        break;
    case RVE_RECON_DONE:
        if (world_player_by_ent(w, ev->ent2) == self) game_add_hint("Reconstructed");
        else { snprintf(t, sizeof(t), "%s is back", ev->text); game_add_hint(t); }
        break;
    case RVE_EJECTED:
        snprintf(t, sizeof(t), "Fabricator lost power: %s's sample ejected", ev->text);
        game_add_hint(t);
        break;
    case RVE_CANCELLED:
        snprintf(t, sizeof(t), "Interrupted (%s)", ev->text);
        game_add_hint(t);
        break;
    }
}

// ------------------------------------------------------------------ commands (host / dev)

static int arg_slot(int argc, const char **argv, int k) {
    if (argc > k) {
        int s = atoi(argv[k]);
        return s >= 0 && s < MAX_PLAYERS ? s : -1;
    }
    return game() ? game()->local_slot : -1;
}

static World *host_world(const char *cmd) {
    World *w = game_world();
    if (!w || !w->authority) { log_info("%s: host only", cmd); return NULL; }
    return w;
}

static void cmd_down(int argc, const char **argv) {
    World *w = host_world("down");
    int s = arg_slot(argc, argv, 1);
    if (!w || s < 0 || !w->players[s].active) return;
    if (!revive_sv_down(w, s, ENT_NONE)) log_info("down: player %d can't be knocked (dead or already down)", s);
}

static void cmd_revive(int argc, const char **argv) {
    World *w = host_world("revive");
    int s = arg_slot(argc, argv, 1);
    if (!w || s < 0 || !w->players[s].active) return;
    Player *p = &w->players[s];
    if (p->sim.down.downed) revive_sv_revive(w, s, -1);
    else if (p->sim.move.mode == MOVE_DEAD) game_respawn_player(w, s);
    else log_info("revive: player %d is fine", s);
}

static void cmd_spawn_corpse(int argc, const char **argv) {
    World *w = host_world("spawn_corpse");
    Player *lp = game_local_player();
    if (!w || !lp) return;
    int s = argc > 1 ? atoi(argv[1]) : lp - w->players;
    if (s < 0 || s >= MAX_PLAYERS) return;
    vec3 at = v3_madd(lp->sim.move.pos, forward_from_yaw(lp->sim.move.yaw), argc > 2 ? (float)atof(argv[2]) : 1.4f);
    EntityId e = revive_spawn_corpse(w, s, at, lp->sim.move.yaw + 1.57f);
    log_info("spawn_corpse: entity %u for player %d", e, s);
}

static void cmd_spawn_fabricator(int argc, const char **argv) {
    World *w = host_world("spawn_fabricator");
    Player *lp = game_local_player();
    if (!w || !lp) return;
    float dist = argc > 1 ? (float)atof(argv[1]) : 3.0f;
    vec3 at = v3_madd(lp->sim.move.pos, forward_from_yaw(lp->sim.move.yaw), dist);
    EntityId e = revive_spawn_fabricator(w, at, lp->sim.move.yaw + DG_PI, argc > 2 ? atoi(argv[2]) != 0 : true);
    log_info("spawn_fabricator: entity %u at %.1f %.1f %.1f", e, at.x, at.y, at.z);
}

// A teammate without a client (host-simulated, stands still): down / kill / revive / reconstruct it alone.
static void cmd_spawn_teammate(int argc, const char **argv) {
    World *w = host_world("spawn_teammate");
    Player *lp = game_local_player();
    if (!w || !lp) return;
    int s = -1;
    for (int k = 0; k < MAX_PLAYERS && s < 0; k++) if (!w->players[k].active) s = k;
    if (s < 0) { log_warn("spawn_teammate: server full"); return; }
    game_spawn_player(w, s, argc > 1 ? argv[1] : "Alex", false, -1);
    Player *p = &w->players[s];
    float dist = argc > 2 ? (float)atof(argv[2]) : 1.5f;
    vec3 at = v3_madd(lp->sim.move.pos, forward_from_yaw(lp->sim.move.yaw), dist);
    p->sim.move.pos = at;
    p->sim.move.yaw = p->sim.move.body_yaw = lp->sim.move.yaw + DG_PI;
    if (p->character) phys_character_set_pos(p->character, at);
    ent_set_pos(w, p->ent, at, quat_yaw(p->sim.move.yaw), true);
    log_info("spawn_teammate: slot %d '%s' at %.1f %.1f %.1f", s, p->name, at.x, at.y, at.z);
}

// Test helper: stand where a timed action is possible and look at its target (scripts).
// revive_approach corpse|fab|downed [side offset m]
static void cmd_revive_approach(int argc, const char **argv) {
    World *w = game_world();
    Player *lp = game_local_player();
    Game *g = game();
    if (!w || !lp || !g || argc < 2) { log_info("usage: revive_approach corpse|fab|downed [offset]"); return; }
    float off = argc > 2 ? (float)atof(argv[2]) : -1.0f;
    vec3 stand = lp->sim.move.pos, look = stand;
    bool found = false;
    ENT_FOREACH(w, i) {
        EntityId e = ENT_MAKE(i, w->gen[i]);
        if (!strcmp(argv[1], "corpse") && w->type[i] == ET_CORPSE && (w->flags[i] & EF_DNA)) {
            vec3 a, b;
            revive_corpse_axis(w, e, &a, &b);
            vec3 mid = v3_lerp(a, b, 0.5f);
            vec3 side = right_from_yaw(quat_get_yaw(w->xf[i].rot));
            stand = v3_madd(mid, side, off >= 0 ? off : 0.45f);
            stand.y = a.y - 0.15f;
            look = ent_focus_point(w, e);
            found = true;
        } else if (!strcmp(argv[1], "fab") && w->type[i] == ET_FABRICATOR) {
            vec3 s;
            quat r;
            revive_fab_socket(w, e, "sample_slot", &s, &r);
            stand = v3_add(s, quat_rotate(r, v3(-0.1f, 0, -(off >= 0 ? off : 0.9f))));
            stand.y = w->xf[i].pos.y + 0.05f;
            look = s;
            found = true;
        } else if (!strcmp(argv[1], "downed") && w->type[i] == ET_PLAYER && (w->flags[i] & EF_DOWNED) && e != lp->ent) {
            vec3 fwd = forward_from_yaw(quat_get_yaw(w->xf[i].rot));
            stand = v3_madd(w->xf[i].pos, fwd, off >= 0 ? off : 1.0f);
            look = v3_add(w->xf[i].pos, v3(0, 0.6f, 0));
            found = true;
        }
        if (found) break;
    }
    if (!found) { log_info("revive_approach: no %s", argv[1]); return; }
    lp->sim.move.pos = stand;
    lp->sim.move.vel = v3(0, 0, 0);
    if (lp->character) { phys_character_set_pos(lp->character, stand); phys_character_set_vel(lp->character, v3(0, 0, 0)); }
    vec3 eye = v3_add(stand, v3(0, strcmp(argv[1], "corpse") ? PLAYER_EYE_STAND : PLAYER_EYE_CROUCH, 0));
    vec3 d = v3_sub(look, eye);
    g->view.yaw = atan2f(-d.x, -d.z);
    g->view.pitch = atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z));
    g->view.smooth_init = false;
    log_info("revive_approach: %s at %.2f %.2f %.2f", argv[1], stand.x, stand.y, stand.z);
}

// Script sync (two-instance tests): hold the script until a condition is true (or the timeout).
// revive_wait players <n> | fab <progress 0..1> | <slot> alive|dead|downed|reconstructing|acted
// [timeout frames, default 3000]. Frames per tick differ by far between a GPU and lavapipe: the
// scripts wait on the state, never on a frame count for a timed action.
static bool wait_done(int argc, const char **argv) {
    World *w = game_world();
    if (argc < 3) return true;
    if (!w || (strcmp(argv[1], "players") && !game_local_player())) return false;   // not connected yet
    if (!strcmp(argv[1], "players")) {
        int n = 0;
        for (int s = 0; s < MAX_PLAYERS; s++) n += w->players[s].active ? 1 : 0;
        return n >= atoi(argv[2]);
    }
    if (!strcmp(argv[1], "fab")) {
        // Any fabricator's reconstruction at this progress (or already past it: opening).
        float at = (float)atof(argv[2]);
        ENT_FOREACH(w, i) {
            FabView fv;
            if (w->type[i] != ET_FABRICATOR || !revive_fab_view(w, ENT_MAKE(i, w->gen[i]), &fv)) continue;
            if ((fv.m.state == FAB_RECONSTRUCTING && fv.progress >= at) || fv.m.state == FAB_OPENING) return true;
        }
        return false;
    }
    int s = atoi(argv[1]);
    if (s < 0 || s >= MAX_PLAYERS || !w->players[s].active) return false;
    const Player *p = &w->players[s];
    bool dead = p->sim.move.mode == MOVE_DEAD || (ent_valid(w, p->ent) && (w->flags[ENT_INDEX(p->ent)] & EF_DEAD));
    bool downed = (ent_valid(w, p->ent) && (w->flags[ENT_INDEX(p->ent)] & EF_DOWNED)) || p->sim.down.downed;
    if (!strcmp(argv[2], "alive")) return !dead && !downed;
    if (!strcmp(argv[2], "dead")) return dead;
    if (!strcmp(argv[2], "downed")) return downed;
    // The timed action (use still held) finished or was cancelled.
    if (!strcmp(argv[2], "acted")) return p->sim.act.phase == ACTP_DONE || p->sim.act.phase == ACTP_CANCELLED;
    if (!strcmp(argv[2], "reconstructing")) {
        RecoveryStatus rs;
        return revive_recovery_status(w, s, &rs) && rs.stage == REC_RECONSTRUCTING;
    }
    return true;
}

static void cmd_revive_wait(int argc, const char **argv) {
    static int frames;
    int timeout = argc > 3 ? atoi(argv[3]) : 3000;
    if (wait_done(argc, argv) || ++frames > timeout) {
        if (frames > timeout) log_warn("revive_wait: timeout (%s %s)", argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "");
        else log_info("revive_wait: %s %s after %d frames", argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "", frames);
        frames = 0;
        return;
    }
    char line[128];
    snprintf(line, sizeof(line), "revive_wait %s %s %d", argc > 1 ? argv[1] : "", argc > 2 ? argv[2] : "", timeout);
    cbuf_retry(line);
}

static void cmd_fab_power(int argc, const char **argv) {
    World *w = host_world("fab_power");
    if (!w || argc < 2) { log_info("usage: fab_power <0|1> [fabricator entity]"); return; }
    bool on = atoi(argv[1]) != 0;
    for (int k = 0; k < MAX_FABS; k++) {
        FabRec *f = &RV.fabs[k];
        if (!f->ent || !ent_valid(w, f->ent)) continue;
        if (argc > 2 && (u32)atoi(argv[2]) != f->ent) continue;
        fab_power(w, f, on);
        log_info("fab_power: %u %s", f->ent, on ? "on" : "off");
    }
}

static void cmd_revive_dump(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    World *w = game_world();
    if (!w) return;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        const Player *p = &w->players[s];
        if (!p->active) continue;
        const DownState *d = &p->sim.down;
        const ActionState *a = &p->sim.act;
        log_info("revive_dump: tick %u player %d '%s' mode %d downed %d downs %u bleed %u/%u reviver %d rv %u/%u act %s phase %d ticks %u/%u cancel %s hp %.0f frozen %u",
                 w->tick, s, p->name, p->sim.move.mode, d->downed, d->downs, d->bleed_ticks, d->bleed_total, d->reviver == REVIVE_NOBODY ? -1 : d->reviver,
                 d->revive_ticks, d->revive_total, act_kind_name((ActKind)a->kind), a->phase, a->ticks, a->dur, act_cancel_name((ActCancel)a->cancel),
                 w->health[ENT_INDEX(p->ent)].hp, p->sim.frozen_ticks);
    }
    ENT_FOREACH(w, i) {
        if (w->type[i] == ET_CORPSE)
            log_info("revive_dump: corpse %u owner %d '%s' samples %u dna %d pos %.2f %.2f %.2f", ENT_MAKE(i, w->gen[i]), corpse_owner(w, i), w->names[i],
                     w->aux_count[i], (w->flags[i] & EF_DNA) != 0, w->xf[i].pos.x, w->xf[i].pos.y, w->xf[i].pos.z);
        if (w->type[i] == ET_FABRICATOR) {
            FabView fv;
            revive_fab_view(w, ENT_MAKE(i, w->gen[i]), &fv);
            log_info("revive_dump: fabricator %u state %s queue %d current %d player %d '%s' progress %.3f remaining %.2f powered %d", ENT_MAKE(i, w->gen[i]),
                     fab_state_name((FabStateId)fv.m.state), fv.m.qn, fv.m.current, fv.current_player, fv.current_name, fv.progress, fv.remaining, fv.m.powered);
        }
    }
    for (int k = 0; k < DNA_MAX; k++) {
        const DnaSample *ds = &RV.dna.s[k];
        if (ds->status != DNA_FREE)
            log_info("revive_dump: sample %d uid %u '%s' player %d status %d carrier %d", k + 1, ds->uid, ds->name, ds->player_slot, ds->status,
                     ds->carrier == REVIVE_NOBODY ? -1 : ds->carrier);
    }
}

static void cmd_reload_rules(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    load_rules();
    log_info("revive: rules reloaded (respawn %s, bleed %.0f s, revive %.1f s)", respawn_mode_name(RV.rules.respawn_mode), RV.rules.bleed_time, RV.rules.revive_time);
}

static void cmd_recon_preview(int argc, const char **argv) {
    RV.recon_preview = argc > 1 ? (float)atof(argv[1]) : -1.0f;
    if (RV.recon_preview > 1.0f) RV.recon_preview = 1.0f;
    log_info("reconstruction_preview: %s", RV.recon_preview < 0 ? "off" : argv[1]);
}

void revive_register(void) {
    load_rules();
    entities_set_interact_def(ET_PLAYER, &k_revive_def);
    entities_set_interact_def(ET_CORPSE, &k_corpse_def);
    entities_set_interact_def(ET_FABRICATOR, &k_fab_def);
    RV.debug = cvar_reg_bool("revive_debug", false, CVAR_LOCKED, "log revive / extraction / fabricator transitions");
    cmd_register("down", cmd_down, "down [slot]: knock a player down (host, revive test)");
    cmd_register("revive", cmd_revive, "revive [slot]: revive from downed, or respawn from dead (host)");
    cmd_register("spawn_corpse", cmd_spawn_corpse, "spawn_corpse [player slot] [distance]: a DNA corpse in front of you (host)");
    cmd_register("spawn_fabricator", cmd_spawn_fabricator, "spawn_fabricator [distance] [powered 0|1]: a reconstruction fabricator in front of you (host)");
    cmd_register("spawn_teammate", cmd_spawn_teammate, "spawn_teammate [name] [distance]: a client-less teammate in front of you (host, revive loop tests)");
    cmd_register("revive_approach", cmd_revive_approach, "revive_approach corpse|fab|downed [offset]: stand in reach of a timed action and look at it (tests)");
    cmd_register("revive_wait", cmd_revive_wait, "revive_wait players <n> | fab <progress> | <slot> alive|dead|downed|reconstructing|acted [timeout]: hold the script until true (tests)");
    cmd_register("fab_power", cmd_fab_power, "fab_power <0|1> [entity]: power fabricators on / off (ejects samples mid-cycle)");
    cmd_register("revive_dump", cmd_revive_dump, "log downed / action / corpse / fabricator / DNA state (tests)");
    cmd_register("revive_rules_reload", cmd_reload_rules, "reload assets/gameplay/revive_rules.json");
    cmd_register("reconstruction_preview", cmd_recon_preview, "reconstruction_preview <0..1>: scrub the reconstruction effect on the dev room fabricator (no arg = off)");
}
