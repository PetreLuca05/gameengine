// Client-side event handling, HUD timers, and inventory/container RPCs.
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "gameplay/effects.h"
#include "gameplay/flesh_fx.h"
#include "gameplay/entities.h"
#include "gameplay/characters.h"
#include "net/net.h"
#include "audio/audio.h"
#include "anim/body_parts.h"
#include "ui/crosshair.h"
#include <stdio.h>
#include <math.h>

#define G (g_engine.game)

void game_add_hint(const char *text) {
    if (!G) return;
    memmove(&G->hints[1], &G->hints[0], sizeof(Hint) * (MAX_HINTS - 1));
    dg_strcpy(G->hints[0].text, sizeof(G->hints[0].text), text);
    G->hints[0].time = 4.0f;
}

static void add_kill(const char *text) {
    memmove(&G->killfeed[1], &G->killfeed[0], sizeof(Hint) * (MAX_KILLFEED - 1));
    dg_strcpy(G->killfeed[0].text, sizeof(G->killfeed[0].text), text);
    G->killfeed[0].time = 6.0f;
}

void game_hud_tick(float dt) {
    for (int i = 0; i < MAX_HINTS; i++) if (G->hints[i].time > 0) G->hints[i].time -= dt;
    for (int i = 0; i < MAX_KILLFEED; i++) if (G->killfeed[i].time > 0) G->killfeed[i].time -= dt;
    for (int i = 0; i < MAX_HURT_MARKERS; i++) if (G->hurt[i].time > 0) G->hurt[i].time -= dt;
    if (G->hitmarker_time > 0) G->hitmarker_time -= dt;
    G->damage_flash = fmaxf(0.0f, G->damage_flash - dt * 1.5f);
    fx_update(dt);
    flesh_fx_update(G->world, dt);
    // Hold-interaction progress for the HUD: authoritative value (host) or replicated (client).
    Player *p = game_local_player();
    G->hold_progress = p ? p->hold_progress : 0;
}

void game_handle_event(const GameEvent *ev) {
    World *w = G->world;
    switch (ev->type) {
    case EV_HINT: game_add_hint(ev->text); break;
    case EV_SOUND: audio_play(ev->text, ev->pos, ev->value, SND_SFX); break;
    case EV_DAMAGE_NUMBER: break;   // floating damage numbers removed (hit markers / damage effects remain)
    case EV_HIT_CONFIRM:
        G->hitmarker_time = crosshair_hitmarker_duration();
        G->hitmarker_kill = (ev->id & 1u) != 0;
        G->hitmarker_head = (ev->id & 2u) != 0;
        audio_play_2d(G->hitmarker_kill ? "hit_kill" : (G->hitmarker_head ? "hit_head" : "hit"), 0.5f, SND_UI);
        break;
    case EV_HURT: {
        for (int i = 0; i < MAX_HURT_MARKERS; i++) {
            if (G->hurt[i].time > 0) continue;
            G->hurt[i].from = v3_sub(ev->pos, v3_scale(ev->dir, 5.0f));
            G->hurt[i].time = 1.5f;
            break;
        }
        G->damage_flash = fminf(1.0f, G->damage_flash + ev->value / 40.0f);
        audio_play_2d("hurt", 0.7f, SND_SFX);
        break;
    }
    case EV_IMPACT: {
        bool predicted = ev->ent2 && (int)ev->ent2 - 1 == G->local_slot && fx_predict_impacts();
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
            flesh_fx_hit(w, &fh);
            break;
        }
        // Own shots were predicted locally (cl_predict_impacts): skip the host's copy.
        if (predicted) break;
        vec3 inc;
        memcpy(&inc, ev->data, sizeof(inc));
        fx_impact_ex(ev->pos, ev->dir, inc, (int)(ev->id & 0xFF), (int)((ev->id >> 8) & 0xF), (ev->id >> 12) & 0xF);
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
    case EV_MUZZLE:
        if (ent_valid(w, ev->ent)) {
            int i = (int)ENT_INDEX(ev->ent);
            vec3 eye = v3_add(w->xf[i].pos, v3(0, (w->flags[i] & EF_CROUCHED) ? PLAYER_EYE_CROUCH : PLAYER_EYE_STAND, 0));
            float yaw = quat_get_yaw(w->xf[i].rot);
            fx_muzzle(v3_madd(eye, forward_from_yaw(yaw), 0.6f), forward_from_yaw(yaw));
        }
        break;
    case EV_KILL: add_kill(ev->text); break;
    case EV_CONTAINER:
        G->container_open = true;
        G->container_ent = ev->ent;
        memcpy(G->container_slots, ev->data, sizeof(G->container_slots));
        G->inv_selected = -1;
        break;
    case EV_CONTAINER_CLOSE: G->container_open = false; break;
    case EV_TOD: daynight_handle_event(ev); break;
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

void game_rpc(int type, int a, int b, int c) {
    if (!G || !G->world) return;
    if (G->mode == GM_HOST) game_sv_handle_rpc(G->local_slot, type, a, b, c);
    else net_send_rpc(type, a, b, c);
}

static void send_container(World *w, int slot, EntityId cont) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_CONTAINER;
    ev.target = (u8)slot;
    ev.except = EVENT_ALL;
    ev.ent = cont;
    memcpy(ev.data, w->cont[ENT_INDEX(cont)].slots, sizeof(w->cont[0].slots));
    ev_push(w, &ev);
}

static void drop_item(World *w, int slot, int inv_slot) {
    Player *p = &w->players[slot];
    if (inv_slot < 0 || inv_slot >= INV_SLOTS) return;
    InvSlot s = p->inv.slots[inv_slot];
    if (!s.item) return;
    memset(&p->inv.slots[inv_slot], 0, sizeof(InvSlot));
    p->inv.revision++;
    vec3 eye = player_eye_pos(&p->sim.move);
    vec3 fwd = forward_from_yaw(p->sim.move.yaw);
    // Let go at hand height in front of the player; the pickup falls onto whatever is below.
    vec3 at = v3_add(v3_madd(p->sim.move.pos, fwd, 0.6f), v3(0, fminf(player_eye_height(&p->sim.move) * 0.7f, 1.2f), 0));
    // Don't drop inside a wall.
    RayHit rh;
    if (phys_raycast(w->phys, eye, v3_sub(at, eye), PL_MASK_WORLD, phys_character_inner_body(p->character), &rh))
        at = v3_madd(rh.point, rh.normal, 0.15f);
    EntityId pe = spawn_pickup(w, at, s.item, s.count, s.ammo, 0);
    if (ent_valid(w, pe)) {
        // The dropped weapon keeps its attachments.
        memcpy(w->pickup[ENT_INDEX(pe)].att, s.att, sizeof(s.att));
        memcpy(w->attach[ENT_INDEX(pe)], s.att, sizeof(s.att));
    }
    ev_hint(w, slot, "Dropped %s", item_def(w->items, s.item)->name);
}

void game_sv_handle_rpc(int slot, int type, int a, int b, int c) {
    World *w = G->world;
    if (!w || slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active) return;
    Player *p = &w->players[slot];
    switch (type) {
    case RPC_INV_MOVE: inv_move(&p->inv, w->items, a, b); break;
    case RPC_INV_DROP: drop_item(w, slot, a); break;
    case RPC_ATTACH: game_sv_attach(w, slot, a, b, c); break;
    case RPC_CONTAINER_TAKE: {
        EntityId ce = p->open_container;
        if (!ent_valid(w, ce) || w->type[ENT_INDEX(ce)] != ET_CONTAINER) return;
        if (v3_dist(p->sim.move.pos, w->xf[ENT_INDEX(ce)].pos) > 3.5f) return; // validate proximity
        Container *ct = &w->cont[ENT_INDEX(ce)];
        int first = a == 255 ? 0 : a, last = a == 255 ? CONTAINER_SLOTS - 1 : a;
        for (int s = DG_MAX(0, first); s <= DG_MIN(CONTAINER_SLOTS - 1, last); s++) {
            InvSlot *cs = &ct->slots[s];
            if (!cs->item) continue;
            int left = inv_add_att(&p->inv, w->items, cs->item, cs->count, cs->ammo, cs->att);
            if (left == cs->count) { ev_hint(w, slot, "Inventory full"); break; }
            if (left > 0) cs->count = (u16)left;
            else memset(cs, 0, sizeof(*cs));
        }
        ct->revision++;
        send_container(w, slot, ce);
        break;
    }
    case RPC_CONTAINER_PUT: {
        EntityId ce = p->open_container;
        if (!ent_valid(w, ce) || a < 0 || a >= INV_SLOTS) return;
        InvSlot *s = &p->inv.slots[a];
        if (!s->item) return;
        int left = slots_add_att(w->cont[ENT_INDEX(ce)].slots, CONTAINER_SLOTS, w->items, s->item, s->count, s->ammo, s->att, 0, CONTAINER_SLOTS);
        if (left > 0) s->count = (u16)left;
        else memset(s, 0, sizeof(*s));
        p->inv.revision++;
        send_container(w, slot, ce);
        break;
    }
    case RPC_CONTAINER_CLOSE: p->open_container = ENT_NONE; break;
    default: break;
    }
    (void)c;
}
