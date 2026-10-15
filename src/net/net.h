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
} LanServer;

#define MAX_LAN_SERVERS 16

bool net_init(void);
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
void net_send_rpc(int type, int a, int b, int c);
NetStats net_stats(void);

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
