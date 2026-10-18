// Networking session layer (host-authoritative P2P). Sits on top of net_transport.h and
// implements the protocol: handshake, user commands, snapshots with delta compression,
// reliable events and RPCs, LAN discovery, net stats and fake lag/loss.
#ifndef DG_NET_H
#define DG_NET_H

#include "core/base.h"
#include "game/usercmd.h"

struct World;

typedef enum TransportKind { TRANSPORT_LAN, TRANSPORT_STEAM } TransportKind;
typedef enum NetRole { NET_OFF, NET_HOST, NET_CLIENT } NetRole;

typedef struct NetStats {
    float ping_ms;       // transport RTT
    float app_rtt_ms;    // client: command -> acked snapshot round trip
    float loss_pct;
    float in_kbps, out_kbps;
    float snapshot_bytes;
    int peers;
    int snapshot_rate;
    float interp_ms;
    float prediction_error;
    const char *transport;
} NetStats;

typedef struct LanServer {
    char name[48];
    char map[32];
    char address[64];
    int players, max_players;
    float ping_ms;       // transport RTT
    float app_rtt_ms;    // client: command -> acked snapshot round trip
    double last_seen;
    bool in_game;        // the game is running (else: in the lobby)
    u32 version;         // the host's lobby version hash (0: an old host)
} LanServer;

#define MAX_LAN_SERVERS 16

bool net_init(void);
// Join handshake (docs/SETTINGS_AUDIT.md): a client whose consistency hash differs from the host's
// (or sends none) is rejected; why gets the MSG_REJECT reason. Pure.
bool net_consistency_ok(u32 host_hash, u32 client_hash, bool missing, char *why, int why_size);
void net_shutdown(void);
bool net_host(TransportKind kind, u16 port);
bool net_connect(TransportKind kind, const char *address);
void net_stop(void);
NetRole net_role(void);
bool net_steam_available(void);
const char *net_transport_name(void);

void net_update(float dt);                       // poll transports, dispatch messages (every frame)
void net_host_after_tick(struct World *w);       // host: send snapshots at sv_snaprate
void net_host_send_events(struct World *w);      // host: deliver queued gameplay events to remote players
void net_client_send_cmds(const UserCmd *history, int history_size, u32 latest_seq);
void net_client_interpolate(struct World *w, float dt);
u32 net_client_view_tick256(void);
double net_client_render_tick(void);    // fractional server tick being rendered
int net_host_client_count(void);
void net_host_send_anim_events(struct World *w);
void net_send_rpc(int type, int a, int b, int c);
u16 net_player_owd_ms(int slot);         // host: one-way delay estimate of a remote player (RTT / 2), 0 = local / none
NetStats net_stats(void);

// Lobby (src/net/lobby_net.c): raw reliable sends (host: to a peer; client: to the host; the data
// starts with MSG_LOBBY), welcome a lobby member into the started game, drop / identify peers.
bool net_send_reliable(int peer, const void *data, u32 size);
void net_flush(void);
bool net_host_welcome(int peer, const char *name);
void net_drop_peer(int peer);
bool net_peer_address(int peer, char *out, int size);   // client: the host's
float net_peer_ping(int peer);                          // ms, < 0 unknown (client: the host's)
TransportKind net_kind(void);
bool net_client_welcomed(void);
int net_listen_port(void);

// LAN browser
void net_lan_refresh(void);
int net_lan_servers(const LanServer **out);

// Steam (no-ops when built without the SDK)
void net_steam_invite(void);
bool net_steam_in_lobby(void);
void net_steam_register_commands(void);
void net_steam_poll_idle(void);
void net_steam_shutdown(void);

#endif
