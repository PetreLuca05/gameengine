// Lobby glue (src/net/lobby_net.h, docs/MENU_LOBBY.md).
//
//   host:   lobby_host -> net_host (LAN: discovery answers; Steam: a Steam lobby of the privacy's type)
//           -> HELLO from a client -> lobby_on_hello (version, ban, size) -> member, snapshot to all
//           -> intents (ready, chat, map, settings, kick, promote, start) validated by lobby_host_intent
//           -> start: LOP_START to all, map load (next frame, the loading screen is up), WELCOME each
//           member (net_host_welcome), streaming done -> IN_GAME
//   client: lobby_join -> net_connect -> HELLO -> snapshot (LOBBY) ... LOP_START (LOADING) -> WELCOME
//           (map load) -> streaming done -> IN_GAME
//   migration (lobby only): the host leaves -> LOP_MIGRATE to all (or the connection drops): every
//           member picks the same successor (lobby_successor); it hosts, the others reconnect to it.
#include "lobby_net.h"
#include "protocol.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/consistency.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "game/scene.h"
#include "world/chunks.h"
#include "world/season.h"
#include "ui/mainmenu/mainmenu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DG_HAVE_STEAM
#include "steam_shim.h"
#endif

#define MIGRATE_RECONNECT_DELAY 2.0   // s: the successor needs a moment to listen
#define MIGRATE_TIMEOUT 15.0
#define JOIN_TIMEOUT 20.0             // s in JOINING (the transport times out at 10 s on its own)
#define LOAD_MIN_SCREEN 0.6           // s the loading screen stays up at least
#define MAX_DROPS 8

typedef struct PendingDrop { int peer; double at; } PendingDrop;

typedef struct LobbySys {
    Lobby L;
    TransportKind kind;
    MapList maps;
    CosmeticDB cos;
    Profile prof;
    u32 version_hash;
    double state_time;            // when the current state began
    // host
    bool welcomed[64];            // per transport peer: WELCOME sent (the game is running)
    PendingDrop drops[MAX_DROPS]; // kicked peers: dropped after the LOP_KICKED went out
    int drop_count;
    double ping_time;
    // loading
    int load_phase;               // 0 none, 1 screen up (host: load next frames), 2 loaded (streaming)
    u64 load_frame;
    double load_start;
    // migration
    u8 successor;
    char successor_addr[64];
    double migrate_at;            // reconnect / host at this time
    bool migrate_done;
    // deferred game transitions (never inside the net poll loop)
    bool pending_menu;
    char pending_msg[160];
    // Steam mirror
    u32 meta_rev;
    int meta_state;
    u64 meta_lobby;
    char presence[128];
    double presence_time;
    u8 buf[4096];
    Cvar *privacy;                // lobby_privacy: read by the Steam transport when it creates the lobby
} LobbySys;

static LobbySys *S;

static double now(void) { return platform_time(); }

static void set_state(LobbyEvent e) {
    LobbyState n = lobby_fsm_next(S->L.state, e);
    if (n == S->L.state) return;
    log_info("lobby: %s + %s -> %s", lobby_state_name(S->L.state), lobby_event_name(e), lobby_state_name(n));
    S->L.state = n;
    S->state_time = now();
}

const Lobby *lobby_get(void) { return &S->L; }
LobbyState lobby_state(void) { return S ? S->L.state : LS_IDLE; }
bool lobby_is_host(void) { return S && S->L.is_host && lobby_state_in_session(S->L.state); }
bool lobby_active(void) { return S && lobby_state_in_session(S->L.state); }
const char *lobby_closed_reason(void) { return S->L.close_reason; }
TransportKind lobby_transport(void) { return S->kind; }
const MapList *lobby_maps(void) { return S ? &S->maps : NULL; }
const MapInfo *lobby_map_info(const char *id) { return maplist_find(&S->maps, id); }
const CosmeticDB *lobby_cosmetics(void) { return &S->cos; }
Profile *lobby_profile(void) { return &S->prof; }
u32 lobby_version_hash_local(void) { return S ? S->version_hash : 0; }

bool lobby_steam_running(void) { return net_steam_available(); }

const char *lobby_local_name(void) {
#ifdef DG_HAVE_STEAM
    if (dgs_available() && dgs_persona_name()[0]) return dgs_persona_name();
#endif
    Game *g = game();
    return g && g->cl_name ? g->cl_name->str : "Survivor";
}

static u64 local_steam_id(void) {
#ifdef DG_HAVE_STEAM
    return dgs_available() ? dgs_my_id() : 0;
#else
    return 0;
#endif
}

float lobby_relay_ping_ms(void) {
#ifdef DG_HAVE_STEAM
    if (dgs_available()) return dgs_relay_ping_ms();
#endif
    return -1.0f;
}

static void local_line(int kind, const char *fmt, const char *arg) {
    lobby_chat_system(&S->L, now(), kind, fmt, arg ? arg : "");
}

// ------------------------------------------------------------------ sending

static bool send_to(int peer, const Writer *w) { return !w->overflow && net_send_reliable(peer, w->buf, w->len); }

static void send_snapshot_to(const LobbyMember *m) {
    if (m->peer < 0) return;
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    lobby_write_snapshot(&w, &S->L, m->id);
    send_to(m->peer, &w);
}

static void broadcast_snapshot(void) {
    if (!S->L.is_host) return;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (S->L.members[i].active) send_snapshot_to(&S->L.members[i]);
}

static void broadcast_chat(const LobbyChatLine *line) {
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    lobby_write_chat(&w, line);
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (S->L.members[i].active && S->L.members[i].peer >= 0) send_to(S->L.members[i].peer, &w);
}

static void log_chat(const LobbyChatLine *l) {
    if (l->from == LOBBY_SYSTEM) log_info("lobby chat: * %s", l->text);
    else log_info("lobby chat: %s: %s", l->name, l->text);
}

static void broadcast_newest_chat(void) {
    const LobbyChatLine *l = lobby_chat_at(&S->L, S->L.chat_count - 1);
    if (l) { log_chat(l); broadcast_chat(l); }
}

static void send_op_text(int peer, int op, const char *text) {
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    w_u8(&w, (u8)op);
    w_str(&w, text);
    send_to(peer, &w);
}

static void send_start(int peer) {
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    w_u8(&w, LOP_START);
    w_str(&w, S->L.settings.map);
    send_to(peer, &w);
}

// ------------------------------------------------------------------ data / profile

static bool map_check(const char *map, void *user) {
    (void)user;
    return maplist_find(&S->maps, map) != NULL;
}

void lobby_profile_changed(void) {
    const char *skin = cosmetics_body_skin(&S->cos, &S->prof.loadout);
    char parts[256];
    cosmetics_parts_string(&S->cos, &S->prof.loadout, parts, sizeof(parts));
    Cvar *cs = cvar_find("cl_skin"), *cp = cvar_find("cl_parts");
    // The body outfit drives cl_skin once the profile has one (or the player changes it); an older
    // cl_skin chosen in Settings > Character stays until then.
    static char last_body[32];
    static bool first = true;
    const char *body = S->prof.loadout.equipped[CSLOT_BODY];
    bool body_changed = first ? body[0] != 0 : strcmp(last_body, body) != 0;
    first = false;
    dg_strcpy(last_body, sizeof(last_body), body);
    if (cs && body_changed && strcmp(cs->str, skin)) cvar_set_cv(cs, skin);
    if (cp && strcmp(cp->str, parts)) cvar_set_cv(cp, parts);
    profile_save(&S->prof);
}

// ------------------------------------------------------------------ season / start weather

// Host: start weather must be one of the season's presets (or "" = the season's random pick).
static bool settings_weather_ok(const LobbySettings *s) {
    if (!s->weather[0]) return true;
    const SeasonTable *t = weather_seasons();
    return s->season < SEASON_COUNT && season_allows(&t->s[s->season], s->weather);
}

// The game's look from the lobby settings: the season's calendar day, the start time, the season's
// weather (the given start preset or a pick), dynamic or fixed.
void lobby_apply_environment(World *w, const LobbySettings *s) {
    if (!w || !w->authority) return;
    const SeasonTable *t = weather_seasons();
    int season = s->season < SEASON_COUNT ? s->season : SEASON_SUMMER;
    daynight_set_calendar(w, t->s[season].day_of_year, (float)s->start_hour);
    weather_apply_season(w, season, s->weather, s->dynamic_weather);
    // Server rules (CVAR_RULE): the host's values replicate to every client (MSG_CVARS).
    Cvar *fd = cvar_find("r_foliage_density");
    if (fd) {
        CvarSource src = cvar_source_push(CVAR_SRC_HOST);
        cvar_set_float(fd, s->foliage_pct ? (float)DG_CLAMP(s->foliage_pct, 50, 100) / 100.0f : 1.0f);
        cvar_source_pop(src);
    }
}

// ------------------------------------------------------------------ session

static void reset_session(void) {
    LobbyState st = S->L.state;
    lobby_init(&S->L);
    S->L.state = st;
    memset(S->welcomed, 0, sizeof(S->welcomed));
    S->drop_count = 0;
    S->load_phase = 0;
    S->migrate_at = 0;
    S->migrate_done = false;
    S->meta_rev = 0;
    S->meta_lobby = 0;
}

static void close_with(LobbyEvent e, const char *reason) {
    dg_strcpy(S->L.close_reason, sizeof(S->L.close_reason), reason ? reason : "");
    log_info("lobby: closed (%s): %s", lobby_event_name(e), S->L.close_reason);
    set_state(e);
    if (S->L.state != LS_CLOSED) { S->L.state = LS_CLOSED; S->state_time = now(); }
}

// Back to the main menu (deferred to lobby_net_update: never inside the net poll).
static void go_menu(const char *msg) {
    S->pending_menu = true;
    dg_strcpy(S->pending_msg, sizeof(S->pending_msg), msg ? msg : "");
}

static bool start_hosting(TransportKind kind) {
    Cvar *mp = cvar_find("sv_maxplayers");
    if (mp) cvar_set_int(mp, S->L.settings.max_players);
    cvar_set_int(S->privacy, S->L.settings.privacy);
    return net_host(kind, (u16)net_listen_port());
}

static void host_identity(char *ident, int ni, char *addr, int na) {
    u64 sid = local_steam_id();
    if (sid) { snprintf(ident, (size_t)ni, "steam:%llu", (unsigned long long)sid); snprintf(addr, (size_t)na, "%llu", (unsigned long long)sid); }
    else { snprintf(ident, (size_t)ni, "lan:local/%s", lobby_local_name()); snprintf(addr, (size_t)na, "127.0.0.1:%d", net_listen_port()); }
}

bool lobby_host(TransportKind kind) {
    if (lobby_active()) lobby_leave("hosting a new lobby");
    if (S->L.state == LS_CLOSED) set_state(LE_ACK);
    reset_session();
    LobbySettings s = S->prof.lobby;
    const char *sel = mainmenu_selected_map();
    if (sel && sel[0] && maplist_find(&S->maps, sel)) dg_strcpy(s.map, sizeof(s.map), sel);
    if (!maplist_find(&S->maps, s.map) && S->maps.count) dg_strcpy(s.map, sizeof(s.map), S->maps.maps[0].id);
    if (kind == TRANSPORT_LAN && s.privacy == LP_FRIENDS) s.privacy = LP_PUBLIC;   // no friends on a LAN
    if (!settings_weather_ok(&s)) s.weather[0] = 0;
    S->kind = kind;
    set_state(LE_HOST);
    char ident[64], addr[64];
    host_identity(ident, sizeof(ident), addr, sizeof(addr));
    lobby_host_begin(&S->L, lobby_local_name(), local_steam_id(), ident, addr, &s);
    if (!game_is_host()) mainmenu_enter(NULL);   // a menu world to host from
    if (!start_hosting(kind)) {
        close_with(LE_TRANSPORT_FAIL, kind == TRANSPORT_STEAM ? "Could not create the Steam lobby (is Steam running?)" : "Could not open the LAN port (another game hosting on this machine?)");
        return false;
    }
    set_state(LE_TRANSPORT_UP);
    lobby_chat_system(&S->L, now(), LC_SYSTEM, "Lobby created (%s, %s)", kind == TRANSPORT_STEAM ? "Steam" : "LAN", lobby_privacy_name(s.privacy));
    return true;
}

bool lobby_join(TransportKind kind, const char *address) {
    if (lobby_active()) lobby_leave("joining another lobby");
    if (S->L.state == LS_CLOSED) set_state(LE_ACK);
    reset_session();
    S->kind = kind;
    set_state(LE_JOIN);
    if (!game_is_host()) mainmenu_enter(NULL);
    if (!net_connect(kind, address)) {
        close_with(LE_TRANSPORT_FAIL, "Could not connect");
        return false;
    }
    lobby_chat_system(&S->L, now(), LC_SYSTEM, "Connecting to %s ...", address);
    return true;
}

// Host leaving the lobby with others in it: they move to the successor.
static void announce_migration(u8 successor) {
    LobbyMember *m = lobby_member(&S->L, successor);
    if (!m) return;
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    w_u8(&w, LOP_MIGRATE);
    w_u8(&w, successor);
    w_str(&w, m->address);
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (S->L.members[i].active && S->L.members[i].peer >= 0) send_to(S->L.members[i].peer, &w);
    net_flush();
#ifdef DG_HAVE_STEAM
    if (m->steam_id && dgs_available()) dgs_set_lobby_owner(m->steam_id);
#endif
}

void lobby_leave(const char *why) {
    if (!S || S->L.state == LS_IDLE) return;
    LobbyState st = S->L.state;
    if (S->L.is_host && (st == LS_LOBBY || st == LS_COUNTDOWN) && lobby_count(&S->L) > 1) {
        u8 succ = lobby_successor(&S->L, S->L.local_id);
        if (succ != LOBBY_NONE) announce_migration(succ);
    }
    log_info("lobby: leaving (%s)", why ? why : "");
    net_stop();
    S->L.state = lobby_fsm_next(st, LE_LEAVE);
    S->state_time = now();
    reset_session();
    S->L.state = LS_IDLE;
    S->L.chat_count = S->L.chat_head = 0;
    // The world stays: the caller returns to the menu (pause > Leave to main menu) or is in it.
}

void lobby_ack_closed(void) {
    if (S->L.state == LS_CLOSED) set_state(LE_ACK);
    S->L.close_reason[0] = 0;
}

// ------------------------------------------------------------------ intents

static void apply_outcome(u8 from, const LobbyOutcome *o);

static void host_apply(u8 from, const LobbyIntent *in) {
    if (in->type == LI_SETTINGS && !settings_weather_ok(&in->settings)) {
        LobbyOutcome o;
        memset(&o, 0, sizeof(o));
        o.kicked = o.promoted = LOBBY_NONE;
        o.result = LR_INVALID;
        dg_strcpy(o.error, sizeof(o.error), "That weather does not happen in this season");
        apply_outcome(from, &o);
        return;
    }
    LobbyOutcome o = lobby_host_intent(&S->L, from, in, now(), map_check, NULL);
    apply_outcome(from, &o);
}

static void begin_loading(void) {
    if (S->L.state == LS_LOBBY || S->L.state == LS_COUNTDOWN) set_state(LE_START);
    if (S->L.state != LS_LOADING) return;
    S->L.countdown = -1.0f;
    S->load_phase = 1;
    S->load_frame = g_engine.frame_index;
    S->load_start = now();
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (S->L.members[i].active && S->L.members[i].peer >= 0) send_start(S->L.members[i].peer);
    net_flush();
    S->L.revision++;
}

static void apply_outcome(u8 from, const LobbyOutcome *o) {
    LobbyMember *m = lobby_member(&S->L, from);
    if (o->result == LR_DENIED || o->result == LR_INVALID || o->result == LR_RATE_LIMITED) {
        if (!m) return;
        if (m->peer < 0) local_line(LC_ERROR, "%s", o->error);
        else send_op_text(m->peer, LOP_ERROR, o->error);
        return;
    }
    if (o->chat) broadcast_newest_chat();
    if (o->countdown) set_state(LE_FORCE_START);
    if (o->changed) {
        if (S->L.state == LS_COUNTDOWN && S->L.countdown < 0) set_state(LE_COUNTDOWN_CANCEL);
        Cvar *mp = cvar_find("sv_maxplayers");
        if (mp && mp->i != S->L.settings.max_players) cvar_set_int(mp, S->L.settings.max_players);
        broadcast_snapshot();
        mainmenu_lobby_settings_changed();
    }
    if (o->promoted != LOBBY_NONE) {
        // Voluntary migration: everyone (us included) moves to the promoted member.
        LobbyMember *t = lobby_member(&S->L, o->promoted);
        if (t) {
            S->successor = t->id;
            dg_strcpy(S->successor_addr, sizeof(S->successor_addr), t->address);
            announce_migration(t->id);
            LobbySettings keep = S->L.settings;
            net_stop();
            S->L.is_host = false;
            S->L.settings = keep;
            set_state(LE_HOST_LOST);
            S->migrate_at = now() + MIGRATE_RECONNECT_DELAY;
        }
    }
    if (o->start) begin_loading();
}

static void kick_peer(int peer, const char *reason) {
    send_op_text(peer, LOP_KICKED, reason);
    net_flush();
    if (S->drop_count < MAX_DROPS) { S->drops[S->drop_count].peer = peer; S->drops[S->drop_count].at = now() + 0.3; S->drop_count++; }
    else net_drop_peer(peer);
}

static void send_intent(const LobbyIntent *in) {
    if (!lobby_active()) return;
    if (S->L.is_host) {
        if (in->type == LI_KICK) {
            LobbyMember *t = lobby_member(&S->L, in->a);
            int peer = t ? t->peer : -1;
            LobbyOutcome o = lobby_host_intent(&S->L, S->L.local_id, in, now(), map_check, NULL);
            if (o.kicked != LOBBY_NONE && peer >= 0) kick_peer(peer, "You were kicked by the host");
            apply_outcome(S->L.local_id, &o);
            return;
        }
        host_apply(S->L.local_id, in);
        return;
    }
    Writer w = writer_make(S->buf, sizeof(S->buf));
    w_u8(&w, MSG_LOBBY);
    lobby_write_intent(&w, in);
    send_to(0, &w);
}

static LobbyIntent mk(int type, int a, const char *text) {
    LobbyIntent in;
    memset(&in, 0, sizeof(in));
    in.type = (u8)type;
    in.a = (u8)a;
    if (text) dg_strcpy(in.text, sizeof(in.text), text);
    return in;
}

void lobby_chat(const char *text) {
    if (!lobby_active() || !text || !text[0]) return;
    LobbyIntent in;
    char err[160];
    if (!lobby_parse_chat(&S->L, text, &in, err, sizeof(err))) { local_line(LC_ERROR, "%s", err); return; }
    send_intent(&in);
}

void lobby_set_ready(bool ready) { LobbyIntent in = mk(LI_READY, ready ? 1 : 0, NULL); send_intent(&in); }
void lobby_set_map(const char *map) { LobbyIntent in = mk(LI_MAP, 0, map); send_intent(&in); }
void lobby_kick(u8 member) { LobbyIntent in = mk(LI_KICK, member, NULL); send_intent(&in); }
void lobby_promote(u8 member) { LobbyIntent in = mk(LI_PROMOTE, member, NULL); send_intent(&in); }
void lobby_start(bool force) { LobbyIntent in = mk(LI_START, force ? 1 : 0, NULL); send_intent(&in); }
void lobby_cancel_start(void) { LobbyIntent in = mk(LI_CANCEL, 0, NULL); send_intent(&in); }

void lobby_set_settings(const LobbySettings *s) {
    LobbyIntent in = mk(LI_SETTINGS, 0, NULL);
    in.settings = *s;
    send_intent(&in);
    // The host's choices become the defaults of its next lobby.
    if (S->L.is_host) { S->prof.lobby = S->L.settings; dg_strcpy(S->prof.last_map, sizeof(S->prof.last_map), S->L.settings.map); profile_save(&S->prof); }
}

void lobby_invite_overlay(void) { net_steam_invite(); }

// ------------------------------------------------------------------ net.c hooks

void lobby_write_hello_ext(Writer *w) {
    w_u32(w, S->version_hash);
    w_str(w, DG_VERSION);
    u64 sid = local_steam_id();
    w_u32(w, (u32)(sid & 0xFFFFFFFFu));
    w_u32(w, (u32)(sid >> 32));
    w_u16(w, (u16)net_listen_port());
}

void lobby_read_hello_ext(Reader *r, LobbyHello *h) {
    memset(h, 0, sizeof(*h));
    h->build_hash = r_u32(r);
    r_str(r, h->build_version, sizeof(h->build_version));
    u32 lo = r_u32(r), hi = r_u32(r);
    h->steam_id = (u64)lo | ((u64)hi << 32);
    h->listen_port = r_u16(r);
    if (r->overflow) memset(h, 0, sizeof(*h));
}

int lobby_on_hello(int peer, const LobbyHello *h, char *reason, int reason_size) {
    if (!lobby_check_version(S->version_hash, DG_VERSION, h->build_hash, h->build_version[0] ? h->build_version : "?", reason, reason_size)) return LJ_REJECT;
    if (!lobby_is_host()) return LJ_WELCOME;   // console hosting (no lobby): straight into the game
    char ip[64] = "", ident[64], addr[64];
    net_peer_address(peer, ip, sizeof(ip));
    if (h->steam_id) {
        snprintf(ident, sizeof(ident), "steam:%llu", (unsigned long long)h->steam_id);
        snprintf(addr, sizeof(addr), "%llu", (unsigned long long)h->steam_id);
    } else {
        snprintf(ident, sizeof(ident), "lan:%.40s/%.18s", ip, h->name);
        snprintf(addr, sizeof(addr), "%.48s:%u", ip[0] ? ip : "127.0.0.1", (unsigned)(h->listen_port ? h->listen_port : DG_DEFAULT_PORT));
    }
    u8 id = lobby_host_add(&S->L, h->name, h->steam_id, ident, addr, peer, now(), reason, reason_size);
    if (id == LOBBY_NONE) return LJ_REJECT;
    if (peer >= 0 && peer < 64) S->welcomed[peer] = false;
    broadcast_snapshot();
    broadcast_newest_chat();
    mainmenu_lobby_settings_changed();
    if (S->L.state == LS_LOADING || S->L.state == LS_IN_GAME) {
        send_start(peer);
        // The map is up (in game, or loaded and streaming): straight in. Still loading: welcomed by
        // host_load_map with everyone else.
        if (S->L.state == LS_IN_GAME || S->load_phase >= 2) { if (peer >= 0 && peer < 64) S->welcomed[peer] = true; return LJ_WELCOME; }
    }
    return LJ_LOBBY;
}

void lobby_on_peer_left(int peer) {
    if (peer >= 0 && peer < 64) S->welcomed[peer] = false;
    if (!lobby_is_host()) return;
    LobbyMember *m = lobby_member_by_peer(&S->L, peer);
    if (!m) return;
    lobby_host_remove(&S->L, m->id, now(), "left");
    broadcast_snapshot();
    broadcast_newest_chat();
    mainmenu_lobby_settings_changed();
}

void lobby_on_host_msg(int peer, Reader *r) {
    if (!lobby_is_host()) return;
    LobbyMember *m = lobby_member_by_peer(&S->L, peer);
    if (!m) return;
    u8 op = r_u8(r);
    if (op != LOP_INTENT) return;
    LobbyIntent in;
    if (!lobby_read_intent(r, &in)) { send_op_text(peer, LOP_ERROR, "Message too long"); return; }
    if (in.type == LI_KICK) {
        // Validated by the model (host only); a member's request is refused there.
        LobbyMember *t = lobby_member(&S->L, in.a);
        int tpeer = t ? t->peer : -1;
        LobbyOutcome o = lobby_host_intent(&S->L, m->id, &in, now(), map_check, NULL);
        if (o.kicked != LOBBY_NONE && tpeer >= 0) kick_peer(tpeer, "You were kicked by the host");
        apply_outcome(m->id, &o);
        return;
    }
    host_apply(m->id, &in);
}

void lobby_on_client_msg(Reader *r) {
    if (!lobby_active() || S->L.is_host) return;
    u8 op = r_u8(r);
    switch (op) {
    case LOP_SNAPSHOT: {
        LobbyState before = S->L.state;
        if (!lobby_read_snapshot(r, &S->L)) { log_warn("lobby: malformed snapshot"); return; }
        if (before == LS_JOINING || before == LS_MIGRATING) {
            set_state(LE_SNAPSHOT);
            const LobbyMember *h = lobby_host_member(&S->L);
            lobby_chat_system(&S->L, now(), LC_SYSTEM, "Joined %s's lobby", h ? h->name : "the");
            S->migrate_at = 0;
        }
        mainmenu_lobby_settings_changed();
        break;
    }
    case LOP_CHAT: {
        LobbyChatLine line;
        if (lobby_read_chat(r, &line)) { line.time = now(); lobby_chat_push(&S->L, &line); log_chat(&line); }
        break;
    }
    case LOP_START:
        set_state(LE_START);
        if (S->L.state == LS_LOADING) { S->load_phase = 1; S->load_start = now(); S->load_frame = g_engine.frame_index; }
        break;
    case LOP_KICKED: {
        char why[128];
        r_str(r, why, sizeof(why));
        bool in_game = S->L.state == LS_LOADING || S->L.state == LS_IN_GAME;
        close_with(LE_KICKED, why[0] ? why : "You were kicked");
        if (in_game) go_menu(NULL);
        break;
    }
    case LOP_MIGRATE: {
        S->successor = r_u8(r);
        r_str(r, S->successor_addr, sizeof(S->successor_addr));
        if (S->L.state == LS_LOBBY || S->L.state == LS_COUNTDOWN) {
            set_state(LE_HOST_LOST);
            S->L.countdown = -1.0f;
            S->migrate_at = now() + (S->successor == S->L.local_id ? 0.2 : MIGRATE_RECONNECT_DELAY);
            const LobbyMember *m = lobby_member(&S->L, S->successor);
            lobby_chat_system(&S->L, now(), LC_SYSTEM, "The host left: %s is the new host", m ? m->name : "?");
        }
        break;
    }
    case LOP_ERROR: {
        char why[128];
        r_str(r, why, sizeof(why));
        local_line(LC_ERROR, "%s", why);
        break;
    }
    default: break;
    }
}

void lobby_on_welcome(void) {
    if (!lobby_active()) return;
    mainmenu_leave_scene();   // the menu background is gone (the replica world replaced it)
    if (S->L.state == LS_LOBBY || S->L.state == LS_JOINING) set_state(LE_START);   // WELCOME without a START (old host)
    S->load_phase = 2;
    if (!S->load_start) S->load_start = now();
}

bool lobby_on_client_lost(const char *reason, bool rejected) {
    if (!S || S->L.state == LS_IDLE) return false;
    if (S->L.state == LS_CLOSED) return true;   // kicked: the host dropping us afterwards
    switch (S->L.state) {
    case LS_JOINING:
        close_with(rejected ? LE_REJECTED : LE_TRANSPORT_FAIL, reason);
        return true;
    case LS_LOBBY:
    case LS_COUNTDOWN: {
        if (rejected) { close_with(LE_REJECTED, reason); return true; }
        // The host is gone without a word: the same successor on every machine.
        const LobbyMember *h = lobby_host_member(&S->L);
        u8 succ = lobby_successor(&S->L, h ? h->id : LOBBY_NONE);
        const LobbyMember *m = lobby_member(&S->L, succ);
        if (!m) { close_with(LE_HOST_LOST, "The host left"); return true; }
        S->successor = succ;
        dg_strcpy(S->successor_addr, sizeof(S->successor_addr), m->address);
        set_state(LE_HOST_LOST);
        S->L.countdown = -1.0f;
        S->migrate_at = now() + (succ == S->L.local_id ? 0.2 : MIGRATE_RECONNECT_DELAY);
        lobby_chat_system(&S->L, now(), LC_SYSTEM, "Lost the host: %s takes over", m->name);
        return true;
    }
    case LS_MIGRATING:
        // The old host's connection closing after LOP_MIGRATE, or the new host unreachable.
        if (S->migrate_done) close_with(rejected ? LE_REJECTED : LE_TRANSPORT_FAIL, rejected ? reason : "Host migration failed");
        return true;
    case LS_LOADING:
    case LS_IN_GAME:
        close_with(rejected ? LE_REJECTED : LE_HOST_LOST, rejected ? reason : "The host left the game");
        go_menu(NULL);
        return true;
    default: return false;
    }
}

int lobby_discovery_players(void) { return lobby_is_host() ? lobby_count(&S->L) : -1; }
bool lobby_discovery_hidden(void) { return lobby_is_host() && S->L.settings.privacy == LP_INVITE; }
int lobby_discovery_state(void) { return lobby_is_host() && (S->L.state == LS_LOADING || S->L.state == LS_IN_GAME) ? 1 : 0; }

// ------------------------------------------------------------------ loading

static float stream_progress(World *w) {
    Player *p = game_local_player();
    if (!w || !p) return 0.0f;
    if (!w->chunks) return 1.0f;
    if (chunks_is_loaded(w, p->sim.move.pos)) return 1.0f;
    return fminf(0.95f, (float)chunks_loaded_count(w) / 64.0f);
}

float lobby_load_progress(void) {
    if (!S || S->L.state != LS_LOADING) return S && S->L.state == LS_IN_GAME ? 1.0f : 0.0f;
    float t = (float)(now() - S->load_start);
    if (S->load_phase < 2) return fminf(0.35f, 0.05f + t * 0.1f);
    return 0.4f + 0.6f * stream_progress(game_world());
}

const char *lobby_load_tip(void) {
    static const char *const tips[] = {
        "Noise draws zombies: gunshots carry much farther than footsteps.",
        "Hold E on crates to search them. Drag items between the crate and your backpack.",
        "Rain and wind mask your footsteps; snow does not hide your tracks.",
        "Night falls fast. Lamps come on at dusk - and so do the hordes.",
        "Attachments change recoil and handling: try them at a weapon bench.",
        "Press Enter to chat with your team. /ready, /map and /kick work in the lobby.",
        "Autumn brings rain, winter brings snow: pick the season in the lobby.",
    };
    u32 i = S ? dg_hash_u32((u32)(S->load_start * 10.0)) % (u32)ARRAY_COUNT(tips) : 0;
    return tips[i];
}

static void host_load_map(void) {
    const MapInfo *mi = maplist_find(&S->maps, S->L.settings.map);
    const char *level = mi ? mi->level : S->L.settings.map;
    log_info("lobby: loading '%s' (%s)", S->L.settings.map, level);
    mainmenu_leave_scene();
    if (!game_host_ex(level, GH_KEEP_NET)) { net_stop(); close_with(LE_TRANSPORT_FAIL, "The map failed to load"); go_menu(NULL); return; }
    lobby_apply_environment(game_world(), &S->L.settings);
    if (mi && mi->scene[0]) scene_enter(mi->scene);   // a scene entry ("handpose"): its level, then the scene
    // Everyone in the lobby joins the game now.
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        LobbyMember *m = &S->L.members[i];
        if (!m->active || m->peer < 0) continue;
        if (m->peer < 64 && S->welcomed[m->peer]) continue;
        if (net_host_welcome(m->peer, m->name) && m->peer < 64) S->welcomed[m->peer] = true;
    }
    S->load_phase = 2;
    S->prof.matches++;
    dg_strcpy(S->prof.last_map, sizeof(S->prof.last_map), S->L.settings.map);
    profile_save(&S->prof);
}

static void update_loading(void) {
    if (S->L.state != LS_LOADING) return;
    // Host: load once the loading screen has been drawn (two frames later).
    if (S->L.is_host && S->load_phase == 1 && g_engine.frame_index > S->load_frame + 2) host_load_map();
    if (S->L.state != LS_LOADING || S->load_phase < 2) return;
    if (stream_progress(game_world()) >= 1.0f && now() - S->load_start > LOAD_MIN_SCREEN) {
        set_state(LE_LOAD_DONE);
        mainmenu_session_started();
    }
    if (!S->L.is_host && S->L.state == LS_LOADING && now() - S->state_time > 120.0) { net_stop(); close_with(LE_TRANSPORT_FAIL, "Loading timed out"); go_menu(NULL); }
}

// ------------------------------------------------------------------ migration

static void update_migration(void) {
    if (S->L.state != LS_MIGRATING) return;
    if (S->migrate_at > 0 && now() >= S->migrate_at && !S->migrate_done) {
        S->migrate_done = true;
        net_stop();
        if (S->successor == S->L.local_id) {
            // We host now: same settings, bans and chat; the others reconnect and rejoin.
            LobbySettings keep = S->L.settings;
            char bans[LOBBY_MAX_BANS][64];
            int nb = S->L.ban_count;
            memcpy(bans, S->L.bans, sizeof(bans));
            char ident[64], addr[64];
            host_identity(ident, sizeof(ident), addr, sizeof(addr));
            lobby_host_begin(&S->L, lobby_local_name(), local_steam_id(), ident, addr, &keep);
            memcpy(S->L.bans, bans, sizeof(bans));
            S->L.ban_count = nb;
            if (!start_hosting(S->kind)) { close_with(LE_TRANSPORT_FAIL, "Could not take over as the host"); return; }
            set_state(LE_MIGRATED);
            lobby_chat_system(&S->L, now(), LC_SYSTEM, "You are the host now");
        } else {
            S->L.is_host = false;
            if (!net_connect(S->kind, S->successor_addr)) close_with(LE_TRANSPORT_FAIL, "Could not reach the new host");
        }
    }
    if (now() - S->state_time > MIGRATE_TIMEOUT && S->L.state == LS_MIGRATING) { net_stop(); close_with(LE_TRANSPORT_FAIL, "Host migration timed out"); }
}

// ------------------------------------------------------------------ Steam mirror / presence

static void update_presence(void) {
    if (now() - S->presence_time < 1.0) return;
    S->presence_time = now();
    const MapInfo *mi = maplist_find(&S->maps, S->L.settings.map);
    char text[128];
    lobby_rich_presence(&S->L, mi ? mi->name : NULL, text, sizeof(text));
#ifdef DG_HAVE_STEAM
    if (!dgs_available()) return;
    if (strcmp(text, S->presence)) dgs_set_rich_presence("status", text);
    // Steam lobby metadata (the browser's filters) follows the host's lobby.
    u64 lob = dgs_current_lobby();
    if (lobby_is_host() && lob && (lob != S->meta_lobby || S->meta_rev != S->L.revision || S->meta_state != (int)S->L.state)) {
        LobbyKV kv[16];
        int n = lobby_metadata(&S->L, lobby_local_name(), S->version_hash, kv, 16);
        for (int i = 0; i < n; i++) dgs_set_lobby_data(kv[i].key, kv[i].value);
        static const int types[LP_COUNT] = { DGS_LOBBY_PUBLIC, DGS_LOBBY_FRIENDS, DGS_LOBBY_PRIVATE };
        dgs_set_lobby_type(types[DG_MIN(S->L.settings.privacy, LP_COUNT - 1)]);
        bool joinable = lobby_count(&S->L) < S->L.settings.max_players && (S->L.settings.late_join || (S->L.state != LS_LOADING && S->L.state != LS_IN_GAME));
        dgs_set_lobby_joinable(joinable);
        S->meta_lobby = lob;
        S->meta_rev = S->L.revision;
        S->meta_state = (int)S->L.state;
        S->L.steam_lobby = lob;
    }
#endif
    dg_strcpy(S->presence, sizeof(S->presence), text);
}

// ------------------------------------------------------------------ update

void lobby_net_update(float dt) {
    if (!S) return;
    double t = now();
    // Host: kicked peers dropped once their message went out; pings into the snapshot (1 Hz).
    for (int i = 0; i < S->drop_count;) {
        if (t >= S->drops[i].at) { net_drop_peer(S->drops[i].peer); S->drops[i] = S->drops[--S->drop_count]; }
        else i++;
    }
    if (lobby_is_host() && t - S->ping_time > 1.0) {
        S->ping_time = t;
        bool changed = false;
        for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
            LobbyMember *m = &S->L.members[i];
            if (!m->active || m->peer < 0) continue;
            float p = net_peer_ping(m->peer);
            u16 ms = (u16)(p < 0 ? 0 : DG_MIN(p, 9999.0f));
            if (abs((int)ms - (int)m->ping_ms) > 5) { m->ping_ms = ms; changed = true; }
        }
        if (changed && (S->L.state == LS_LOBBY || S->L.state == LS_COUNTDOWN)) broadcast_snapshot();
    }
    if (!S->L.is_host && (S->L.state == LS_LOBBY || S->L.state == LS_COUNTDOWN)) {
        LobbyMember *me = lobby_local(&S->L);
        if (me) { float p = net_peer_ping(0); if (p >= 0) me->ping_ms = (u16)p; }
    }
    // Forced-start countdown (host decides; clients only display theirs).
    if (S->L.is_host && lobby_tick(&S->L, dt)) begin_loading();
    else if (!S->L.is_host && S->L.state == LS_COUNTDOWN && S->L.countdown > 0) S->L.countdown = fmaxf(0.0f, S->L.countdown - dt);
    if (S->L.state == LS_JOINING && t - S->state_time > JOIN_TIMEOUT) { net_stop(); close_with(LE_TRANSPORT_FAIL, "Connection timed out"); }
    update_loading();
    update_migration();
    update_presence();
    if (S->pending_menu) {
        S->pending_menu = false;
        mainmenu_enter(S->pending_msg[0] ? S->pending_msg : NULL);
    }
}

// ------------------------------------------------------------------ browser / friends / avatar

u64 lobby_local_steam_id(void) { return local_steam_id(); }

bool lobby_avatar_rgba(u64 steam_id, int *w, int *h, u8 *out, int cap) {
#ifdef DG_HAVE_STEAM
    if (dgs_available() && steam_id) return dgs_avatar_rgba(steam_id, w, h, out, cap);
#endif
    (void)steam_id; (void)w; (void)h; (void)out; (void)cap;
    return false;
}

void lobby_browser_refresh(int source, const BrowserFilter *f) {
    if (source == BS_LAN) { net_lan_refresh(); return; }
#ifdef DG_HAVE_STEAM
    if (source == BS_STEAM && dgs_available()) dgs_request_lobby_list(f ? f->map : "", f ? f->region : 1, f && f->players == 1 ? 1 : 0);
#endif
    (void)f;
}

bool lobby_browser_busy(int source) {
#ifdef DG_HAVE_STEAM
    if (source == BS_STEAM) return dgs_lobby_list_status() == 1;
#endif
    (void)source;
    return false;
}

static bool entry_passes(const BrowserEntry *e, const BrowserFilter *f) {
    if (!f) return true;
    if (f->map[0] && strcmp(f->map, e->map)) return false;
    if (f->players == 1 && e->max_players > 0 && e->players >= e->max_players) return false;
    if (f->players == 2 && e->players <= 0) return false;
    if (f->friends_only && !e->friend_host) return false;
    if (!f->show_running && e->in_game) return false;
    return true;
}

int lobby_browser_entries(int source, const BrowserFilter *f, BrowserEntry *out, int max) {
    int n = 0;
    if (source == BS_LAN) {
        const LanServer *sv;
        int c = net_lan_servers(&sv);
        for (int i = 0; i < c && n < max; i++) {
            BrowserEntry e;
            memset(&e, 0, sizeof(e));
            dg_strcpy(e.name, sizeof(e.name), sv[i].name);
            dg_strcpy(e.map, sizeof(e.map), sv[i].map);
            dg_strcpy(e.address, sizeof(e.address), sv[i].address);
            e.kind = TRANSPORT_LAN;
            e.players = sv[i].players;
            e.max_players = sv[i].max_players;
            e.in_game = sv[i].in_game;
            e.version_ok = !sv[i].version || sv[i].version == S->version_hash;
            e.ping_ms = sv[i].ping_ms;
            e.season = -1;
            if (entry_passes(&e, f)) out[n++] = e;
        }
        return n;
    }
#ifdef DG_HAVE_STEAM
    if (!dgs_available()) return 0;
    if (source == BS_STEAM) {
        static DgsLobbyInfo li[64];
        int c = dgs_lobby_list(li, 64);
        char ver[16];
        snprintf(ver, sizeof(ver), "%08x", S->version_hash);
        for (int i = 0; i < c && n < max; i++) {
            BrowserEntry e;
            memset(&e, 0, sizeof(e));
            dg_strcpy(e.name, sizeof(e.name), li[i].name[0] ? li[i].name : "Deadground lobby");
            dg_strcpy(e.map, sizeof(e.map), li[i].map);
            snprintf(e.address, sizeof(e.address), "lobby:%llu", (unsigned long long)li[i].id);
            e.kind = TRANSPORT_STEAM;
            e.players = li[i].players ? li[i].players : li[i].members;
            e.max_players = li[i].max_players;
            e.in_game = !strcmp(li[i].state, "in_game");
            e.friend_host = li[i].friend_owner;
            e.version_ok = !strcmp(li[i].version, ver);
            e.season = li[i].season;
            e.ping_ms = -1.0f;
            if (entry_passes(&e, f)) out[n++] = e;
        }
    } else if (source == BS_FRIENDS) {
        static DgsFriend fr[256];
        int c = dgs_friends(fr, 256);
        for (int i = 0; i < c && n < max; i++) {
            if (!fr[i].in_this_game || !fr[i].lobby) continue;
            BrowserEntry e;
            memset(&e, 0, sizeof(e));
            snprintf(e.name, sizeof(e.name), "%s's game", fr[i].name);
            snprintf(e.address, sizeof(e.address), "lobby:%llu", (unsigned long long)fr[i].lobby);
            e.kind = TRANSPORT_STEAM;
            e.friend_host = true;
            e.version_ok = true;   // checked at join
            e.ping_ms = -1.0f;
            e.season = -1;
            out[n++] = e;
        }
    }
#endif
    return n;
}

int lobby_friends(FriendEntry *out, int max) {
    int n = 0;
#ifdef DG_HAVE_STEAM
    if (!dgs_available()) return 0;
    static DgsFriend fr[256];
    int c = dgs_friends(fr, 256);
    // Online (and in this game) first.
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < c && n < max; i++) {
            int rank = fr[i].in_this_game ? 0 : (fr[i].state ? 1 : 2);
            if (rank != pass) continue;
            FriendEntry *e = &out[n++];
            e->id = fr[i].id;
            dg_strcpy(e->name, sizeof(e->name), fr[i].name);
            e->state = fr[i].state;
            e->in_game = fr[i].in_this_game;
            e->lobby = fr[i].lobby;
        }
#endif
    (void)out; (void)max;
    return n;
}

bool lobby_invite_friend(u64 steam_id) {
#ifdef DG_HAVE_STEAM
    if (dgs_available()) return dgs_invite_friend(steam_id);
#endif
    (void)steam_id;
    return false;
}

// ------------------------------------------------------------------ commands

static void cmd_lobby_host(int argc, const char **argv) { lobby_host(argc > 1 && !strcmp(argv[1], "steam") ? TRANSPORT_STEAM : TRANSPORT_LAN); }
static void cmd_lobby_join(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: lobby_join <ip[:port] | lobby:<steam lobby id> | <steamid64>>"); return; }
    bool steam = !strncmp(argv[1], "lobby:", 6) || (strlen(argv[1]) >= 15 && !strchr(argv[1], '.') && !strchr(argv[1], ':'));
    lobby_join(steam ? TRANSPORT_STEAM : TRANSPORT_LAN, argv[1]);
}
static void cmd_lobby_leave(int argc, const char **argv) { (void)argc; (void)argv; lobby_leave("lobby_leave"); }
static void cmd_lobby_ack(int argc, const char **argv) { (void)argc; (void)argv; lobby_ack_closed(); }
static void cmd_lobby_ready(int argc, const char **argv) {
    const LobbyMember *me = lobby_local(&S->L);
    lobby_set_ready(argc > 1 ? atoi(argv[1]) != 0 : !(me && me->ready));
}
static void cmd_lobby_say(int argc, const char **argv) {
    char text[LOBBY_CHAT_MAX + 1] = "";
    for (int i = 1; i < argc; i++) { if (i > 1) strncat(text, " ", sizeof(text) - strlen(text) - 1); strncat(text, argv[i], sizeof(text) - strlen(text) - 1); }
    lobby_chat(text);
}
static void cmd_lobby_start(int argc, const char **argv) { lobby_start(argc > 1 && !strcmp(argv[1], "force")); }
static void cmd_lobby_kick(int argc, const char **argv) {
    LobbyMember *m = argc > 1 ? lobby_member_by_name(&S->L, argv[1]) : NULL;
    if (!m) { log_info("lobby_kick: no such player"); return; }
    lobby_kick(m->id);
}
static void cmd_lobby_season(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: lobby_season <spring|summer|autumn|winter> [hour] [start weather|random] [static]"); return; }
    LobbySettings s = S->L.settings;
    int season = season_from_id(argv[1]);
    if (season < 0) { log_info("lobby_season: unknown season"); return; }
    s.season = (u8)season;
    if (argc > 2) s.start_hour = (u8)DG_CLAMP(atoi(argv[2]), 0, 23);
    s.weather[0] = 0;
    if (argc > 3 && strcmp(argv[3], "random")) dg_strcpy(s.weather, sizeof(s.weather), argv[3]);
    s.dynamic_weather = !(argc > 4 && !strcmp(argv[4], "static"));
    lobby_set_settings(&s);
}
// cos_equip <item id> | cos_equip none <slot> | cos_equip: list (scripted tests; the Cosmetics tab).
static void cmd_cos_equip(int argc, const char **argv) {
    if (argc < 2) {
        for (int s = 0; s < COS_SLOTS; s++) if (S->prof.loadout.equipped[s][0]) log_info("  %-12s %s", cosmetics_slot_name(s), S->prof.loadout.equipped[s]);
        Cvar *cp = cvar_find("cl_parts"), *cs = cvar_find("cl_skin");
        log_info("cos_equip: cl_skin '%s' cl_parts '%s'", cs ? cs->str : "", cp ? cp->str : "");
        return;
    }
    if (!strcmp(argv[1], "none")) {
        int slot = argc > 2 ? char_slot_from_name(argv[2]) : -1;
        if (argc > 2 && !strcmp(argv[2], "weapon_skin")) slot = COS_SLOT_WEAPON;
        if (slot < 0) { log_info("cos_equip none <slot>"); return; }
        cosmetics_unequip(&S->prof.loadout, slot);
    } else if (!cosmetics_equip(&S->cos, &S->prof.loadout, &S->prof.unlocks, argv[1])) {
        log_info("cos_equip: '%s' unknown or not owned", argv[1]);
        return;
    }
    lobby_profile_changed();
    Cvar *cp = cvar_find("cl_parts"), *cs = cvar_find("cl_skin");
    log_info("cos_equip: cl_skin '%s' cl_parts '%s'", cs ? cs->str : "", cp ? cp->str : "");
}

// lobby_wait players <n> | ready <n> | state <name> | host [timeout frames]: scripted tests hold the
// script until the lobby gets there (two processes start at different speeds), at most the timeout.
static void cmd_lobby_wait(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: lobby_wait players <n> | ready <n> | state <name> | host [timeout frames, default 1800]"); return; }
    bool host = !strcmp(argv[1], "host");
    int ai = host ? 2 : 3;
    const char *want = host ? "" : argc > 2 ? argv[2] : "";
    int left = argc > ai ? atoi(argv[ai]) : 1800;
    const Lobby *L = &S->L;
    int ready = 0;
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) if (L->members[i].active && L->members[i].ready) ready++;
    bool done = host ? L->is_host && L->state == LS_LOBBY
              : !strcmp(argv[1], "players") ? lobby_count(L) >= atoi(want)
              : !strcmp(argv[1], "ready") ? ready >= atoi(want)
              : !strcmp(argv[1], "state") ? !strcmp(lobby_state_name(L->state), want)
              : true;
    if (done) { log_info("lobby_wait %s %s: done", argv[1], want); return; }
    if (left <= 0) { log_warn("lobby_wait %s %s: timed out (state %s, %d players, %d ready)", argv[1], want, lobby_state_name(L->state), lobby_count(L), ready); return; }
    char line[128];
    if (host) snprintf(line, sizeof(line), "lobby_wait host %d", left - 2);
    else snprintf(line, sizeof(line), "lobby_wait %s %.32s %d", argv[1], want, left - 2);
    cbuf_retry(line);
}

static void cmd_lobby_status(int argc, const char **argv) {
    (void)argc; (void)argv;
    const Lobby *L = &S->L;
    const SeasonTable *st = weather_seasons();
    log_info("lobby: %s, %s, %d/%d players, map %s, %s %02d:00, weather %s (%s), privacy %s, version %08x", lobby_state_name(L->state), L->is_host ? "host" : "member",
             lobby_count(L), L->settings.max_players, L->settings.map, L->settings.season < SEASON_COUNT ? st->s[L->settings.season].name : "?", L->settings.start_hour,
             L->settings.weather[0] ? L->settings.weather : "season pick", L->settings.dynamic_weather ? "dynamic" : "fixed", lobby_privacy_name(L->settings.privacy), S->version_hash);
    for (int i = 0; i < LOBBY_MAX_MEMBERS; i++) {
        const LobbyMember *m = &L->members[i];
        if (m->active) log_info("  #%d %-20s %s%s ping %d ms  %s", m->id, m->name, m->host ? "host " : "", m->ready ? "ready" : "-", m->ping_ms, m->ident);
    }
    if (L->state == LS_CLOSED) log_info("  closed: %s", L->close_reason);
}

void lobby_net_init(void) {
    S = (LobbySys *)calloc(1, sizeof(LobbySys));
    lobby_init(&S->L);
    S->privacy = cvar_reg_int("lobby_privacy", LP_FRIENDS, 0, LP_COUNT - 1, CVAR_EDITABLE, "Steam lobby type for the next hosted lobby (0 public, 1 friends, 2 invite only; set by the lobby)");
    maplist_load(&S->maps, "assets/maps");
    cosmetics_load(&S->cos, "assets/cosmetics");
    if (!profile_load(&S->prof)) log_info("profile: none yet (%s)", profile_path());
    if (!maplist_find(&S->maps, S->prof.lobby.map) && S->maps.count) dg_strcpy(S->prof.lobby.map, sizeof(S->prof.lobby.map), S->maps.maps[0].id);
    // The consistency set (assets/render/consistency.json) is part of the content identity.
    S->version_hash = lobby_version_hash(DG_VERSION, DG_PROTOCOL_VERSION, maplist_hash(&S->maps) ^ cosmetics_hash(&S->cos) ^ consistency_hash());
    log_info("lobby: version %s, build hash %08x", DG_VERSION, S->version_hash);
    cmd_register("lobby_host", cmd_lobby_host, "lobby_host [lan|steam]: open a lobby (main menu Host game)");
    cmd_register("lobby_join", cmd_lobby_join, "lobby_join <ip[:port] | lobby:<id> | steamid64>: join a lobby");
    cmd_register("lobby_leave", cmd_lobby_leave, "leave the lobby / game (back to the main menu)");
    cmd_register("lobby_ready", cmd_lobby_ready, "lobby_ready [0|1]: toggle / set ready");
    cmd_register("lobby_ack", cmd_lobby_ack, "dismiss the 'session ended' message");
    cmd_register("lobby_say", cmd_lobby_say, "lobby_say <text>: lobby chat (slash commands work)");
    cmd_register("lobby_start", cmd_lobby_start, "lobby_start [force]: start (host; force = 10 s countdown)");
    cmd_register("lobby_kick", cmd_lobby_kick, "lobby_kick <name>: kick (host)");
    cmd_register("lobby_season", cmd_lobby_season, "lobby_season <season> [hour] [start weather|random] [static]: the lobby's season / time / weather (host)");
    cmd_register("lobby_status", cmd_lobby_status, "print the lobby");
    cmd_register("lobby_wait", cmd_lobby_wait, "lobby_wait players <n> | ready <n> | state <name> | host [timeout frames]: hold a script until the lobby gets there");
    cmd_register("cos_equip", cmd_cos_equip, "cos_equip <item id> | none <slot>: equip a cosmetic (no args: the loadout)");
}

void lobby_net_shutdown(void) {
    if (!S) return;
    lobby_leave("shutdown");
    profile_save(&S->prof);
    free(S);
    S = NULL;
}
