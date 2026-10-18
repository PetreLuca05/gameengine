// Spectating, host side (docs/SPECTATE.md): the observer table, target validation and the death
// flow (auto spectate, respawn rules, join dead). The net layer asks it whose local data and
// relevance a client gets, which events it receives, and hands it every command that arrives so
// they are relayed to the sender's observers at once.
#include "spectate.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/engine.h"
#include "world/world.h"
#include "gameplay/events.h"
#include "gameplay/health.h"

typedef struct SpecHost {
    SpecObserver obs[MAX_PLAYERS];
    SpecCmdRing ring[MAX_PLAYERS];        // each player's latest commands (relay + switch history)
    u32 relay_bytes[MAX_PLAYERS];
    bool respawn_requested[MAX_PLAYERS];
    SpecSendFn send;
    Cvar *allowed, *fp_enemies, *respawn_time, *respawn_mode, *join_dead;
} SpecHost;

static SpecHost S;

void spectate_register(void) {
    S.allowed = cvar_reg_bool("sv_spec_allowed", true, CVAR_ARCHIVE | CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "dead players may spectate living players (docs/SPECTATE.md)");
    S.fp_enemies = cvar_reg_bool("sv_spec_fp_enemies", true, CVAR_ARCHIVE | CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "first-person spectating of players on another team (PvP: 0)");
    S.respawn_time = cvar_reg_float("sv_respawn_time", PLAYER_RESPAWN_TIME, 0.0f, 600.0f, CVAR_ARCHIVE | CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "seconds a dead player waits before respawning");
    S.respawn_mode = cvar_reg_int("sv_respawn_mode", 0, 0, 1, CVAR_ARCHIVE | CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "0 = respawn automatically after sv_respawn_time, 1 = on request (Respawn button) once it has passed");
    S.join_dead = cvar_reg_bool("sv_join_dead", false, CVAR_ARCHIVE | CVAR_SERVER | CVAR_LOCKED | CVAR_RULE, "late joiners join dead and start spectating (while anyone is alive)");
}

void spectate_reset(void) {
    SpecSendFn send = S.send;
    Cvar *a = S.allowed, *f = S.fp_enemies, *rt = S.respawn_time, *rm = S.respawn_mode, *jd = S.join_dead;
    memset(&S, 0, sizeof(S));
    S.send = send;
    S.allowed = a; S.fp_enemies = f; S.respawn_time = rt; S.respawn_mode = rm; S.join_dead = jd;
    for (int s = 0; s < MAX_PLAYERS; s++) { S.obs[s].target = SPEC_NONE; S.obs[s].killer = SPEC_NONE; }
}

void spectate_set_sender(SpecSendFn fn) { S.send = fn; }
const SpecObserver *spectate_observer(int slot) { return slot >= 0 && slot < MAX_PLAYERS ? &S.obs[slot] : NULL; }
const SpecCmdRing *spectate_host_ring(int target) { return target >= 0 && target < MAX_PLAYERS ? &S.ring[target] : NULL; }
void spectate_count_relay_bytes(int obs, u32 bytes) { if (obs >= 0 && obs < MAX_PLAYERS) S.relay_bytes[obs] += bytes; }
u32 spectate_take_relay_bytes(int obs) {
    if (obs < 0 || obs >= MAX_PLAYERS) return 0;
    u32 b = S.relay_bytes[obs];
    S.relay_bytes[obs] = 0;
    return b;
}

static bool watching(const SpecObserver *o) { return (o->mode == SPEC_IN_EYE || o->mode == SPEC_THIRD) && o->target < MAX_PLAYERS; }

static bool player_alive(World *w, int s) {
    const Player *p = &w->players[s];
    return p->active && ent_valid(w, p->ent) && !(w->flags[ENT_INDEX(p->ent)] & EF_DEAD) && p->sim.move.mode != MOVE_DEAD;
}

static void gather(World *w, SpecPlayerInfo *pl, SpecRules *r) {
    for (int s = 0; s < MAX_PLAYERS; s++) {
        pl[s].active = w->players[s].active;
        pl[s].alive = pl[s].active && player_alive(w, s);
        pl[s].dev = S.obs[s].dev != 0;
        pl[s].team = 0;   // no teams yet: everyone is friendly
        pl[s].mode = S.obs[s].mode;
    }
    r->allowed = !S.allowed || S.allowed->i;
    r->fp_enemies = !S.fp_enemies || S.fp_enemies->i;
    r->round_allows = true;   // no rounds yet ("no spectating pre-round" hooks in here)
}

static int nearest_alive(World *w, int obs, const SpecPlayerInfo *pl, const SpecRules *r, int mode) {
    vec3 from = w->players[obs].sim.move.pos;
    int best = -1;
    float bd = 1e30f;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (spec_validate(pl, MAX_PLAYERS, obs, mode, s, r) != SPEC_OK) continue;
        float d = v3_dist(from, w->players[s].sim.move.pos);
        if (d < bd) { bd = d; best = s; }
    }
    return best;
}

static void set_target(SpecObserver *o, int mode, int target) {
    u8 t = target >= 0 ? (u8)target : SPEC_NONE;
    if (o->mode != mode || o->target != t) {
        if (o->target != t) { o->seq++; o->forced = 2; }   // switch: a full state + history burst
        o->mode = (u8)mode;
        o->target = t;
    }
}

SpecResult spectate_request(World *w, int obs, int mode, int target_req, bool dev) {
    if (!w || obs < 0 || obs >= MAX_PLAYERS || !w->players[obs].active) return SPEC_ERR_TARGET;
    SpecObserver *o = &S.obs[obs];
    if (mode == SPEC_OFF) {
        set_target(o, SPEC_OFF, -1);
        o->dev = 0;
        return SPEC_OK;
    }
    if (dev) o->dev = 1;
    SpecPlayerInfo pl[MAX_PLAYERS];
    SpecRules r;
    gather(w, pl, &r);
    pl[obs].mode = SPEC_OFF;   // re-targeting itself is fine
    int cur = o->target < MAX_PLAYERS ? o->target : -1;
    int want_mode = mode == SPEC_FREE ? SPEC_FREE : mode;
    int target = target_req;
    if (target_req == SPEC_REQ_NEXT || target_req == SPEC_REQ_PREV) target = spec_cycle(pl, MAX_PLAYERS, obs, want_mode, cur, target_req == SPEC_REQ_NEXT ? 1 : -1, &r);
    else if (target_req == SPEC_REQ_KEEP) target = cur >= 0 ? cur : spec_cycle(pl, MAX_PLAYERS, obs, want_mode, -1, 1, &r);
    else if (target_req == SPEC_REQ_AUTO) {
        // The killer when it is a living player, else the nearest living player.
        target = o->killer < MAX_PLAYERS && spec_validate(pl, MAX_PLAYERS, obs, want_mode, o->killer, &r) == SPEC_OK ? o->killer : nearest_alive(w, obs, pl, &r, want_mode);
    }
    if (want_mode != SPEC_FREE && target < 0) {
        // Nobody to watch: the free camera (when even that is not allowed the request fails below).
        if (target_req < 0) want_mode = SPEC_FREE;
    }
    SpecResult res = spec_validate(pl, MAX_PLAYERS, obs, want_mode, target, &r);
    if (res == SPEC_ERR_ENEMY) {
        // First person on an enemy is disabled: watch them from behind instead.
        want_mode = SPEC_THIRD;
        res = spec_validate(pl, MAX_PLAYERS, obs, want_mode, target, &r);
    }
    if (res != SPEC_OK) {
        if (!dev || res != SPEC_ERR_ALIVE) ev_hint(w, obs, "Spectate: %s", spec_result_name(res));
        if (dev && res == SPEC_ERR_ALIVE) o->dev = 0;
        return res;
    }
    set_target(o, want_mode, want_mode == SPEC_FREE ? -1 : target);
    if (want_mode == SPEC_FREE && !o->free_pos_valid) {
        o->free_pos = v3_add(w->players[obs].sim.move.pos, v3(0, 2.5f, 0));
        o->free_pos_valid = true;
    }
    log_info("spectate: slot %d -> %s %d", obs, spec_mode_name(want_mode), want_mode == SPEC_FREE ? -1 : target);
    return SPEC_OK;
}

void spectate_free_pos(int obs, vec3 pos) {
    if (obs < 0 || obs >= MAX_PLAYERS) return;
    S.obs[obs].free_pos = pos;
    S.obs[obs].free_pos_valid = true;
}

void spectate_on_death(World *w, int victim, int killer) {
    if (victim < 0 || victim >= MAX_PLAYERS) return;
    SpecObserver *o = &S.obs[victim];
    o->killer = killer >= 0 && killer < MAX_PLAYERS && killer != victim ? (u8)killer : SPEC_NONE;
    S.respawn_requested[victim] = false;
    if (S.respawn_time) w->players[victim].respawn_timer = S.respawn_time->f;
    // Observers of the victim move on (spectate_host_tick re-targets them).
}

void spectate_on_respawn(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    SpecObserver *o = &S.obs[slot];
    set_target(o, SPEC_OFF, -1);
    o->dev = 0;
    o->killer = SPEC_NONE;
    o->free_pos_valid = false;
    S.respawn_requested[slot] = false;
}

void spectate_player_left(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    memset(&S.obs[slot], 0, sizeof(SpecObserver));
    S.obs[slot].target = S.obs[slot].killer = SPEC_NONE;
    spec_ring_reset(&S.ring[slot]);
    S.respawn_requested[slot] = false;
}

void spectate_request_respawn(int slot) {
    if (slot >= 0 && slot < MAX_PLAYERS) S.respawn_requested[slot] = true;
}

bool spectate_respawn_ready(World *w, int slot) {
    const Player *p = &w->players[slot];
    if (p->respawn_timer > 0) return false;
    if (S.respawn_mode && S.respawn_mode->i == 1) return S.respawn_requested[slot];   // on request
    return true;
}

bool spectate_join_dead(World *w) {
    if (!S.join_dead || !S.join_dead->i) return false;
    for (int s = 0; s < MAX_PLAYERS; s++) if (player_alive(w, s)) return true;   // only when someone is alive to watch
    return false;
}

void spectate_host_tick(World *w) {
    SpecPlayerInfo pl[MAX_PLAYERS];
    SpecRules r;
    gather(w, pl, &r);
    for (int s = 0; s < MAX_PLAYERS; s++) {
        SpecObserver *o = &S.obs[s];
        if (!w->players[s].active) continue;
        if (o->forced > 0 && !watching(o)) o->forced = 0;
        if (o->mode == SPEC_OFF) continue;
        // Alive again (respawned) and not a dev spectator: back to playing.
        if (pl[s].alive && !o->dev) { spectate_on_respawn(s); continue; }
        if (!r.allowed) { set_target(o, SPEC_OFF, -1); continue; }
        if (!watching(o)) continue;
        pl[s].mode = SPEC_OFF;
        SpecResult res = spec_validate(pl, MAX_PLAYERS, s, o->mode, o->target, &r);
        pl[s].mode = o->mode;
        if (res == SPEC_OK) continue;
        // The target died / left / started spectating: the next legal player, else the free camera.
        int from = o->target;
        pl[s].mode = SPEC_OFF;
        int next = spec_cycle(pl, MAX_PLAYERS, s, o->mode, from, 1, &r);
        pl[s].mode = o->mode;
        if (next >= 0) set_target(o, o->mode, next);
        else {
            if (!o->free_pos_valid && from < MAX_PLAYERS) { o->free_pos = v3_add(w->players[from].sim.move.pos, v3(0, 2.5f, 0)); o->free_pos_valid = true; }
            set_target(o, SPEC_FREE, -1);
        }
    }
}

void spectate_host_cmds(World *w, int slot, const UserCmd *cmds, int count, u32 send_ms) {
    if (slot < 0 || slot >= MAX_PLAYERS) return;
    SpecCmdRing *ring = &S.ring[slot];
    bool fresh = false;
    for (int k = 0; k < count; k++) fresh |= spec_ring_put(ring, &cmds[k], send_ms);
    if (!fresh || !S.send) return;
    // Relay now (not on the next tick / snapshot): the newest commands with the usual redundancy.
    u32 to = ring->newest;
    u32 from = to >= SPEC_CMDS_PER_PACKET ? to - SPEC_CMDS_PER_PACKET + 1 : 1;
    for (int o = 0; o < MAX_PLAYERS; o++) {
        const SpecObserver *ob = &S.obs[o];
        if (o == slot || !w->players[o].active || w->players[o].local || !watching(ob) || ob->target != slot) continue;
        S.send(o, slot, from, to);
    }
}

int spectate_source(World *w, int s) {
    if (s < 0 || s >= MAX_PLAYERS) return s;
    const SpecObserver *o = &S.obs[s];
    if (watching(o) && w->players[o->target].active) return o->target;
    return s;
}

vec3 spectate_relevance_center(World *w, int s) {
    const SpecObserver *o = &S.obs[s];
    if (o->mode == SPEC_FREE && o->free_pos_valid) return o->free_pos;
    // In eye / third person: the world streams as if the observer stood where the target is.
    return w->players[spectate_source(w, s)].sim.move.pos;
}

void spectate_fill_block(World *w, int s, SpecBlock *b) {
    memset(b, 0, sizeof(*b));
    SpecObserver *o = &S.obs[s];
    const Player *p = &w->players[s];
    b->data_slot = (u8)spectate_source(w, s);
    b->mode = o->mode;
    b->target = watching(o) ? o->target : SPEC_NONE;
    b->killer = o->killer;
    b->seq = o->seq;
    if (o->forced > 0 && watching(o)) { b->flags |= SPECF_FORCED; o->forced--; }
    if (o->dev) b->flags |= SPECF_DEV;
    bool dead = !player_alive(w, s);
    bool manual = S.respawn_mode && S.respawn_mode->i == 1;
    if (dead && p->respawn_timer <= 0 && manual) b->flags |= SPECF_CAN_RESPAWN;
    b->respawn_ds = dead ? (u8)DG_CLAMP((int)ceilf(p->respawn_timer * 10.0f), 0, 254) : 0;
    if (dead && manual && p->respawn_timer <= 0) b->respawn_ds = 255;
}

int spectate_event_target(int s) {
    if (s < 0 || s >= MAX_PLAYERS) return SPEC_NONE;
    return watching(&S.obs[s]) ? S.obs[s].target : SPEC_NONE;
}

u8 spectate_observers_mask(int target) {
    u8 m = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) if (watching(&S.obs[s]) && S.obs[s].target == target) m |= (u8)(1u << s);
    return m;
}
