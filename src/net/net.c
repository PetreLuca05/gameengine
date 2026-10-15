// Session layer: host-authoritative peer-to-peer over a NetTransport (LAN or Steam).
#include "net.h"
#include "net_transport.h"
#include "protocol.h"
#include "bitbuf.h"
#include "replication.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "world/chunks.h"
#include "gameplay/entities.h"
#include <enet/enet.h>
#include <stdlib.h>
#include <stdio.h>

#define NET_BUF 131072

typedef struct RemoteClient {
    bool active;
    bool welcomed;
    PeerId peer;
    int slot;
    SnapHistory history;
    u32 acked_tick;
    u32 cvar_revision;
    double connect_time;
    float view_tick;
} RemoteClient;

typedef struct Discovery {
    ENetSocket sock;
    bool open;
    bool browsing;
    double browse_until;
    LanServer servers[MAX_LAN_SERVERS];
    int server_count;
    double query_time;
} Discovery;

typedef struct NetSystem {
    NetRole role;
    TransportKind kind;
    NetTransport *t;
    RemoteClient clients[MAX_PLAYERS];

    // client
    PeerId server_peer;
    bool connected, welcomed;
    double connect_started;
    int local_slot;
    SnapHistory cl_history;
    u32 latest_snap;
    InterpBuffer *interp;
    double server_time_offset;  // server_time - local_time
    bool time_init;
    double render_tick;

    // stats
    double cmd_send_time[128];   // client: send time per command seq (app-level RTT)
    float app_rtt_ms;
    u32 snaps_received, snaps_expected;
    float snap_loss_pct, last_snap_bytes;
    u64 bytes_in, bytes_out, win_in, win_out;
    double stats_time;
    float in_kbps, out_kbps, snap_bytes;
    u32 cvar_mod_sum;

    Discovery disc;
    Discovery browse;
    u8 buf[NET_BUF];
    Cvar *port, *snaprate, *cl_interp, *maxplayers, *showstats, *relevance;
} NetSystem;

#define N (g_engine.net)

static u32 server_cvar_revision(void) {
    u32 sum = 0;
    for (int i = 0; i < cvar_count(); i++) {
        Cvar *c = cvar_at(i);
        if (c->flags & CVAR_SERVER) sum += c->modified + (u32)i;
    }
    return sum;
}

// ------------------------------------------------------------------ helpers

static bool send_msg(PeerId peer, int channel, const Writer *w) {
    if (!N->t || w->overflow) { if (w->overflow) log_error("net: message overflow"); return false; }
    N->bytes_out += w->len;
    N->win_out += w->len;
    return N->t->send(N->t, peer, channel, w->buf, w->len);
}

static RemoteClient *client_by_peer(PeerId peer) {
    for (int i = 0; i < MAX_PLAYERS; i++) if (N->clients[i].active && N->clients[i].peer == peer) return &N->clients[i];
    return NULL;
}

static void close_transport(void) {
    if (N->t) {
        N->t->flush(N->t);
        N->t->destroy(N->t);
    }
    N->t = NULL;
}

static void discovery_close(Discovery *d) {
    if (d->open) enet_socket_destroy(d->sock);
    d->open = false;
}

// ------------------------------------------------------------------ host

static void send_cvars(RemoteClient *c) {
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_CVARS);
    u32 count_at = w.len;
    w_u16(&w, 0);
    u16 n = 0;
    for (int i = 0; i < cvar_count(); i++) {
        Cvar *cv = cvar_at(i);
        if (!(cv->flags & CVAR_SERVER)) continue;
        w_str(&w, cv->name);
        w_str(&w, cv->str);
        n++;
    }
    memcpy(w.buf + count_at, &n, 2);
    send_msg(c->peer, CH_RELIABLE, &w);
}

static void send_inventory(RemoteClient *c) {
    World *w = game_world();
    Player *p = &w->players[c->slot];
    Writer wr = writer_make(N->buf, NET_BUF);
    w_u8(&wr, MSG_INVENTORY);
    w_u32(&wr, p->inv.revision);
    w_bytes(&wr, p->inv.slots, sizeof(p->inv.slots));
    send_msg(c->peer, CH_RELIABLE, &wr);
    p->inv_sent_revision = p->inv.revision;
}

static void host_handle_hello(PeerId peer, Reader *r) {
    u16 version = r_u16(r);
    char name[32];
    r_str(r, name, sizeof(name));
    if (version != DG_PROTOCOL_VERSION) {
        Writer w = writer_make(N->buf, NET_BUF);
        w_u8(&w, MSG_REJECT);
        w_str(&w, "protocol version mismatch");
        send_msg(peer, CH_RELIABLE, &w);
        N->t->disconnect(N->t, peer);
        return;
    }
    RemoteClient *c = client_by_peer(peer);
    if (c && c->welcomed) return;
    int slot = game_sv_player_join(peer, name[0] ? name : "Player");
    if (slot < 0) {
        Writer w = writer_make(N->buf, NET_BUF);
        w_u8(&w, MSG_REJECT);
        w_str(&w, "server full");
        send_msg(peer, CH_RELIABLE, &w);
        N->t->disconnect(N->t, peer);
        return;
    }
    c = &N->clients[slot];
    memset(c, 0, sizeof(*c));
    c->active = true;
    c->welcomed = true;
    c->peer = peer;
    c->slot = slot;
    c->connect_time = platform_time();
    snap_history_init(&c->history);
    World *w = game_world();
    Writer wr = writer_make(N->buf, NET_BUF);
    w_u8(&wr, MSG_WELCOME);
    w_u8(&wr, (u8)slot);
    w_str(&wr, game()->map);
    w_u32(&wr, (u32)game()->world_seed->i);
    w_u32(&wr, w->tick);
    w_u8(&wr, (u8)N->snaprate->i);
    send_msg(peer, CH_RELIABLE, &wr);
    send_cvars(c);
    send_inventory(c);
    c->cvar_revision = server_cvar_revision();
    log_info("net: '%s' joined as slot %d (peer %d)", name, slot, peer);
}

static void host_handle_cmds(RemoteClient *c, Reader *r) {
    u32 ack = r_u32(r);
    float view_tick = r_f32(r);
    u8 count = r_u8(r);
    if (ack > c->acked_tick) c->acked_tick = ack;
    c->view_tick = view_tick;
    for (int i = 0; i < count && i < CMDS_PER_PACKET; i++) {
        UserCmd cmd;
        r_bytes(r, &cmd, sizeof(cmd));
        if (r->overflow) break;
        game_sv_receive_cmd(c->slot, &cmd);
    }
}

static void host_disconnect_client(PeerId peer) {
    RemoteClient *c = client_by_peer(peer);
    if (!c) return;
    log_info("net: slot %d disconnected", c->slot);
    game_sv_player_leave(c->slot);
    snap_history_free(&c->history);
    memset(c, 0, sizeof(*c));
}

static void host_handle(const NetEvent *e) {
    if (e->type == NETEV_CONNECT) { log_info("net: peer %d connected, waiting for hello", e->peer); return; }
    if (e->type == NETEV_DISCONNECT) { host_disconnect_client(e->peer); return; }
    Reader r = reader_make(e->data, e->size);
    u8 type = r_u8(&r);
    RemoteClient *c = client_by_peer(e->peer);
    switch (type) {
    case MSG_HELLO: host_handle_hello(e->peer, &r); break;
    case MSG_CMDS: if (c) host_handle_cmds(c, &r); break;
    case MSG_RPC: {
        if (!c) break;
        int rt = r_u8(&r);
        int a = r_i32(&r), b = r_i32(&r), cc = r_i32(&r);
        game_sv_handle_rpc(c->slot, rt, a, b, cc);
        break;
    }
    case MSG_BYE: N->t->disconnect(N->t, e->peer); break;
    default: break;
    }
}

void net_host_send_events(World *w) {
    if (N->role != NET_HOST) return;
    for (int k = 0; k < w->event_count; k++) {
        const GameEvent *ev = &w->events[k];
        for (int s = 0; s < MAX_PLAYERS; s++) {
            RemoteClient *c = &N->clients[s];
            if (!c->active || !c->welcomed) continue;
            if (ev->target != EVENT_ALL && ev->target != s) continue;
            if (ev->except == s) continue;
            Writer wr = writer_make(N->buf, NET_BUF);
            w_u8(&wr, MSG_EVENT);
            w_bytes(&wr, ev, sizeof(GameEvent));
            send_msg(c->peer, CH_RELIABLE, &wr);
        }
    }
}

static float relevance_radius(World *w) {
    if (w->chunks) return chunks_relevance_radius();
    return N->relevance->f;
}

void net_host_after_tick(World *w) {
    if (N->role != NET_HOST || !N->t) return;
    int rate = DG_CLAMP(N->snaprate->i, 5, SIM_HZ);
    int interval = DG_MAX(1, SIM_HZ / rate);
    u32 rev = server_cvar_revision();
    for (int s = 0; s < MAX_PLAYERS; s++) {
        RemoteClient *c = &N->clients[s];
        if (!c->active || !c->welcomed) continue;
        Player *p = &w->players[c->slot];
        if (!p->active) continue;
        if (p->inv.revision != p->inv_sent_revision) send_inventory(c);
        if (c->cvar_revision != rev) { send_cvars(c); c->cvar_revision = rev; }
        if (w->tick % (u32)interval) continue;

        SnapFrame *cur = snap_history_slot(&c->history, w->tick);
        cur->tick = w->tick;
        repl_capture(w, p->sim.move.pos, relevance_radius(w), cur);
        SnapFrame *base = snap_history_find(&c->history, c->acked_tick);
        if (base == cur) base = NULL;

        Writer wr = writer_make(N->buf, NET_BUF);
        w_u8(&wr, MSG_SNAPSHOT);
        w_u32(&wr, w->tick);
        w_u32(&wr, base ? base->tick : 0);
        w_u32(&wr, p->last_processed_seq);
        // Private state for the owning client (prediction / reconciliation).
        w_bytes(&wr, &p->sim, sizeof(PlayerSim));
        w_f32(&wr, p->hold_progress);
        w_u32(&wr, p->open_container);
        repl_write_delta(&wr, base, cur);
        N->snap_bytes = N->snap_bytes * 0.9f + (float)wr.len * 0.1f;
        send_msg(c->peer, CH_UNRELIABLE, &wr);
    }
    N->t->flush(N->t);
}

// ------------------------------------------------------------------ client

static void client_send_hello(void) {
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_HELLO);
    w_u16(&w, DG_PROTOCOL_VERSION);
    w_str(&w, game()->cl_name->str);
    send_msg(N->server_peer, CH_RELIABLE, &w);
}

static void client_handle_snapshot(Reader *r) {
    World *w = game_world();
    if (!w || !N->welcomed) return;
    u32 tick = r_u32(r);
    u32 base_tick = r_u32(r);
    u32 ack_seq = r_u32(r);
    PlayerSim sim;
    r_bytes(r, &sim, sizeof(sim));
    float hold = r_f32(r);
    u32 open_container = r_u32(r);
    if (tick <= N->latest_snap) return; // old / duplicate (unreliable reordering)
    // Application-level stats: RTT via acked command, loss via snapshot tick gaps.
    {
        double sent = N->cmd_send_time[ack_seq % 128];
        if (ack_seq && sent > 0) {
            float rtt = (float)((platform_time() - sent) * 1000.0);
            N->app_rtt_ms = N->app_rtt_ms > 0 ? N->app_rtt_ms * 0.9f + rtt * 0.1f : rtt;
        }
        u32 interval = (u32)DG_MAX(1, SIM_HZ / DG_CLAMP(N->snaprate->i, 5, SIM_HZ));
        if (N->latest_snap) {
            N->snaps_expected += (tick - N->latest_snap) / interval;
            N->snaps_received++;
            if (N->snaps_expected >= 60) {
                N->snap_loss_pct = 100.0f * (1.0f - (float)N->snaps_received / (float)N->snaps_expected);
                if (N->snap_loss_pct < 0) N->snap_loss_pct = 0;
                N->snaps_expected = N->snaps_received = 0;
            }
        }
        N->last_snap_bytes = (float)r->len;
    }
    SnapFrame *base = base_tick ? snap_history_find(&N->cl_history, base_tick) : NULL;
    if (base_tick && !base) return; // can't decode without the baseline; host will resend from an older ack
    SnapFrame *out = snap_history_slot(&N->cl_history, tick);
    if (out == base) return;
    out->tick = 0;
    if (!repl_read_delta(r, base, out)) { log_warn("net: malformed snapshot"); return; }
    out->tick = tick;
    N->latest_snap = tick;

    // Clock: estimate the host's simulation time.
    double now = platform_time();
    double server_time = (double)tick * SIM_DT;
    double offset = server_time - now;
    if (!N->time_init) { N->server_time_offset = offset; N->time_init = true; }
    else if (offset > N->server_time_offset) N->server_time_offset = offset;              // packet arrived early: jump forward
    else N->server_time_offset = N->server_time_offset * 0.98 + offset * 0.02;            // drift slowly back

    repl_apply(w, out, N->local_slot, N->interp);
    Player *p = &w->players[N->local_slot];
    if (p->active && p->character) {
        game_cl_reconcile(&sim, ack_seq);
        p->hold_progress = hold;
        p->open_container = open_container;
    }
    w->tick = DG_MAX(w->tick, tick);
}

static void client_handle(const NetEvent *e) {
    if (e->type == NETEV_CONNECT) {
        N->connected = true;
        N->server_peer = e->peer;
        log_info("net: connected, sending hello");
        client_send_hello();
        return;
    }
    if (e->type == NETEV_DISCONNECT) {
        bool was = N->welcomed;
        net_stop();
        game_cl_disconnected(was ? "connection lost" : "could not connect");
        return;
    }
    N->bytes_in += e->size;
    N->win_in += e->size;
    Reader r = reader_make(e->data, e->size);
    u8 type = r_u8(&r);
    switch (type) {
    case MSG_WELCOME: {
        int slot = r_u8(&r);
        char map[64];
        r_str(&r, map, sizeof(map));
        u32 seed = r_u32(&r);
        u32 tick = r_u32(&r);
        r_u8(&r);
        N->welcomed = true;
        N->local_slot = slot;
        N->latest_snap = 0;
        N->time_init = false;
        for (int i = 0; i < SNAP_HISTORY; i++) N->cl_history.frames[i].tick = 0;
        memset(N->interp, 0, sizeof(InterpBuffer) * MAX_ENTITIES);
        game_cl_welcome(slot, map, seed, tick);
        break;
    }
    case MSG_REJECT: {
        char reason[128];
        r_str(&r, reason, sizeof(reason));
        net_stop();
        game_cl_disconnected(reason);
        break;
    }
    case MSG_SNAPSHOT: client_handle_snapshot(&r); break;
    case MSG_EVENT: {
        GameEvent ev;
        r_bytes(&r, &ev, sizeof(ev));
        if (!r.overflow && game_world()) game_handle_event(&ev);
        break;
    }
    case MSG_INVENTORY: {
        World *w = game_world();
        if (!w) break;
        Player *p = &w->players[N->local_slot];
        u32 rev = r_u32(&r);
        r_bytes(&r, p->inv.slots, sizeof(p->inv.slots));
        p->inv.revision = rev;
        break;
    }
    case MSG_CVARS: {
        u16 n = r_u16(&r);
        for (int i = 0; i < n && !r.overflow; i++) {
            char name[64], value[160];
            r_str(&r, name, sizeof(name));
            r_str(&r, value, sizeof(value));
            Cvar *cv = cvar_find(name);
            if (cv && (cv->flags & CVAR_SERVER)) {
                // Host-owned value: bypass local cheat protection.
                u32 flags = cv->flags;
                cv->flags &= ~(u32)CVAR_CHEAT;
                cvar_set_cv(cv, value);
                cv->flags = flags;
            }
        }
        break;
    }
    default: break;
    }
}

void net_client_send_cmds(const UserCmd *history, int size, u32 latest) {
    if (N->role != NET_CLIENT || !N->welcomed || !N->t) return;
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_CMDS);
    w_u32(&w, N->latest_snap);
    w_f32(&w, (float)N->render_tick);
    int count = (int)DG_MIN((u32)CMDS_PER_PACKET, latest);
    w_u8(&w, (u8)count);
    for (u32 s = latest - (u32)count + 1; s <= latest; s++) w_bytes(&w, &history[s % (u32)size], sizeof(UserCmd));
    N->cmd_send_time[latest % 128] = platform_time();
    send_msg(N->server_peer, CH_UNRELIABLE, &w);
    N->t->flush(N->t);
}

void net_client_interpolate(World *w, float dt) {
    if (N->role != NET_CLIENT || !N->time_init) return;
    double server_now = platform_time() + N->server_time_offset;
    N->render_tick = (server_now - N->cl_interp->f / 1000.0) / SIM_DT;
    repl_interpolate(w, N->interp, N->render_tick, N->local_slot);
    entities_client_sync(w);
}

u32 net_client_view_tick256(void) {
    if (!N || N->role != NET_CLIENT) return 0;
    return (u32)(N->render_tick * 256.0);
}

void net_send_rpc(int type, int a, int b, int c) {
    if (N->role != NET_CLIENT || !N->welcomed) return;
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_RPC);
    w_u8(&w, (u8)type);
    w_i32(&w, a);
    w_i32(&w, b);
    w_i32(&w, c);
    send_msg(N->server_peer, CH_RELIABLE, &w);
}

// ------------------------------------------------------------------ LAN discovery

static void discovery_host_open(void) {
    Discovery *d = &N->disc;
    if (d->open) return;
    d->sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (d->sock == ENET_SOCKET_NULL) return;
    enet_socket_set_option(d->sock, ENET_SOCKOPT_REUSEADDR, 1);
    enet_socket_set_option(d->sock, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option(d->sock, ENET_SOCKOPT_BROADCAST, 1);
    ENetAddress a;
    a.host = ENET_HOST_ANY;
    a.port = DG_DISCOVERY_PORT;
    if (enet_socket_bind(d->sock, &a) != 0) {
        log_warn("net: LAN discovery port %d busy (another host on this machine?)", DG_DISCOVERY_PORT);
        enet_socket_destroy(d->sock);
        return;
    }
    d->open = true;
}

static void discovery_host_poll(void) {
    Discovery *d = &N->disc;
    if (!d->open) return;
    for (int k = 0; k < 8; k++) {
        char data[256];
        ENetBuffer b = { .data = data, .dataLength = sizeof(data) - 1 }; // field order differs per platform
        ENetAddress from;
        int n = enet_socket_receive(d->sock, &from, &b, 1);
        if (n <= 0) break;
        data[n] = 0;
        if (strncmp(data, "DGQ1", 4) != 0) continue;
        World *w = game_world();
        int players = 0;
        if (w) for (int s = 0; s < MAX_PLAYERS; s++) if (w->players[s].active) players++;
        char reply[256];
        snprintf(reply, sizeof(reply), "DGA1|%d|%d|%d|%s|%s", N->port->i, players, N->maxplayers->i,
                 game()->map, game()->sv_hostname->str);
        ENetBuffer rb = { .data = reply, .dataLength = strlen(reply) };
        enet_socket_send(d->sock, &from, &rb, 1);
    }
}

void net_lan_refresh(void) {
    Discovery *d = &N->browse;
    if (!d->open) {
        d->sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if (d->sock == ENET_SOCKET_NULL) return;
        enet_socket_set_option(d->sock, ENET_SOCKOPT_NONBLOCK, 1);
        enet_socket_set_option(d->sock, ENET_SOCKOPT_BROADCAST, 1);
        ENetAddress any = { ENET_HOST_ANY, 0 };
        enet_socket_bind(d->sock, &any);
        d->open = true;
    }
    d->server_count = 0;
    d->browsing = true;
    d->browse_until = platform_time() + 3.0;
    d->query_time = platform_time();
    const char *q = "DGQ1";
    ENetBuffer b = { .data = (void *)q, .dataLength = 4 };
    ENetAddress bcast = { ENET_HOST_BROADCAST, DG_DISCOVERY_PORT };
    enet_socket_send(d->sock, &bcast, &b, 1);
    ENetAddress local;
    enet_address_set_host(&local, "127.0.0.1");
    local.port = DG_DISCOVERY_PORT;
    enet_socket_send(d->sock, &local, &b, 1);
}

static void discovery_browse_poll(void) {
    Discovery *d = &N->browse;
    if (!d->open || !d->browsing) return;
    for (int k = 0; k < 16; k++) {
        char data[256];
        ENetBuffer b = { .data = data, .dataLength = sizeof(data) - 1 }; // field order differs per platform
        ENetAddress from;
        int n = enet_socket_receive(d->sock, &from, &b, 1);
        if (n <= 0) break;
        data[n] = 0;
        if (strncmp(data, "DGA1|", 5) != 0) continue;
        int port = 0, players = 0, maxp = 0;
        char map[32] = { 0 }, name[64] = { 0 };
        char *p = data + 5;
        port = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        players = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        maxp = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        *bar = 0;
        dg_strcpy(map, sizeof(map), p);
        dg_strcpy(name, sizeof(name), bar + 1);
        char ip[64];
        enet_address_get_host_ip(&from, ip, sizeof(ip));
        char addr[64];
        snprintf(addr, sizeof(addr), "%s:%d", ip, port);
        bool dup = false;
        for (int i = 0; i < d->server_count; i++) if (!strcmp(d->servers[i].address, addr)) dup = true;
        if (dup || d->server_count >= MAX_LAN_SERVERS) continue;
        LanServer *s = &d->servers[d->server_count++];
        dg_strcpy(s->name, sizeof(s->name), name);
        dg_strcpy(s->map, sizeof(s->map), map);
        dg_strcpy(s->address, sizeof(s->address), addr);
        s->players = players;
        s->max_players = maxp;
        s->ping_ms = (float)((platform_time() - d->query_time) * 1000.0);
        s->last_seen = platform_time();
    }
    if (platform_time() > d->browse_until) d->browsing = false;
}

int net_lan_servers(const LanServer **out) {
    *out = N->browse.servers;
    return N->browse.server_count;
}

// ------------------------------------------------------------------ public API

static NetTransport *make_transport(TransportKind kind) {
    NetTransport *raw = kind == TRANSPORT_STEAM ? transport_steam_create() : transport_lan_create();
    return transport_conditioner_wrap(raw);
}

bool net_host(TransportKind kind, u16 port) {
    if (N->role == NET_HOST && N->kind == kind) return true;
    if (!game_is_host()) { log_warn("net: start a local game before hosting"); return false; }
    net_stop();
    N->t = make_transport(kind);
    if (!N->t) { log_warn("net: %s transport unavailable", kind == TRANSPORT_STEAM ? "Steam" : "LAN"); return false; }
    if (!N->t->listen(N->t, port ? port : (u16)N->port->i, N->maxplayers->i)) {
        close_transport();
        return false;
    }
    N->role = NET_HOST;
    N->kind = kind;
    // Existing local players keep their slots; remote slots are filled as clients join.
    if (kind == TRANSPORT_LAN) discovery_host_open();
    log_info("net: hosting via %s", N->t->name);
    return true;
}

bool net_connect(TransportKind kind, const char *address) {
    net_stop();
    N->t = make_transport(kind);
    if (!N->t) { log_warn("net: transport unavailable"); return false; }
    if (!N->t->connect(N->t, address)) {
        close_transport();
        game_add_hint("Connection failed");
        return false;
    }
    N->role = NET_CLIENT;
    N->kind = kind;
    N->connected = N->welcomed = false;
    N->connect_started = platform_time();
    char hint[96];
    snprintf(hint, sizeof(hint), "Connecting to %s ...", address);
    game_add_hint(hint);
    return true;
}

void net_stop(void) {
    if (!N) return;
    if (N->role == NET_CLIENT && N->t && N->connected) {
        Writer w = writer_make(N->buf, NET_BUF);
        w_u8(&w, MSG_BYE);
        send_msg(N->server_peer, CH_RELIABLE, &w);
    }
    for (int s = 0; s < MAX_PLAYERS; s++) {
        if (N->clients[s].active) snap_history_free(&N->clients[s].history);
        memset(&N->clients[s], 0, sizeof(RemoteClient));
    }
    // Remove remote players from a still-running host world.
    if (N->role == NET_HOST && game_world()) {
        World *w = game_world();
        for (int s = 0; s < MAX_PLAYERS; s++) if (w->players[s].active && !w->players[s].local) game_sv_player_leave(s);
    }
    close_transport();
    discovery_close(&N->disc);
    N->role = NET_OFF;
    N->connected = N->welcomed = false;
    N->time_init = false;
}

NetRole net_role(void) { return N ? N->role : NET_OFF; }

const char *net_transport_name(void) { return N && N->t ? N->t->name : "none"; }

void net_update(float dt) {
    if (!N) return;
    discovery_browse_poll();
    net_steam_poll_idle();
    if (N->role == NET_HOST) discovery_host_poll();
    if (!N->t) return;
    NetEvent events[256];
    int n = N->t->poll(N->t, events, 256);
    for (int i = 0; i < n && N->t; i++) {
        if (N->role == NET_HOST) {
            if (events[i].type == NETEV_DATA) { N->bytes_in += events[i].size; N->win_in += events[i].size; }
            host_handle(&events[i]);
        } else if (N->role == NET_CLIENT) {
            client_handle(&events[i]);
        }
    }
    if (N->role == NET_CLIENT && !N->welcomed && platform_time() - N->connect_started > 10.0) {
        net_stop();
        game_cl_disconnected("connection timed out");
    }
    double now = platform_time();
    if (now - N->stats_time >= 1.0) {
        double span = now - N->stats_time;
        N->in_kbps = (float)(N->win_in / 1024.0 / span);
        N->out_kbps = (float)(N->win_out / 1024.0 / span);
        N->win_in = N->win_out = 0;
        N->stats_time = now;
    }
    (void)dt;
}

NetStats net_stats(void) {
    NetStats s;
    memset(&s, 0, sizeof(s));
    s.transport = net_transport_name();
    s.in_kbps = N->in_kbps;
    s.out_kbps = N->out_kbps;
    s.snapshot_bytes = N->snap_bytes;
    s.snapshot_rate = N->snaprate->i;
    s.interp_ms = N->cl_interp->f;
    s.prediction_error = game() ? game()->prediction_error : 0;
    if (!N->t) return s;
    int count = 0;
    float rtt_sum = 0, loss_sum = 0;
    if (N->role == NET_HOST) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!N->clients[i].active) continue;
            float rtt, loss;
            if (N->t->peer_stats(N->t, N->clients[i].peer, &rtt, &loss)) { rtt_sum += rtt; loss_sum += loss; count++; }
        }
    } else if (N->role == NET_CLIENT && N->connected) {
        float rtt, loss;
        if (N->t->peer_stats(N->t, N->server_peer, &rtt, &loss)) { rtt_sum = rtt; loss_sum = loss; count = 1; }
    }
    s.peers = count;
    if (count) { s.ping_ms = rtt_sum / (float)count; s.loss_pct = loss_sum / (float)count; }
    if (N->role == NET_CLIENT) {
        // Application-level numbers include simulated lag/loss and host processing.
        s.app_rtt_ms = N->app_rtt_ms;
        s.loss_pct = fmaxf(s.loss_pct, N->snap_loss_pct);
        s.snapshot_bytes = N->last_snap_bytes;
    }
    return s;
}

// ------------------------------------------------------------------ console commands

static void cmd_connect(int argc, const char **argv) {
    if (argc < 2) { log_info("usage: connect <ip[:port]>"); return; }
    net_connect(TRANSPORT_LAN, argv[1]);
}

static void cmd_host(int argc, const char **argv) {
    bool steam = argc > 1 && !strcmp(argv[1], "steam");
    if (!game_is_host()) game_host(game()->g_map->str);
    net_host(steam ? TRANSPORT_STEAM : TRANSPORT_LAN, (u16)N->port->i);
}

static void cmd_disconnect(int argc, const char **argv) { game_disconnect_to_local("disconnected"); }

static void cmd_lanlist(int argc, const char **argv) {
    for (int i = 0; i < N->browse.server_count; i++)
        log_info("  %s  %s  %s  %d/%d", N->browse.servers[i].address, N->browse.servers[i].name, N->browse.servers[i].map,
                 N->browse.servers[i].players, N->browse.servers[i].max_players);
}

static void cmd_lanrefresh(int argc, const char **argv) { net_lan_refresh(); }

bool net_init(void) {
    N = (NetSystem *)calloc(1, sizeof(NetSystem));
    if (enet_initialize() != 0) { log_error("net: enet_initialize failed"); return false; }
    N->port = cvar_reg_int("net_port", DG_DEFAULT_PORT, 1, 65535, CVAR_ARCHIVE, "UDP port for LAN hosting");
    N->snaprate = cvar_reg_int("sv_snaprate", 30, 5, 60, CVAR_ARCHIVE, "snapshots per second sent to clients");
    N->cl_interp = cvar_reg_float("cl_interp", 100.0f, 0.0f, 500.0f, CVAR_ARCHIVE, "remote entity interpolation delay (ms)");
    N->maxplayers = cvar_reg_int("sv_maxplayers", 4, 1, MAX_PLAYERS, CVAR_ARCHIVE, "maximum players when hosting");
    N->showstats = cvar_reg_bool("net_showstats", false, CVAR_ARCHIVE, "show network statistics overlay");
    N->relevance = cvar_reg_float("sv_relevance", 120.0f, 10.0f, 2000.0f, CVAR_ARCHIVE, "entity relevance distance (m)");
    N->interp = (InterpBuffer *)calloc(MAX_ENTITIES, sizeof(InterpBuffer));
    snap_history_init(&N->cl_history);
    N->stats_time = platform_time();
    cmd_register("connect", cmd_connect, "connect <ip[:port]> to a LAN/direct host");
    cmd_register("host", cmd_host, "host [lan|steam]: accept players into this game");
    cmd_register("disconnect", cmd_disconnect, "leave the session / stop hosting");
    cmd_register("lan_refresh", cmd_lanrefresh, "broadcast a LAN server query");
    cmd_register("lan_list", cmd_lanlist, "list discovered LAN servers");
    net_steam_register_commands();
    return true;
}

void net_shutdown(void) {
    if (!N) return;
    net_stop();
    discovery_close(&N->browse);
    snap_history_free(&N->cl_history);
    free(N->interp);
    enet_deinitialize();
    net_steam_shutdown();
    free(N);
    N = NULL;
}
