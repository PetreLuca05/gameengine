// Steam transport: Steam Networking Sockets P2P (relay) + friends-only lobbies.
// Compiled to stubs when the Steamworks SDK is not configured (LAN-only build).
#include "net_transport.h"
#include "net.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef DG_HAVE_STEAM
#include "steam_shim.h"

#define STEAM_MAX_PEERS 16

typedef struct SteamImpl {
    u32 conns[STEAM_MAX_PEERS];  // index = PeerId
    bool is_server;
    bool waiting_lobby;
    DgsEvent raw[256];
} SteamImpl;

static int peer_for(SteamImpl *s, u32 conn, bool create) {
    for (int i = 0; i < STEAM_MAX_PEERS; i++) if (s->conns[i] == conn) return i;
    if (!create) return -1;
    for (int i = 0; i < STEAM_MAX_PEERS; i++) if (!s->conns[i]) { s->conns[i] = conn; return i; }
    return -1;
}

static bool st_listen(NetTransport *t, u16 port, int max_peers) {
    SteamImpl *s = (SteamImpl *)t->impl;
    (void)port;
    if (!dgs_listen()) { log_error("steam: CreateListenSocketP2P failed"); return false; }
    dgs_create_lobby(max_peers);
    s->is_server = true;
    log_info("steam: listening (P2P) as %llu, creating friends-only lobby", (unsigned long long)dgs_my_id());
    return true;
}

// address: "lobby:<id>" joins a lobby and connects to its owner; otherwise a raw SteamID64.
static bool st_connect(NetTransport *t, const char *address) {
    SteamImpl *s = (SteamImpl *)t->impl;
    if (!strncmp(address, "lobby:", 6)) {
        s->waiting_lobby = true;
        return dgs_join_lobby(strtoull(address + 6, NULL, 10));
    }
    return dgs_connect(strtoull(address, NULL, 10));
}

static bool st_send(NetTransport *t, PeerId peer, int channel, const void *data, u32 size) {
    SteamImpl *s = (SteamImpl *)t->impl;
    if (peer < 0 || peer >= STEAM_MAX_PEERS || !s->conns[peer]) return false;
    return dgs_send(s->conns[peer], data, size, channel == CH_RELIABLE);
}

static int st_poll(NetTransport *t, NetEvent *out, int max) {
    SteamImpl *s = (SteamImpl *)t->impl;
    int n_raw = dgs_poll(s->raw, 256);
    int n = 0;
    for (int i = 0; i < n_raw && n < max; i++) {
        DgsEvent *r = &s->raw[i];
        NetEvent *e = &out[n];
        memset(e, 0, sizeof(*e));
        switch (r->type) {
        case DGS_EV_CONNECTED:
            e->type = NETEV_CONNECT;
            e->peer = peer_for(s, r->conn, true);
            if (e->peer >= 0) n++;
            break;
        case DGS_EV_DISCONNECTED:
            e->type = NETEV_DISCONNECT;
            e->peer = peer_for(s, r->conn, false);
            if (e->peer >= 0) { s->conns[e->peer] = 0; n++; }
            break;
        case DGS_EV_DATA:
            e->type = NETEV_DATA;
            e->peer = peer_for(s, r->conn, false);
            e->channel = CH_RELIABLE;
            e->data = (const u8 *)r->data;
            e->size = r->size;
            if (e->peer >= 0) n++;
            break;
        case DGS_EV_LOBBY_ENTERED:
            if (s->waiting_lobby && r->owner && r->owner != dgs_my_id()) {
                s->waiting_lobby = false;
                log_info("steam: entered lobby %llu, connecting to host %llu", (unsigned long long)r->id, (unsigned long long)r->owner);
                dgs_connect(r->owner);
            }
            break;
        case DGS_EV_LOBBY_CREATED:
            log_info("steam: lobby %llu created; friends can join via the Steam overlay", (unsigned long long)r->id);
            break;
        case DGS_EV_JOIN_REQUESTED: {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "connect_lobby %llu", (unsigned long long)r->id);
            cbuf_add(cmd);
            break;
        }
        default: break;
        }
    }
    return n;
}

static void st_disconnect(NetTransport *t, PeerId peer) {
    SteamImpl *s = (SteamImpl *)t->impl;
    if (peer < 0 || peer >= STEAM_MAX_PEERS || !s->conns[peer]) return;
    dgs_close(s->conns[peer]);
    s->conns[peer] = 0;
}

static void st_flush(NetTransport *t) { (void)t; }

static bool st_stats(NetTransport *t, PeerId peer, float *rtt, float *loss) {
    SteamImpl *s = (SteamImpl *)t->impl;
    if (peer < 0 || peer >= STEAM_MAX_PEERS || !s->conns[peer]) return false;
    return dgs_stats(s->conns[peer], rtt, loss);
}

static void st_destroy(NetTransport *t) {
    SteamImpl *s = (SteamImpl *)t->impl;
    for (int i = 0; i < STEAM_MAX_PEERS; i++) if (s->conns[i]) dgs_close(s->conns[i]);
    dgs_leave_lobby();
    free(s);
    free(t);
}

NetTransport *transport_steam_create(void) {
    if (!dgs_available() && !dgs_init()) return NULL;
    NetTransport *t = (NetTransport *)calloc(1, sizeof(NetTransport));
    t->impl = calloc(1, sizeof(SteamImpl));
    t->name = "Steam P2P";
    t->listen = st_listen;
    t->connect = st_connect;
    t->send = st_send;
    t->poll = st_poll;
    t->disconnect = st_disconnect;
    t->flush = st_flush;
    t->peer_stats = st_stats;
    t->destroy = st_destroy;
    return t;
}

bool net_steam_available(void) { return dgs_available() || dgs_init(); }
void net_steam_invite(void) { dgs_invite_overlay(); }
bool net_steam_in_lobby(void) { return dgs_current_lobby() != 0; }

static void cmd_connect_lobby(int argc, const char **argv) {
    if (argc < 2) return;
    char addr[64];
    snprintf(addr, sizeof(addr), "lobby:%s", argv[1]);
    net_connect(TRANSPORT_STEAM, addr);
}

static void cmd_connect_steam(int argc, const char **argv) {
    if (argc > 1) net_connect(TRANSPORT_STEAM, argv[1]);
}

void net_steam_register_commands(void) {
    cmd_register("connect_lobby", cmd_connect_lobby, "join a Steam lobby by id (used by invites / rich presence)");
    cmd_register("connect_steam", cmd_connect_steam, "connect to a host by SteamID64");
    dgs_init();
}

void net_steam_poll_idle(void) {
    // When no Steam session is active, still pump callbacks so friend invites work.
    if (!dgs_available() || net_role() != NET_OFF) return;
    DgsEvent ev[16];
    int n = dgs_poll(ev, 16);
    for (int i = 0; i < n; i++) {
        if (ev[i].type == DGS_EV_JOIN_REQUESTED) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "connect_lobby %llu", (unsigned long long)ev[i].id);
            cbuf_add(cmd);
        }
    }
}

void net_steam_shutdown(void) { dgs_shutdown(); }

#else  // ---------------------------------------------------------------- LAN-only build

NetTransport *transport_steam_create(void) {
    log_warn("steam: this build has no Steam support (configure with STEAM_SDK_DIR)");
    return NULL;
}
bool net_steam_available(void) { return false; }
void net_steam_invite(void) { log_info("steam: not available in this build"); }
bool net_steam_in_lobby(void) { return false; }
void net_steam_register_commands(void) {}
void net_steam_poll_idle(void) {}
void net_steam_shutdown(void) {}

#endif
