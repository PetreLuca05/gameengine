// Pings (docs/UI_WHEELS_HUD.md "Pings"): the local request (tap = contextual, the ping wheel), host
// validation (rate limit, range, own-ping removal) and replication as EV_PING, and the client side
// (the shared PingList, the chat line and the audio hook). Markers: src/ui/hud/hud_pings.c.
#include "game_internal.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "gameplay/weapons.h"
#include "audio/audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G (g_engine.game)

#define PING_MAX_RANGE 1000.0f   // m from the pinging player
#define PING_REMOVE_RADIUS 2.5f  // a ping this close to one of your own removes it

static struct {
    PingTypes types;
    bool loaded;
    Cvar *enable, *sound, *chat;
} PG;

const PingTypes *game_ping_types(void) {
    if (!PG.loaded) {
        PG.loaded = true;
        ping_types_defaults(&PG.types);
        char *json = platform_read_file("assets/ui/wheels/ping.json", NULL);
        char err[128];
        if (json && !ping_types_parse(&PG.types, json, err, sizeof(err))) log_warn("ping.json: %s", err);
        free(json);
    }
    return &PG.types;
}

// What the crosshair is on: the hit point and the contextual kind (ping_contextual_type).
bool game_ping_target(vec3 *pos, int *kind) {
    World *w = G ? G->world : NULL;
    Player *p = game_local_player();
    if (!w || !p) return false;
    *kind = 0;
    // The interaction focus first (pickups have no bodies).
    if (ent_valid(w, G->focus)) {
        int fi = (int)ENT_INDEX(G->focus);
        int t = w->type[fi];
        if (t == ET_PICKUP || t == ET_CONTAINER) { *pos = ent_focus_point(w, G->focus); *kind = 2; return true; }
        if (t == ET_VEHICLE || t == ET_FUEL_PUMP) { *pos = ent_focus_point(w, G->focus); *kind = 3; return true; }
    }
    vec3 dir = dir_from_angles(G->view.yaw, G->view.pitch);
    EntityId hit = ENT_NONE;
    if (!weapon_trace(w, G->local_slot, G->view.eye, dir, PING_MAX_RANGE, pos, &hit)) return false;
    if (ent_valid(w, hit)) {
        int t = w->type[ENT_INDEX(hit)];
        if (t == ET_ZOMBIE || t == ET_DUMMY) *kind = 1;
        else if (t == ET_PICKUP || t == ET_CONTAINER) *kind = 2;
        else if (t == ET_VEHICLE) *kind = 3;
        if (*kind == 1) *pos = v3_add(w->xf[ENT_INDEX(hit)].pos, v3(0, 1.9f, 0));   // above the head
    }
    return true;
}

// Local request: type < 0 = contextual.
bool game_ping_request(int type) {
    vec3 pos;
    int kind = 0;
    if (!game_ping_target(&pos, &kind)) {
        game_add_hint("Nothing to ping");
        return false;
    }
    if (type < 0) type = ping_contextual_type(kind);
    int a, b, c;
    ping_pack(type, pos, &a, &b, &c);
    game_rpc(RPC_PING, a, b, c);
    return true;
}

static void broadcast(World *w, u32 uid, int slot, int type, vec3 pos, float lifetime) {
    GameEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_PING;
    ev.target = EVENT_ALL;
    ev.except = EVENT_ALL;
    ev.id = uid;
    ev.ent = (u32)slot;
    ev.ent2 = (u32)type;
    ev.pos = pos;
    ev.value = lifetime;
    dg_strcpy(ev.text, sizeof(ev.text), w->players[slot].name);
    ev_push(w, &ev);
}

// Host: a ping on a player's behalf (auto "Need help" when downed / dead): no rate limit, the usual cap.
void game_sv_auto_ping(World *w, int slot, int type, vec3 pos) {
    if (slot < 0 || slot >= MAX_PLAYERS || !w->players[slot].active || type < 0 || type >= PING_TYPES) return;
    broadcast(w, ++G->ping_uid, slot, type, pos, game_ping_types()->t[type].lifetime);
}

// Host: RPC_PING from a player.
void game_sv_ping(World *w, int slot, int a, int b, int c) {
    int type;
    vec3 pos;
    ping_unpack(a, b, c, &type, &pos);
    Player *p = &w->players[slot];
    if (type < 0 || type >= PING_TYPES || p->sim.move.mode == MOVE_DEAD) return;
    if (v3_dist(p->sim.move.pos, pos) > PING_MAX_RANGE + 5.0f) return;
    // Pinging on top of one of your own pings takes it away.
    const Ping *own = ping_find_near(&G->pings, slot, pos, PING_REMOVE_RADIUS);
    if (own) { broadcast(w, own->uid, slot, own->type, own->pos, -1.0f); return; }
    if (!ping_rate_ok(&G->ping_limit, slot, (float)w->time)) {
        ev_hint(w, slot, "Pinging too fast");
        return;
    }
    broadcast(w, ++G->ping_uid, slot, type, pos, game_ping_types()->t[type].lifetime);
}

// Every machine: EV_PING.
void game_pings_event(const GameEvent *ev) {
    int owner = (int)ev->ent, type = (int)ev->ent2;
    if (ev->value < 0) { ping_remove(&G->pings, ev->id); return; }
    if (PG.enable && !PG.enable->i && owner != G->local_slot) return;
    ping_add(&G->pings, ev->id, owner, type, ev->pos, G->hud_time, ev->value);
    const PingTypeDef *t = &game_ping_types()->t[DG_CLAMP(type, 0, PING_TYPES - 1)];
    if (!PG.chat || PG.chat->i) {
        char line[96];
        snprintf(line, sizeof(line), "%s %s", ev->text[0] ? ev->text : "Someone", t->chat);
        game_add_hint(line);
    }
    if ((!PG.sound || PG.sound->i) && t->sound[0]) audio_play_2d(t->sound, 0.55f, SND_UI);
}

void game_pings_tick(float dt) {
    (void)dt;
    ping_expire(&G->pings, G->hud_time);
}

void game_pings_reset(void) {
    if (!G) return;
    memset(&G->pings, 0, sizeof(G->pings));
    memset(&G->ping_limit, 0, sizeof(G->ping_limit));
}

static void cmd_ping(int argc, const char **argv) {
    int type = -1;
    if (argc > 1) {
        type = ping_type_from_id(argv[1]);
        if (type < 0) { log_info("ping [generic|enemy|loot|danger|go|help|vehicle|defend]"); return; }
    }
    game_ping_request(type);
}

static void cmd_ping_list(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (!G) return;
    for (int i = 0; i < PING_MAX; i++) {
        const Ping *p = &G->pings.p[i];
        if (!p->active) continue;
        log_info("ping %u: %s by slot %d at %.1f %.1f %.1f, %.1f s left", p->uid, ping_type_id(p->type), p->owner, p->pos.x, p->pos.y, p->pos.z, p->expires - G->hud_time);
    }
    log_info("%d ping(s)", ping_count(&G->pings));
}

void game_pings_register(void) {
    PG.enable = cvar_reg_bool("ping_enable", true, CVAR_ARCHIVE | CVAR_EDITABLE, "pings: show other players' pings (markers, compass, chat)");
    PG.sound = cvar_reg_bool("ping_sound", true, CVAR_ARCHIVE | CVAR_EDITABLE, "pings: play the ping sound");
    PG.chat = cvar_reg_bool("ping_chat", true, CVAR_ARCHIVE | CVAR_EDITABLE, "pings: a chat line for each new ping");
    cmd_register("ping", cmd_ping, "ping what the crosshair is on (contextual), or a type: ping enemy");
    cmd_register("ping_list", cmd_ping_list, "list the live pings");
}
