// Client-side event handling, HUD timers, and inventory/container RPCs.
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "gameplay/effects.h"
#include "gameplay/flesh_fx.h"
#include "gameplay/entities.h"
#include "gameplay/health.h"
#include "gameplay/characters.h"
#include "net/net.h"
#include "net/spectate.h"
#include "core/cvar.h"
#include "audio/audio.h"
#include "anim/body_parts.h"
#include "ui/crosshair.h"
#include "gameplay/throwables.h"
#include "gameplay/revive/revive.h"
#include "gameplay/ballistics/bal_world.h"
#include "gameplay/ballistics/bal_fx.h"
#include "game_story.h"
#include <stdio.h>
#include <math.h>

#define G (g_engine.game)

void game_add_hint(const char *text) {
    if (!G) return;
    memmove(&G->hints[1], &G->hints[0], sizeof(Hint) * (MAX_HINTS - 1));
    dg_strcpy(G->hints[0].text, sizeof(G->hints[0].text), text);
    G->hints[0].time = 4.0f;
}

// New map / session: kills, the feed, pings, wheels.
void game_hud_reset(void) {
    kill_stats_reset(&G->kill_stats);
    memset(&G->kill_feed, 0, sizeof(G->kill_feed));
    game_pings_reset();
    wheels_reset();
    G->hud_time = 0;
}

// EV_KILL: the kill feed on every machine, the counter for the local player's own kills.
static void add_kill(const GameEvent *ev) {
    KillEventData kd;
    memcpy(&kd, ev->data, sizeof(kd));
    kd.killer[sizeof(kd.killer) - 1] = kd.victim[sizeof(kd.victim) - 1] = 0;
    Player *lp = game_local_player();
    bool mine = lp && ev->ent2 == lp->ent && !(ev->id & KILLF_SELF);
    bool me_victim = lp && ev->ent == lp->ent;
    killfeed_push(&G->kill_feed, kd.killer, kd.victim, kd.weapon, ev->id, mine, me_victim);
    if (mine) kill_stats_add(&G->kill_stats, ev->id, G->hud_time);
}

void game_hud_tick(float dt) {
    for (int i = 0; i < MAX_HINTS; i++) if (G->hints[i].time > 0) G->hints[i].time -= dt;
    G->hud_time += dt;
    killfeed_tick(&G->kill_feed, dt);
    kill_stats_tick(&G->kill_stats, G->hud_time, dt);
    game_pings_tick(dt);
    for (int i = 0; i < MAX_HURT_MARKERS; i++) if (G->hurt[i].time > 0) G->hurt[i].time -= dt;
    if (G->hitmarker_time > 0) G->hitmarker_time -= dt;
    G->damage_flash = fmaxf(0.0f, G->damage_flash - dt * 1.5f);
    fx_update(dt);
    flesh_fx_update(G->world, dt);
    if (G->world) bal_world_frame(G->world, G->view.eye, dt);
    // Hold-interaction progress for the HUD: authoritative value (host) or replicated (client), of
    // the viewed player (a spectated player's comes with its local data).
    Player *p = game_view_player();
    G->hold_progress = p ? p->hold_progress : 0;
}

void game_handle_event(const GameEvent *ev) {
    World *w = G->world;
    switch (ev->type) {
    case EV_HINT: game_add_hint(ev->text); break;
    case EV_STORY: story_handle_event(ev); break;
    case EV_SOUND: audio_play(ev->text, ev->pos, ev->value, SND_SFX); break;
    case EV_DAMAGE_NUMBER: break;   // floating damage numbers removed (hit markers / damage effects remain)
    case EV_HIT_CONFIRM:
        G->hitmarker_kill = (ev->id & 1u) != 0;
        G->hitmarker_head = (ev->id & 2u) != 0;
        G->hitmarker_time = crosshair_hitmarker_duration(G->hitmarker_kill);
        break;   // visual only: no hit-confirm sound
    case EV_HURT: {
        for (int i = 0; i < MAX_HURT_MARKERS; i++) {
            if (G->hurt[i].time > 0) continue;
            G->hurt[i].from = v3_sub(ev->pos, v3_scale(ev->dir, 5.0f));
            G->hurt[i].time = 1.5f;
            break;
        }
        G->damage_flash = fminf(1.0f, G->damage_flash + ev->value / 40.0f);
        audio_play_2d("hurt", 0.7f, SND_SFX);
        wheels_on_hurt();   // ends a door peek, stops auto-use of interruptible items
        lean_on_hurt();     // and a lean out of a window
        break;
    }
    case EV_IMPACT: {
        bool predicted = ev->ent2 && (int)ev->ent2 - 1 == game_view_slot() && fx_predict_impacts();   // the viewed player's shots were predicted here
        if (ev->ent && (((ev->id >> 12) & 0xF) & FX_IMPACT_FLESH)) {
            // Character hit: flesh effects (the shooter predicted only the entry burst).
            FleshHit fh;
            memset(&fh, 0, sizeof(fh));
            bool kill, melee;
            flesh_unpack_event(ev->data, &fh.dir, &fh.part, &kill, &melee, &fh.dist);
            fh.target = ev->ent;
            fh.pos = ev->pos;
            fh.normal = ev->dir;
            fh.caliber = (int)((ev->id >> 8) & 0xF);
            fh.damage = ev->value;
            u32 fl = (ev->id >> 12) & 0xF;
            fh.flags = ((fl & FX_IMPACT_HEAD) ? FLESH_HEAD : 0u) | (kill ? FLESH_KILL : 0u) | (melee ? FLESH_MELEE : 0u) | (predicted ? FLESH_NO_ENTRY : 0u);
            if (bal_impact_outcome(ev->id) & BAL_IMP_BALLISTIC) {
                fh.flags |= FLESH_BALLISTIC | ((bal_impact_outcome(ev->id) & BAL_IMP_PENETRATE) ? FLESH_EXITS : 0u);
                fh.cavity = bal_impact_cavity(ev->id);
            }
            flesh_fx_hit(w, &fh);
            break;
        }
        // Own shots were predicted locally (cl_predict_impacts): skip the host's copy.
        if (predicted) break;
        vec3 inc;
        memcpy(&inc, ev->data, sizeof(inc));
        fx_impact_ex(ev->pos, ev->dir, inc, (int)(ev->id & 0xFF), (int)((ev->id >> 8) & 0xF), ((ev->id >> 12) & 0xF) | (ev->id & 0xFFFF0000u));
        break;
    }
    case EV_TRACER: {
        vec3 from = ev->pos;
        if (ev->id & 1u) {
            // Remote shot: flash (and tracer start) at the shooter's visible barrel when we have it.
            vec3 mp, md = v3_norm(v3_sub(ev->dir, ev->pos));
            if (ent_valid(w, ev->ent) && chars_item_muzzle(w, ev->ent, &mp, &md)) from = mp;
            fx_muzzle_ex(from, md, (int)((ev->id >> 4) & 0xF), (int)((ev->id >> 8) & 0xF), false);
            if (((ev->id >> 12) & 0xF) != 3u) {   // not shotgun shells: casing out of the right side
                vec3 right = v3_norm(v3_cross(md, v3(0, 1, 0)));
                fx_shell_ex(from, v3_add(v3_scale(right, 2.2f), v3(0, 1.4f, 0)), (int)((ev->id >> 12) & 0xF));
            }
        }
        fx_tracer(from, ev->dir);
        break;
    }
    case EV_SHOT: {
        // Another player's ballistic shot: flash + casing at their visible barrel, the report, and a
        // replay of the bullets for tracers / cracks (the host's EV_IMPACT events carry the hits).
        BalShotRecord rec;
        memcpy(&rec, ev->data, sizeof(rec));
        vec3 mp = ev->pos, md = ev->dir;
        if (ent_valid(w, ev->ent)) chars_item_muzzle(w, ev->ent, &mp, &md);
        if (w->items && rec.weapon < w->items->weapon_count)
            bal_fx_muzzle((int)ev->ent2 - 1, rec.cart, rec.device, w->items->weapons[rec.weapon].barrel_length, mp, md, false);
        if (rec.caliber != 3u) {   // not shotgun shells: casing out of the right side
            vec3 right = v3_norm(v3_cross(md, v3(0, 1, 0)));
            fx_shell_ex(mp, v3_add(v3_scale(right, 2.2f), v3(0, 1.4f, 0)), rec.caliber);
        }
        bal_on_shot_event(w, ev, mp);
        break;
    }
    case EV_MUZZLE:
        if (ent_valid(w, ev->ent)) {
            int i = (int)ENT_INDEX(ev->ent);
            vec3 eye = v3_add(w->xf[i].pos, v3(0, (w->flags[i] & EF_CROUCHED) ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
            float yaw = quat_get_yaw(w->xf[i].rot);
            fx_muzzle(v3_madd(eye, forward_from_yaw(yaw), 0.6f), forward_from_yaw(yaw));
        }
        break;
    case EV_KILL: add_kill(ev); break;
    case EV_PING: game_pings_event(ev); break;
    case EV_EXPLOSION: throwables_event(ev); break;
    case EV_REVIVE:
    case EV_DNA: revive_handle_event(ev); break;
    case EV_CONTAINER:
        // The screen opens with the loot side; the tree follows (MSG_LOOT, or the host's own).
        G->container_open = true;
        G->container_ent = ev->ent;
        if (G->mode != GM_HOST && G->loot_ent != ev->ent) { G->loot_ent = ENT_NONE; inv_tree_init(&G->loot, -1); }
        break;
    case EV_CONTAINER_CLOSE: G->container_open = false; break;
    case EV_TOD: daynight_handle_event(ev); break;
    case EV_VEHICLE: vehicles_handle_event(ev); break;
    case EV_BENCH:
        G->bench_open = true;
        G->bench_ent = ev->ent;
        break;
    case EV_RESPAWN:
        if (G->local_slot >= 0 && w->players[G->local_slot].ent == ev->ent) {
            G->view.yaw = ev->value;
            G->view.pitch = 0;
            G->view.smooth_init = false;
            G->error_offset = v3(0, 0, 0);
        }
        break;
    case EV_CHAT: game_add_hint(ev->text); break;
    default: break;
    }
}

// ------------------------------------------------------------------ RPCs

static const ItemDB *db_of(World *w) { return w->items; }

// Client: the predicted trees = the host's last trees + the unacknowledged intents re-applied.
static void cl_repredict(void) {
    World *w = G->world;
    Player *p = game_local_player();
    if (!w || !p) return;
    if (G->inv_base_valid) {
        p->inv.t = G->inv_base.t;
        p->inv.hands = G->inv_base.hands;
        p->inv.revision = G->inv_base.revision;
    }
    G->loot = G->loot_base;
    InvTree *loot = G->loot_ent ? &G->loot : NULL;
    for (int i = 0; i < G->inv_pending_count; i++) {
        InvCtx c = inv_ctx(&p->inv, db_of(w), loot);
        inv_intent_apply(&c, &G->inv_pending[i], NULL);
    }
}

int game_inv_intent(InvIntent *in) {
    if (!G || !G->world) return IIR_INVALID;
    Player *p = game_local_player();
    if (!p) return IIR_INVALID;
    if (G->mode == GM_HOST) {
        // Authoritative right away: the result is the real one.
        in->seq = 0;
        return game_sv_inv_intent_r(G->local_slot, in);
    }
    // Client: predict on the local copies, remember it until the host acknowledges it.
    if (G->inv_pending_count >= (int)ARRAY_COUNT(G->inv_pending)) return IIR_INVALID;
    in->seq = ++G->inv_seq;
    InvCtx c = inv_ctx(&p->inv, db_of(G->world), G->loot_ent ? &G->loot : NULL);
    int r = inv_intent_apply(&c, in, NULL);
    G->inv_pending[G->inv_pending_count++] = *in;
    net_send_inv_intent(in);
    return r;
}

int game_inv_pending(void) { return G ? G->inv_pending_count : 0; }

// Client: the host's inventory (acknowledging intents up to ack_seq): keep it as the base, drop
// the acknowledged intents and re-apply the rest on top (their predicted results).
void game_cl_inventory(int slot, u32 revision, u32 ack_seq, u16 hands, const InvTree *tree) {
    World *w = G ? G->world : NULL;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    G->inv_base.t = *tree;
    G->inv_base.hands = hands;
    G->inv_base.revision = revision;
    G->inv_base_valid = true;
    int n = 0;
    for (int i = 0; i < G->inv_pending_count; i++)
        if (G->inv_pending[i].seq > ack_seq) G->inv_pending[n++] = G->inv_pending[i];
    G->inv_pending_count = n;
    cl_repredict();
}

void game_cl_loot(EntityId e, u32 revision, const InvTree *tree) {
    if (!G || !G->world) return;
    if (!tree) {
        G->loot_ent = ENT_NONE;
        inv_tree_init(&G->loot_base, -1);
    } else {
        G->loot_ent = e;
        G->loot_rev = revision;
        G->loot_base = *tree;
    }
    cl_repredict();
}

InvTree *game_loot_tree(void) {
    if (!G || !G->world || !G->container_open) return NULL;
    if (G->mode == GM_HOST) {
        Player *p = game_local_player();
        return p ? container_tree(G->world, p->open_container, false) : NULL;
    }
    return G->loot_ent ? &G->loot : NULL;
}

EntityId game_loot_ent(void) { return G && G->container_open ? G->container_ent : ENT_NONE; }

void game_rpc(int type, int a, int b, int c) {
    if (!G || !G->world) return;
    if (G->mode == GM_HOST) game_sv_handle_rpc(G->local_slot, type, a, b, c);
    else net_send_rpc(type, a, b, c);
}

static void spawn_dropped(World *w, int slot, const InvTree *t, u16 uid);

// Is the open world inventory within reach (crates 3.5 m, vehicles 4.5 m)?
static bool loot_reachable(World *w, int slot, EntityId e) {
    if (!ent_valid(w, e)) return false;
    return v3_dist(w->players[slot].sim.move.pos, w->xf[ENT_INDEX(e)].pos) <= game_loot_reach(w, e);
}

static Cvar *g_inv_reach;

void game_loot_register(void) {
    g_inv_reach = cvar_reg_float("sv_inv_reach", 3.5f, 0.5f, 1000.0f, CVAR_LOCKED, "inventory: how far (m) a player may be from an open world container (vehicles + 1 m); tests raise it");
}

float game_loot_reach(World *w, EntityId e) {
    return (g_inv_reach ? g_inv_reach->f : 3.5f) + (ent_valid(w, e) && w->type[ENT_INDEX(e)] == ET_VEHICLE ? 1.0f : 0.0f);
}

// Host: an inventory intent (gameplay/inventory/inv_intent.c) plus its world effects. Every
// intent is acknowledged with an inventory message (even a rejected one), so the client's
// prediction always converges on the host's result; a changed world inventory is resent to
// everyone who has it open (net.c).
int game_sv_inv_intent_r(int slot, const InvIntent *in) {
    World *w = G->world;
    Player *p = &w->players[slot];
    EntityId oc = p->open_container;
    InvTree *loot = container_tree(w, oc, false);
    InvCtx c = inv_ctx(&p->inv, w->items, loot);
    c.loot_reachable = loot && loot_reachable(w, slot, oc);
    InvEffect fx;
    int r = inv_intent_apply(&c, in, &fx);
    if (in->seq > p->inv_ack_seq) p->inv_ack_seq = in->seq;
    p->inv.revision++;
    if (r != IIR_OK) return r;
    bool touched_loot = in->src_side == INV_SIDE_LOOT || in->dst_side == INV_SIDE_LOOT || in->type == II_TAKE_ALL || in->type == II_QUICK_MOVE;
    if (loot && touched_loot) w->cont[ENT_INDEX(oc)].revision++;
    for (int k = 0; k < INV_MAX_ITEMS; k++) {
        const InvItem *d = &fx.dropped.items[k];
        if (inv_alive(d) && !d->parent) spawn_dropped(w, slot, &fx.dropped, d->uid);   // one pickup per discarded item
    }
    if (fx.used_item) {
        const ItemDef *d = item_def(w->items, fx.used_item);
        heal(w, p->ent, d->heal, d->armor);
        ev_hint(w, slot, "Used %s", d->name);
        ev_sound(w, "heal", p->sim.move.pos, 0.8f, EVENT_ALL);
    }
    return r;
}

void game_sv_inv_intent(int slot, const InvIntent *in) {
    World *w = G ? G->world : NULL;
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return;
    game_sv_inv_intent_r(slot, in);
}

void game_sv_loot_close(int slot) {
    World *w = G ? G->world : NULL;
    if (!w || slot < 0 || slot >= MAX_PLAYERS) return;
    w->players[slot].open_container = ENT_NONE;
    w->players[slot].loot_sent_ent = ENT_NONE;
}

static void spawn_dropped(World *w, int slot, const InvTree *t, u16 uid) {
    Player *p = &w->players[slot];
    const InvItem *top = inv_get_c(t, uid);
    if (!top) return;
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 fwd = forward_from_yaw(p->sim.move.yaw);
    // Let go at hand height in front of the player; the pickup falls onto whatever is below.
    vec3 at = v3_add(v3_madd(p->sim.move.pos, fwd, 0.6f), v3(0, fminf(player_eye_height(&p->sim.move) * 0.7f, 1.2f), 0));
    // Don't drop inside a wall.
    RayHit rh;
    if (phys_raycast(w->phys, eye, v3_sub(at, eye), PL_MASK_WORLD, phys_character_inner_body(p->character), &rh))
        at = v3_madd(rh.point, rh.normal, 0.15f);
    u16 item = top->s.item;
    spawn_pickup_tree(w, at, t, uid);   // a weapon keeps its attachments
    ev_hint(w, slot, "Dropped %s", item_def(w->items, item)->name);
}

// G tap (docs/UI_WHEELS_HUD.md "Throwables"): the held item, selection k (a weapon slot or the
// hands), goes on the ground in front of the player, whole (the stack; a weapon with its
// attachments). Host-applied as a discard, so the client's tree follows by MSG_INVENTORY.
static void drop_held(int slot, int k) {
    Player *p = &G->world->players[slot];
    const InvSlot *s = k >= 0 && k < INV_SEL_COUNT ? inv_sel_c(&p->inv, k) : NULL;
    const InvItem *it = s && s->item ? inv_item_of(&p->inv, s) : NULL;
    if (!it) return;
    InvIntent in;
    memset(&in, 0, sizeof(in));
    in.type = II_DISCARD;
    in.src_side = INV_SIDE_SELF;
    in.src = it->uid;
    game_sv_inv_intent_r(slot, &in);
}

void game_sv_handle_rpc(int slot, int type, int a, int b, int c) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return;
    Player *p = &w->players[slot];
    switch (type) {
    case RPC_INV_DROP: drop_held(slot, a); break;
    case RPC_ATTACH: game_sv_attach(w, slot, a, b, c); break;
    case RPC_SET_PART: chars_sv_player_part(w, slot, a, b); break;
    case RPC_DOOR_ACTION: door_action(w, (EntityId)(u32)a, slot, b); break;
    case RPC_PING: game_sv_ping(w, slot, a, b, c); break;
    case RPC_CONTAINER_CLOSE: game_sv_loot_close(slot); break;
    case RPC_SPECTATE: {
        // Dev spectating (alive): the host's own player, or anyone with sv_cheats.
        Cvar *cheats = cvar_find("sv_cheats");
        bool dev = c && (p->local || (cheats && cheats->i));
        spectate_request(w, slot, a, b, dev);
        break;
    }
    case RPC_RESPAWN: spectate_request_respawn(slot); break;
    case RPC_SPEC_POS: spectate_free_pos(slot, v3((float)a / 16.0f, (float)b / 16.0f, (float)c / 16.0f)); break;
    default: break;
    }
    (void)c;
}
