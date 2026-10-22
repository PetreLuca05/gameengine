// Minimal extern "C" API over the Steamworks SDK (C++), used by src/net/transport_steam.c.
// Compiled only when STEAM_SDK_DIR is configured (DG_HAVE_STEAM).
#ifndef DG_STEAM_SHIM_H
#define DG_STEAM_SHIM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DGS_EV_CONNECTED = 1,      // conn
    DGS_EV_DISCONNECTED,       // conn
    DGS_EV_DATA,               // conn, data, size
    DGS_EV_LOBBY_CREATED,      // id = lobby
    DGS_EV_LOBBY_ENTERED,      // id = lobby, owner = host steam id
    DGS_EV_JOIN_REQUESTED,     // id = lobby the user asked to join (friends list / invite)
};

typedef struct DgsEvent {
    int type;
    uint32_t conn;
    uint64_t id;
    uint64_t owner;
    const void *data;
    uint32_t size;
} DgsEvent;

bool dgs_init(void);
void dgs_shutdown(void);
void dgs_run_callbacks(void);
bool dgs_available(void);
uint64_t dgs_my_id(void);
const char *dgs_persona_name(void);

bool dgs_listen(void);
bool dgs_connect(uint64_t steam_id);
bool dgs_send(uint32_t conn, const void *data, uint32_t size, bool reliable);
void dgs_close(uint32_t conn);
int dgs_poll(DgsEvent *out, int max);   // data pointers valid until the next dgs_poll
bool dgs_stats(uint32_t conn, float *ping_ms, float *loss_pct);

bool dgs_create_lobby(int max_members);
bool dgs_join_lobby(uint64_t lobby);
void dgs_leave_lobby(void);
uint64_t dgs_current_lobby(void);
void dgs_invite_overlay(void);
void dgs_set_rich_presence(const char *key, const char *value);

// Lobby (src/net/lobby_net.c, docs/MENU_LOBBY.md)
enum { DGS_LOBBY_PRIVATE = 0, DGS_LOBBY_FRIENDS = 1, DGS_LOBBY_PUBLIC = 2 };   // = ELobbyType
bool dgs_create_lobby_typed(int max_members, int type);
void dgs_set_lobby_data(const char *key, const char *value);
void dgs_set_lobby_type(int type);
void dgs_set_lobby_joinable(bool joinable);
uint64_t dgs_lobby_owner(void);
bool dgs_set_lobby_owner(uint64_t steam_id);
bool dgs_conn_steam_id(uint32_t conn, uint64_t *out);   // the remote end of a connection
float dgs_relay_ping_ms(void);                          // nearest relay (POP), < 0 unknown yet

// Lobby browser: request, then poll the status (0 idle, 1 pending, 2 ready) and read the results.
typedef struct DgsLobbyInfo {
    uint64_t id, owner;
    char name[64], map[32], version[16], state[16];
    int players, max_players, members, privacy, season;
    bool friend_owner;          // the owner is on our friends list
} DgsLobbyInfo;
// distance: 0 close, 1 default, 2 far, 3 worldwide (ELobbyDistanceFilter). map: "" = any.
void dgs_request_lobby_list(const char *map, int distance, int min_free_slots);
int dgs_lobby_list_status(void);
int dgs_lobby_list(DgsLobbyInfo *out, int max);

// Friends
typedef struct DgsFriend {
    uint64_t id;
    char name[64];
    int state;                  // 0 offline, 1 online, 2 busy / away / snooze
    bool in_this_game;
    uint64_t lobby;             // the friend's lobby in this game (0 none)
} DgsFriend;
int dgs_friends(DgsFriend *out, int max);
bool dgs_invite_friend(uint64_t steam_id);
// Small avatar as RGBA8 (32 x 32). false while Steam still downloads it.
bool dgs_avatar_rgba(uint64_t steam_id, int *w, int *h, uint8_t *out, int cap);

// Steam Stats (docs/STATS.md "Steam"): an INT stat by its API name, then one store per batch.
bool dgs_stat_set_int(const char *name, int32_t value);
bool dgs_stats_store(void);

#ifdef __cplusplus
}
#endif
#endif
