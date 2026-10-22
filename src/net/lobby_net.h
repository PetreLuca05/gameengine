// Lobby glue (docs/MENU_LOBBY.md): runs the pure lobby (src/net/lobby.h) over the session layer
// (net.c, LAN or Steam through the same transport abstraction), mirrors it into the Steam lobby
// (metadata, owner, rich presence) and the LAN discovery reply, owns the main menu's data (maps,
// cosmetics, profile) and drives the game through the lobby's states: map load on start, loading
// progress, return to the main menu, host migration in the lobby.
#ifndef DG_LOBBY_NET_H
#define DG_LOBBY_NET_H

#include "core/base.h"
#include "net/lobby.h"
#include "net/net.h"
#include "net/bitbuf.h"
#include "world/maplist.h"
#include "gameplay/cosmetics.h"
#include "game/profile.h"

void lobby_net_init(void);
void lobby_net_shutdown(void);          // leaves the lobby, saves the profile
void lobby_net_update(float dt);        // every frame (after net_update)

const Lobby *lobby_get(void);
LobbyState lobby_state(void);
bool lobby_is_host(void);
bool lobby_active(void);                // a session exists (CREATING .. MIGRATING)

// Session (the main menu / browser / invites call these)
bool lobby_host(TransportKind kind);    // the profile's last settings, the menu's selected map
bool lobby_join(TransportKind kind, const char *address);   // "ip[:port]", a SteamID64 or "lobby:<id>"
void lobby_leave(const char *why);      // back to IDLE (the main menu)
void lobby_ack_closed(void);            // dismiss the CLOSED message
const char *lobby_closed_reason(void);
TransportKind lobby_transport(void);

// Intents (host: applied locally; client: sent to the host). Errors arrive as chat lines.
void lobby_chat(const char *text);      // slash commands included
void lobby_set_ready(bool ready);
void lobby_set_map(const char *map);
void lobby_set_settings(const LobbySettings *s);
void lobby_apply_rules(const LobbySettings *s);   // host: the session's server rules (foliage density ...)
void lobby_kick(u8 member);
void lobby_promote(u8 member);
void lobby_start(bool force);
bool lobby_set_story(const char *chapter, const char *slot);   // host: the party plays this chapter (slot: the save to continue, NULL = new)
void lobby_cancel_start(void);
void lobby_invite_overlay(void);        // Steam overlay invite dialog

// The lobby settings' look on a world (host): the season's calendar day, the start hour, the season's
// weather (start preset or a pick; dynamic or fixed). The game start and the menu background use it.
struct World;
void lobby_apply_environment(struct World *w, const LobbySettings *s);

// Loading screen
float lobby_load_progress(void);        // 0..1 (map load, then chunk streaming around the spawn)
const char *lobby_load_tip(void);

// Data
const MapList *lobby_maps(void);
const MapInfo *lobby_map_info(const char *id);
const CosmeticDB *lobby_cosmetics(void);
Profile *lobby_profile(void);
void lobby_profile_changed(void);       // cosmetics / settings edited: re-apply to the character, save
const char *lobby_local_name(void);     // Steam persona or the LAN name
u32 lobby_version_hash_local(void);

// Steam helpers for the menu (no-ops / false without Steam)
bool lobby_steam_running(void);
float lobby_relay_ping_ms(void);        // < 0 unknown
u64 lobby_local_steam_id(void);
bool lobby_avatar_rgba(u64 steam_id, int *w, int *h, u8 *out, int cap);

// Server browser (Play > Join): Steam lobbies, friends' games, LAN games.
typedef enum BrowserSource { BS_STEAM, BS_FRIENDS, BS_LAN, BS_DIRECT, BS_COUNT } BrowserSource;
typedef struct BrowserFilter {
    char map[32];             // "" = any
    int players;              // 0 any, 1 not full, 2 not empty
    int region;               // Steam distance: 0 close, 1 default, 2 far, 3 worldwide
    bool friends_only;
    bool show_running;        // games already started (late join)
} BrowserFilter;
typedef struct BrowserEntry {
    char name[64], map[32];
    char address[64];         // what lobby_join takes
    TransportKind kind;
    int players, max_players, season;
    bool in_game, friend_host, version_ok;
    float ping_ms;            // < 0 unknown
} BrowserEntry;
void lobby_browser_refresh(int source, const BrowserFilter *f);
bool lobby_browser_busy(int source);
int lobby_browser_entries(int source, const BrowserFilter *f, BrowserEntry *out, int max);
typedef struct FriendEntry { u64 id; char name[64]; int state; bool in_game; u64 lobby; } FriendEntry;
int lobby_friends(FriendEntry *out, int max);   // online first
bool lobby_invite_friend(u64 steam_id);

// Called by net.c ----------------------------------------------------------------
typedef enum LobbyJoin { LJ_WELCOME, LJ_LOBBY, LJ_REJECT } LobbyJoin;
typedef struct LobbyHello {
    char name[32];
    u32 build_hash;
    char build_version[24];
    u64 steam_id;
    u16 listen_port;
} LobbyHello;
void lobby_write_hello_ext(Writer *w);  // client: appended to MSG_HELLO
void lobby_read_hello_ext(Reader *r, LobbyHello *h);
int lobby_on_hello(int peer, const LobbyHello *h, char *reason, int reason_size);   // LobbyJoin
void lobby_on_peer_left(int peer);
void lobby_on_host_msg(int peer, Reader *r);
void lobby_on_client_msg(Reader *r);
void lobby_on_welcome(void);            // client: WELCOME arrived (the map loads now)
// Client: the connection ended (lost, rejected, timed out). true = the lobby handled it (no fallback
// to the local game).
bool lobby_on_client_lost(const char *reason, bool rejected);
int lobby_discovery_players(void);      // LAN discovery reply: lobby members (or -1: count the world)
bool lobby_discovery_hidden(void);      // invite-only lobbies don't answer LAN discovery
int lobby_discovery_state(void);        // 0 lobby, 1 in game

#endif
