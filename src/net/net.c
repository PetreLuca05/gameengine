// Session layer: host-authoritative peer-to-peer over a NetTransport (LAN or Steam).
#include "net.h"
#include "anim/anim_library.h"
#include "anim/anim_net.h"
#include "gameplay/characters.h"
#include "net_transport.h"
#include "lan_ifaddr.h"
#include "protocol.h"
#include "bitbuf.h"
#include "replication.h"
#include "net_prof.h"
#include "lobby_net.h"
#include "ui/mainmenu/mainmenu.h"
#include "spectate.h"
#include "player/spectator.h"
#include "gameplay/zombies/horde.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/consistency.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/game_internal.h"
#include "world/chunks.h"
#include "gameplay/entities.h"
#include "gameplay/inventory/inv_wire.h"
#include <enet/enet.h>
#include <stdlib.h>
#include <stdio.h>

#define NET_BUF 131072
// A peer that loads a map answers nothing until its load ends (a large open world: 15 s and more), and
// the loading side reads the other's answers only after it: the peer is not dropped for this long
// (net_peer_load_grace), then the transport's timeout again.
#define NET_LOAD_GRACE_MS 90000

typedef struct RemoteClient {
    bool active;
    bool welcomed;
    bool loading;                  // welcomed, no commands yet (the map is loading): the load grace
    PeerId peer;
    int slot;
    SnapHistory history;
    u32 acked_tick;
    u32 cvar_revision;
    u16 weather_version;           // last weather version sent
    u8 weather_resend;             // snapshots left that carry the weather after a change
    u32 weather_count;             // snapshots since the last periodic weather pack
    double connect_time;
    float view_tick;
    u8 zombie_age[MAX_ENTITIES];   // snapshots since each zombie was last updated (repl_rate_limit)
    int zombies_held;
    u64 bytes_sent;
    int spec_inv_slot;             // spectated player whose inventory was sent last (-1 none)
    u32 spec_inv_rev;
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
    bool cl_loading;            // the host's peer has the load grace until the first snapshot
    bool lobby_seen;            // the host answered with the lobby (we wait there: no connect timeout)
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
        if (c->flags & (CVAR_SERVER | CVAR_RULE)) sum += c->modified + (u32)i;
    }
    return sum;
}

// ------------------------------------------------------------------ helpers

static bool send_msg(PeerId peer, int channel, const Writer *w) {
    if (!N->t || w->overflow) { if (w->overflow) log_error("net: message overflow"); return false; }
    N->bytes_out += w->len;
    N->win_out += w->len;
    if (w->len) netprof_message(w->buf[0], w->len, channel == CH_RELIABLE);
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
        if (!(cv->flags & (CVAR_SERVER | CVAR_RULE))) continue;   // gameplay rules + the server rules of the consistency set
        w_str(&w, cv->name);
        w_str(&w, cv->str);
        n++;
    }
    memcpy(w.buf + count_at, &n, 2);
    send_msg(c->peer, CH_RELIABLE, &w);
}

// MSG_INVENTORY of player `slot`: the client's own, or the player it spectates (HUD ammo / hotbar).
static void send_inventory_of(RemoteClient *c, int slot) {
    World *w = game_world();
    Player *p = &w->players[slot];
    Writer wr = writer_make(N->buf, NET_BUF);
    w_u8(&wr, MSG_INVENTORY);
    w_u8(&wr, (u8)slot);
    w_u32(&wr, p->inv.revision);
    w_u32(&wr, p->inv_ack_seq);
    for (int k = 0; k < INV_HOTBAR; k++) w_u16(&wr, p->inv.quick[k]);
    static u8 tree[INV_WIRE_TREE_MAX];
    int n = inv_wire_write_tree(&p->inv.t, tree, sizeof(tree));
    w_u16(&wr, (u16)n);
    w_bytes(&wr, tree, (u32)n);
    send_msg(c->peer, CH_RELIABLE, &wr);
    if (slot == c->slot) p->inv_sent_revision = p->inv.revision;
    else { c->spec_inv_slot = slot; c->spec_inv_rev = p->inv.revision; }
}

static void send_inventory(RemoteClient *c) { send_inventory_of(c, c->slot); }

// MSG_LOOT: the world inventory the client's player has open (relevance: only open containers'
// trees travel), resent whenever its revision changes; a closed / gone one is sent once as closed.
static void send_loot(RemoteClient *c, World *w, Player *p) {
    EntityId oc = p->open_container;
    InvTree *t = container_tree(w, oc, false);
    if (!t) oc = ENT_NONE;
    u32 rev = oc ? w->cont[ENT_INDEX(oc)].revision : 0;
    if (p->loot_sent_ent == oc && p->loot_sent_rev == rev) return;
    Writer wr = writer_make(N->buf, NET_BUF);
    w_u8(&wr, MSG_LOOT);
    w_u32(&wr, oc);
    w_u32(&wr, rev);
    w_u8(&wr, t ? 1 : 0);
    if (t) {
        static u8 tree[INV_WIRE_TREE_MAX];
        int n = inv_wire_write_tree(t, tree, sizeof(tree));
        w_u16(&wr, (u16)n);
        w_bytes(&wr, tree, (u32)n);
    }
    send_msg(c->peer, CH_RELIABLE, &wr);
    p->loot_sent_ent = oc;
    p->loot_sent_rev = rev;
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
    // Animation asset identity: every category hash must match the host's.
    {
        u8 cats = r_u8(r);
        AnimManifest theirs;
        memset(&theirs, 0, sizeof(theirs));
        for (int k = 0; k < cats && k < AC_CATEGORY_COUNT; k++) theirs.hash[k] = r_u32(r);
        const AnimManifest *mine = g_engine.anim ? &g_engine.anim->manifest : NULL;
        char reason[256] = "";
        int len = 0;
        if (mine) {
            for (int k = 0; k < AC_CATEGORY_COUNT; k++)
                if (k >= cats || theirs.hash[k] != mine->hash[k])
                    len += snprintf(reason + len, sizeof(reason) - (size_t)len, "%s%s (host %08x, you %08x)", len ? ", " : "asset mismatch: ",
                                    asset_category_name(k), mine->hash[k], k < cats ? theirs.hash[k] : 0);
        }
        if (len > 0) {
            log_warn("net: rejecting '%s': %s", name, reason);
            Writer w = writer_make(N->buf, NET_BUF);
            w_u8(&w, MSG_REJECT);
            w_str(&w, reason);
            send_msg(peer, CH_RELIABLE, &w);
            N->t->disconnect(N->t, peer);
            return;
        }
    }
    // Consistency set (docs/SETTINGS_AUDIT.md): the values that decide what a player can see must be
    // the host's, or the client could see more than everyone else.
    {
        char reason[160];
        if (!consistency_join_ok(r_u32(r), reason, sizeof(reason))) {
            log_warn("net: rejecting '%s': %s", name, reason);
            Writer w = writer_make(N->buf, NET_BUF);
            w_u8(&w, MSG_REJECT);
            w_str(&w, reason);
            send_msg(peer, CH_RELIABLE, &w);
            N->t->disconnect(N->t, peer);
            return;
        }
    }
    // Lobby (docs/MENU_LOBBY.md): build / content identity, kick bans, lobby size; players wait in
    // the lobby until the host starts (late joiners into a running game are welcomed at once).
    LobbyHello lh;
    lobby_read_hello_ext(r, &lh);
    dg_strcpy(lh.name, sizeof(lh.name), name);
    char why[160];
    int join = lobby_on_hello(peer, &lh, why, sizeof(why));
    if (join == LJ_REJECT) {
        log_warn("net: rejecting '%s': %s", name, why);
        Writer w = writer_make(N->buf, NET_BUF);
        w_u8(&w, MSG_REJECT);
        w_str(&w, why);
        send_msg(peer, CH_RELIABLE, &w);
        N->t->flush(N->t);
        N->t->disconnect(N->t, peer);
        return;
    }
    if (join == LJ_LOBBY) return;
    net_host_welcome(peer, name);
}

// Spawns a connected peer into the running game: WELCOME (map, seed, tick), server cvars, inventory.
bool net_host_welcome(int peer, const char *name) {
    if (N->role != NET_HOST || !N->t) return false;
    RemoteClient *c = client_by_peer(peer);
    if (c && c->welcomed) return true;
    int slot = game_sv_player_join(peer, name[0] ? name : "Player");
    if (slot < 0) {
        Writer w = writer_make(N->buf, NET_BUF);
        w_u8(&w, MSG_REJECT);
        w_str(&w, "server full");
        send_msg(peer, CH_RELIABLE, &w);
        N->t->disconnect(N->t, peer);
        return false;
    }
    c = &N->clients[slot];
    memset(c, 0, sizeof(*c));
    c->active = true;
    c->welcomed = true;
    c->peer = peer;
    c->slot = slot;
    c->spec_inv_slot = -1;
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
    c->loading = true;
    net_peer_load_grace(peer);
    log_info("net: '%s' joined as slot %d (peer %d)", name, slot, peer);
    return true;
}

void net_peer_load_grace(int peer) {
    if (!N || !N->t || !N->t->set_peer_timeout) return;
    if (N->role == NET_CLIENT) {   // the host's peer, whatever is passed
        peer = N->server_peer;
        N->cl_loading = true;
    }
    N->t->set_peer_timeout(N->t, peer, NET_LOAD_GRACE_MS);
}

static void host_handle_cmds(RemoteClient *c, Reader *r) {
    u32 ack = r_u32(r);
    float view_tick = r_f32(r);
    u32 send_ms = r_u32(r);
    u8 count = r_u8(r);
    if (ack > c->acked_tick) c->acked_tick = ack;
    c->view_tick = view_tick;
    if (c->loading) {   // loaded: the normal timeout again
        c->loading = false;
        if (N->t->set_peer_timeout) N->t->set_peer_timeout(N->t, c->peer, 0);
    }
    UserCmd cmds[CMDS_PER_PACKET];
    int n = 0;
    for (int i = 0; i < count && i < CMDS_PER_PACKET; i++) {
        r_bytes(r, &cmds[n], sizeof(UserCmd));
        if (r->overflow) break;
        game_sv_receive_cmd(c->slot, &cmds[n]);
        n++;
    }
    // Relayed to this player's in-eye observers right here (flushed at the end of net_update).
    if (n && game_world()) spectate_host_cmds(game_world(), c->slot, cmds, n, send_ms);
}

// MSG_SPEC_CMDS: target's commands from .. to for the observer in slot obs (spectate.c decides).
static void spec_send(int obs, int target, u32 from, u32 to) {
    RemoteClient *c = &N->clients[obs];
    const SpecCmdRing *ring = spectate_host_ring(target);
    if (!c->active || !c->welcomed || !ring || !N->t) return;
    Writer wr = writer_make(N->buf, NET_BUF);
    w_u8(&wr, MSG_SPEC_CMDS);
    spec_write_cmds(&wr, (u8)target, ring, from, to);
    spectate_count_relay_bytes(obs, wr.len);
    c->bytes_sent += wr.len;
    send_msg(c->peer, CH_UNRELIABLE, &wr);
}

// One-way delay estimate from a player to the host (spectator delay readout).
u16 net_player_owd_ms(int slot) {
    if (slot < 0 || slot >= MAX_PLAYERS || !N->t) return 0;
    RemoteClient *c = &N->clients[slot];
    float rtt, loss;
    if (!c->active || !N->t->peer_stats(N->t, c->peer, &rtt, &loss)) return 0;   // the host's own player: 0
    return (u16)DG_CLAMP((int)(rtt * 0.5f), 0, 65535);
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
    if (e->type == NETEV_DISCONNECT) { host_disconnect_client(e->peer); lobby_on_peer_left(e->peer); return; }
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
    case MSG_INV_INTENT: {
        if (!c) break;
        u8 b[INV_WIRE_INTENT];
        r_bytes(&r, b, sizeof(b));
        if (r.overflow) break;
        InvIntent in;
        inv_wire_read_intent(&in, b);
        game_sv_inv_intent(c->slot, &in);
        break;
    }
    case MSG_BYE: N->t->disconnect(N->t, e->peer); break;
    case MSG_LOBBY: lobby_on_host_msg(e->peer, &r); break;
    default: break;
    }
}

void net_host_send_anim_events(World *w) {
    if (N->role != NET_HOST || !w->anim_event_count) return;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        RemoteClient *c = &N->clients[s];
        if (!c->active || !c->welcomed) continue;
        int k = 0;
        while (k < w->anim_event_count) {
            Writer wr = writer_make(N->buf, NET_BUF);
            w_u8(&wr, MSG_ANIM_EVENTS);
            u32 count_at = wr.len;
            w_u8(&wr, 0);
            u8 n = 0;
            while (k < w->anim_event_count && n < 200 && wr.len + 20 < NET_BUF) {
                anim_event_write(&wr, &w->anim_events[k]);
                k++;
                n++;
            }
            wr.buf[count_at] = n;
            send_msg(c->peer, CH_RELIABLE, &wr);
        }
    }
}

int net_host_client_count(void) {
    int n = 0;
    for (int s = 0; s < MAX_PLAYERS; s++) if (N->clients[s].active && N->clients[s].welcomed) n++;
    return n;
}

double net_client_render_tick(void) { return N ? N->render_tick : 0.0; }

void net_host_send_events(World *w) {
    if (N->role != NET_HOST) return;
    for (int k = 0; k < w->event_count; k++) {
        const GameEvent *ev = &w->events[k];
        for (int s = 0; s < MAX_PLAYERS; s++) {
            RemoteClient *c = &N->clients[s];
            if (!c->active || !c->welcomed) continue;
            // Own events, plus the HUD feedback of the player this client spectates (spectate_core.c).
            if (!spec_event_delivers(ev->target, ev->except, ev->type, s, spectate_event_target(s))) continue;
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
        send_loot(c, w, p);
        if (c->cvar_revision != rev) { send_cvars(c); c->cvar_revision = rev; }
        // Spectating: the target's local data, inventory and relevance (docs/SPECTATE.md).
        int src = spectate_source(w, c->slot);
        Player *sp = &w->players[src];
        if (src != c->slot && (c->spec_inv_slot != src || c->spec_inv_rev != sp->inv.revision)) send_inventory_of(c, src);
        SpecBlock sb;
        spectate_fill_block(w, c->slot, &sb);
        bool forced = (sb.flags & SPECF_FORCED) != 0;
        if ((w->tick % (u32)interval) && !forced) continue;
        sb.target_owd_ms = net_player_owd_ms(src);
        vec3 center = spectate_relevance_center(w, c->slot);

        SnapFrame *cur = snap_history_slot(&c->history, w->tick);
        cur->tick = w->tick;
        repl_capture(w, center, relevance_radius(w), cur);
        SnapFrame *base = snap_history_find(&c->history, c->acked_tick);
        if (base == cur) base = NULL;
        {
            const HordeConfig *hc = horde_config();
            int budget = hc ? hc->net_budget / DG_MAX(1, rate) : 0;
            c->zombies_held = repl_rate_limit(base, cur, center, w->tick / (u32)interval, c->zombie_age, budget);
        }

        Writer wr = writer_make(N->buf, NET_BUF);
        w_u8(&wr, MSG_SNAPSHOT);
        w_u32(&wr, w->tick);
        w_u32(&wr, base ? base->tick : 0);
        u32 sec_at = wr.len;   // profiler: bytes of every section and whether they changed (net_prof.c)
        netprof_section(s, NS_HEADER, wr.buf, sec_at);
        // Local data (prediction / reconciliation) of the client's player, or of the player it
        // spectates (the observer runs it through the same pipeline), then the spectator block.
        SpecLocalData ld;
        ld.ack_seq = sp->last_processed_seq;
        ld.sim = sp->sim;
        ld.hold_progress = sp->hold_progress;
        ld.open_container = src == c->slot ? sp->open_container : ENT_NONE;
        spec_write_local(&wr, &ld);
        netprof_section(s, NS_LOCAL, wr.buf + sec_at, wr.len - sec_at);
        sec_at = wr.len;
        spec_write_block(&wr, &sb);
        netprof_section(s, NS_SPECTATE, wr.buf + sec_at, wr.len - sec_at);
        sec_at = wr.len;
        {
            // World time (every snapshot: 20 bytes, late joiners are in sync at once).
            u8 tw[20];
            tod_pack(&w->tod, tw);
            w_bytes(&wr, tw, sizeof(tw));
            netprof_section(s, NS_TIME, tw, sizeof(tw));
            sec_at = wr.len;
            // Weather (120 bytes): a few snapshots after every change, then every 10th (late
            // joiners: the first snapshot carries it, version 0 never matches).
            if (c->weather_version != w->weather.version) { c->weather_version = w->weather.version; c->weather_resend = 3; }
            bool send_wx = c->weather_resend > 0 || ++c->weather_count >= 10 || c->acked_tick == 0;
            if (c->weather_resend > 0) c->weather_resend--;
            if (send_wx) c->weather_count = 0;
            w_u8(&wr, send_wx ? 1 : 0);
            if (send_wx) {
                u8 wx[WEATHER_PACK_SIZE];
                weather_pack(&w->weather, wx);
                w_bytes(&wr, wx, sizeof(wx));
            }
            netprof_section(s, NS_WEATHER, wr.buf + sec_at, wr.len - sec_at);
            sec_at = wr.len;
            vehicles_net_write(w, &wr);   // every snapshot: pose, velocity, fuel, tires, seats
            netprof_section(s, NS_VEHICLES, wr.buf + sec_at, wr.len - sec_at);
            sec_at = wr.len;
        }
        repl_write_delta(&wr, base, cur);
        netprof_section(s, NS_ENTITIES, wr.buf + sec_at, wr.len - sec_at);
        N->snap_bytes = N->snap_bytes * 0.9f + (float)wr.len * 0.1f;
        c->bytes_sent += wr.len;
        send_msg(c->peer, CH_UNRELIABLE, &wr);
        if (forced) {
            // Target switch: the commands the target sent since this state go right after it, so the
            // observer replays from the snapshot to the target's present at once (no pop, no wait).
            const SpecCmdRing *ring = spectate_host_ring(src);
            if (ring && ring->newest > sp->last_processed_seq) spec_send(c->slot, src, sp->last_processed_seq + 1, ring->newest);
        }
    }
    N->t->flush(N->t);
}

// ------------------------------------------------------------------ client

static void client_send_hello(void) {
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_HELLO);
    w_u16(&w, DG_PROTOCOL_VERSION);
    w_str(&w, lobby_local_name());
    w_u8(&w, AC_CATEGORY_COUNT);
    for (int k = 0; k < AC_CATEGORY_COUNT; k++) w_u32(&w, g_engine.anim ? g_engine.anim->manifest.hash[k] : 0);
    w_u32(&w, consistency_hash());   // the shared values (assets/render/consistency.json)
    lobby_write_hello_ext(&w);
    send_msg(N->server_peer, CH_RELIABLE, &w);
}

static void client_handle_snapshot(Reader *r) {
    World *w = game_world();
    if (!w || !N->welcomed) return;
    u32 tick = r_u32(r);
    u32 base_tick = r_u32(r);
    SpecLocalData ld;
    spec_read_local(r, &ld);
    SpecBlock sb;
    spec_read_block(r, &sb);
    bool own = sb.data_slot == N->local_slot;   // else the spectated player's (docs/SPECTATE.md)
    u32 ack_seq = own ? ld.ack_seq : 0;
    u8 tw[20];
    r_bytes(r, tw, sizeof(tw));
    u8 has_wx = r_u8(r);
    u8 wx[WEATHER_PACK_SIZE];
    if (has_wx) r_bytes(r, wx, sizeof(wx));
    vehicles_net_read(w, r, tick > N->latest_snap, ack_seq);
    if (tick <= N->latest_snap) return; // old / duplicate (unreliable reordering)
    if (!r->overflow) tod_unpack(&w->tod, tw);
    if (!r->overflow && has_wx) weather_unpack(&w->weather, wx);
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
    spectator_cl_block(w, &sb, tick);
    Player *p = &w->players[N->local_slot];
    if (own && p->active && p->character) {
        game_cl_reconcile(&ld.sim, ack_seq);
        p->hold_progress = ld.hold_progress;
        p->open_container = ld.open_container;
    } else if (!own) {
        spectator_cl_local_data(w, sb.data_slot, &ld, &sb);
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
        const char *why = was || N->lobby_seen ? "connection lost" : "could not connect";
        net_stop();
        if (!lobby_on_client_lost(why, false)) game_cl_disconnected(why);
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
        net_peer_load_grace(e->peer);   // the map load blocks: the host's unanswered packets wait
        game_cl_welcome(slot, map, seed, tick);
        lobby_on_welcome();
        break;
    }
    case MSG_REJECT: {
        char reason[160];
        r_str(&r, reason, sizeof(reason));
        net_stop();
        if (!lobby_on_client_lost(reason, true)) game_cl_disconnected(reason);
        break;
    }
    case MSG_LOBBY:
        N->lobby_seen = true;
        lobby_on_client_msg(&r);
        break;
    case MSG_SNAPSHOT:
        // The first snapshot after a load: the acks that waited behind it are in, the normal timeout again.
        if (N->cl_loading && N->t->set_peer_timeout) N->t->set_peer_timeout(N->t, e->peer, 0);
        N->cl_loading = false;
        client_handle_snapshot(&r);
        break;
    case MSG_EVENT: {
        GameEvent ev;
        r_bytes(&r, &ev, sizeof(ev));
        if (!r.overflow && game_world()) game_handle_event(&ev);
        break;
    }
    case MSG_ANIM_EVENTS: {
        u8 n = r_u8(&r);
        World *w = game_world();
        for (int k = 0; k < n; k++) {
            AnimEvent ev;
            if (!anim_event_read(&r, &ev)) break;
            if (w) chars_on_event(w, &ev);
        }
        break;
    }
    case MSG_INVENTORY: {
        World *w = game_world();
        if (!w) break;
        int slot = r_u8(&r);
        u32 rev = r_u32(&r);
        u32 ack = r_u32(&r);
        u16 quick[INV_HOTBAR];
        for (int k = 0; k < INV_HOTBAR; k++) quick[k] = r_u16(&r);
        u16 n = r_u16(&r);
        static u8 buf[INV_WIRE_TREE_MAX];
        static InvTree tree;
        if (n > sizeof(buf)) break;
        r_bytes(&r, buf, n);
        if (r.overflow || slot >= MAX_PLAYERS || !inv_wire_read_tree(&tree, buf, n)) break;
        if (slot == N->local_slot) game_cl_inventory(N->local_slot, rev, ack, quick, &tree);   // reconcile with the predicted intents
        else spectator_cl_inventory(w, slot, rev, quick, &tree);                            // the spectated player's (HUD)
        break;
    }
    case MSG_LOOT: {
        EntityId e = r_u32(&r);
        u32 rev = r_u32(&r);
        u8 open = r_u8(&r);
        static u8 buf[INV_WIRE_TREE_MAX];
        static InvTree tree;
        if (!open) { if (!r.overflow) game_cl_loot(e, rev, NULL); break; }
        u16 n = r_u16(&r);
        if (n > sizeof(buf)) break;
        r_bytes(&r, buf, n);
        if (r.overflow || !inv_wire_read_tree(&tree, buf, n)) break;
        game_cl_loot(e, rev, &tree);
        break;
    }
    case MSG_SPEC_CMDS: {
        World *w = game_world();
        if (w) spectator_cl_cmds(w, &r);
        break;
    }
    case MSG_CVARS: {
        u16 n = r_u16(&r);
        for (int i = 0; i < n && !r.overflow; i++) {
            char name[64], value[160];
            r_str(&r, name, sizeof(name));
            r_str(&r, value, sizeof(value));
            Cvar *cv = cvar_find(name);
            if (cv && (cv->flags & (CVAR_SERVER | CVAR_RULE))) {
                // Host-owned value: bypass local cheat protection and the settings locks.
                u32 flags = cv->flags;
                cv->flags &= ~(u32)CVAR_CHEAT;
                CvarSource src = cvar_source_push(CVAR_SRC_HOST);
                cvar_set_cv(cv, value);
                cvar_source_pop(src);
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
    w_u32(&w, platform_wall_ms());   // send stamp: relayed to observers (spectator delay, same machine)
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

void net_send_inv_intent(const InvIntent *in) {
    if (N->role != NET_CLIENT || !N->welcomed) return;
    Writer w = writer_make(N->buf, NET_BUF);
    w_u8(&w, MSG_INV_INTENT);
    u8 b[INV_WIRE_INTENT];
    inv_wire_write_intent(in, b);
    w_bytes(&w, b, sizeof(b));
    send_msg(N->server_peer, CH_RELIABLE, &w);
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
        if (lobby_discovery_hidden()) continue;   // invite-only lobby
        World *w = game_world();
        int players = lobby_discovery_players();
        if (players < 0) { players = 0; if (w) for (int s = 0; s < MAX_PLAYERS; s++) if (w->players[s].active) players++; }
        char reply[256];
        // DGA2|port|players|max|map|state (0 lobby, 1 in game)|build hash|name
        const Lobby *L = lobby_get();
        snprintf(reply, sizeof(reply), "DGA2|%d|%d|%d|%s|%d|%08x|%s", N->port->i, players, N->maxplayers->i,
                 lobby_active() ? L->settings.map : game()->map, lobby_discovery_state(), lobby_version_hash_local(), game()->sv_hostname->str);
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
    int sent = enet_socket_send(d->sock, &bcast, &b, 1);
    // each interface's subnet broadcast too (macOS: the limited broadcast leaves on one interface)
    u32 nets[8];
    int nn = lan_broadcast_addrs(nets, 8);
    for (int i = 0; i < nn; i++) {
        ENetAddress a = { ENET_HOST_TO_NET_32(nets[i]), DG_DISCOVERY_PORT };
        int r = enet_socket_send(d->sock, &a, &b, 1);
        if (r > sent) sent = r;
    }
    // A refused broadcast: macOS 15+ without the Local Network permission (System Settings > Privacy &
    // Security > Local Network), or a firewall. Same-machine games are still found (127.0.0.1 below).
    static bool warned;
    if (sent < 0 && !warned) {
        warned = true;
        log_warn("net: LAN broadcast refused (macOS: allow Deadground under Privacy & Security > Local Network); only this machine's games are listed");
    }
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
        bool v2 = !strncmp(data, "DGA2|", 5);
        if (!v2 && strncmp(data, "DGA1|", 5) != 0) continue;
        int port = 0, players = 0, maxp = 0, state = 1;
        u32 version = 0;
        char map[32] = { 0 }, name[64] = { 0 };
        char *p = data + 5;
        port = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        players = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        maxp = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        *bar = 0;
        dg_strcpy(map, sizeof(map), p);
        p = bar + 1;
        if (v2) {
            state = atoi(p); p = strchr(p, '|'); if (!p) continue; p++;
            version = (u32)strtoul(p, NULL, 16); p = strchr(p, '|'); if (!p) continue; p++;
        }
        dg_strcpy(name, sizeof(name), p);
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
        s->in_game = state != 0;
        s->version = version;
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
    N->connected = N->welcomed = N->lobby_seen = false;
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
    N->connected = N->welcomed = N->lobby_seen = false;
    N->time_init = false;
}

// ------------------------------------------------------------------ lobby helpers (lobby_net.c)

bool net_send_reliable(int peer, const void *data, u32 size) {
    if (!N || !N->t || N->role == NET_OFF) return false;
    if (N->role == NET_CLIENT) peer = N->server_peer;
    N->bytes_out += size;
    N->win_out += size;
    return N->t->send(N->t, peer, CH_RELIABLE, data, size);
}

void net_flush(void) { if (N && N->t) N->t->flush(N->t); }

void net_drop_peer(int peer) {
    if (!N || !N->t || N->role != NET_HOST) return;
    N->t->flush(N->t);
    N->t->disconnect(N->t, peer);
}

bool net_peer_address(int peer, char *out, int size) {
    if (!N || !N->t || !N->t->peer_address) return false;
    return N->t->peer_address(N->t, N->role == NET_CLIENT ? N->server_peer : peer, out, size);
}

float net_peer_ping(int peer) {
    float rtt = 0, loss = 0;
    if (!N || !N->t || !N->t->peer_stats(N->t, N->role == NET_CLIENT ? N->server_peer : peer, &rtt, &loss)) return -1.0f;
    return rtt;
}

TransportKind net_kind(void) { return N ? N->kind : TRANSPORT_LAN; }
bool net_client_welcomed(void) { return N && N->role == NET_CLIENT && N->welcomed; }
int net_listen_port(void) { return N ? N->port->i : DG_DEFAULT_PORT; }

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
    if (N->role == NET_HOST && N->t) N->t->flush(N->t);   // relayed spectator commands leave in this frame
    if (N->role == NET_CLIENT && !N->welcomed && !N->lobby_seen && platform_time() - N->connect_started > 10.0) {
        net_stop();
        if (!lobby_on_client_lost("connection timed out", false)) game_cl_disconnected("connection timed out");
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
    // With the main menu: through the lobby (version check, lobby screen, loading screen).
    if (mainmenu_enabled()) { lobby_join(TRANSPORT_LAN, argv[1]); return; }
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

// net_snapstats: per-client snapshot bandwidth since the last call (host), held zombie updates;
// on a client the zombie motion smoothness.
static void cmd_snapstats(int argc, const char **argv) {
    (void)argc; (void)argv;
    if (N->role == NET_CLIENT) {
        float sp = 0, j = repl_zombie_jitter(&sp);
        log_info("net_snapstats: client zombie motion: mean speed %.2f m/s, velocity change per frame %.3f m/s (%.0f %%)", sp, j, sp > 0 ? 100.0f * j / sp : 0.0f);
        return;
    }
    static double last;
    double now = platform_time();
    double span = last > 0 ? now - last : 1.0;
    last = now;
    for (int s = 0; s < MAX_PLAYERS; s++) {
        RemoteClient *c = &N->clients[s];
        if (!c->active) continue;
        u32 relay = spectate_take_relay_bytes(s);
        log_info("net_snapstats: client %d: %.1f kB/s snapshots + relays (%.1f s), %d zombie updates held last snapshot; spectator relay %.1f kB/s (source slot %d)", s,
                 (double)c->bytes_sent / 1024.0 / span, span, c->zombies_held, (double)relay / 1024.0 / span, game_world() ? spectate_source(game_world(), c->slot) : -1);
        c->bytes_sent = 0;
    }
    void repl_debug_dump(void);
    repl_debug_dump();
}

// CVAR_LOCKED | CVAR_RULE cvars (docs/SETTINGS_AUDIT.md): the host's server rules. A client takes them
// from the host (MSG_CVARS) and may not set them itself.
static bool can_set_rules(void) { return !N || N->role != NET_CLIENT; }

// Scripts: waits (one frame per retry, the rest of the script after it) until n remote clients are
// welcomed, at most `timeout` frames (perf_audit LAN scene: the host captures with its client in).
static void cmd_net_wait_clients(int argc, const char **argv) {
    int want = argc > 1 ? atoi(argv[1]) : 1, left = argc > 2 ? atoi(argv[2]) : 3600;
    int have = net_host_client_count();
    if (have >= want) { log_info("net_wait_clients %d: done", want); return; }
    if (left <= 0) { log_warn("net_wait_clients %d: timed out with %d client(s)", want, have); return; }
    char line[64];
    snprintf(line, sizeof(line), "net_wait_clients %d %d", want, left - 1);
    cbuf_retry(line);
}

bool net_init(void) {
    N = (NetSystem *)calloc(1, sizeof(NetSystem));
    cvar_set_rule_gate(can_set_rules);
    netprof_register();   // profile section "net" (docs/PERF_AUDIT.md)
    if (enet_initialize() != 0) { log_error("net: enet_initialize failed"); return false; }
    N->port = cvar_reg_int("net_port", DG_DEFAULT_PORT, 1, 65535, CVAR_ARCHIVE | CVAR_EDITABLE, "UDP port for LAN hosting");
    N->snaprate = cvar_reg_int("sv_snaprate", 30, 5, 60, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "snapshots per second sent to clients");
    N->cl_interp = cvar_reg_float("cl_interp", 100.0f, 0.0f, 500.0f, CVAR_LOCKED, "remote entity interpolation delay (ms)");
    N->maxplayers = cvar_reg_int("sv_maxplayers", 4, 1, MAX_PLAYERS, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "maximum players when hosting");
    N->showstats = cvar_reg_bool("net_showstats", false, CVAR_ARCHIVE | CVAR_EDITABLE, "show network statistics overlay");
    N->relevance = cvar_reg_float("sv_relevance", 120.0f, 10.0f, 2000.0f, CVAR_ARCHIVE | CVAR_LOCKED | CVAR_RULE, "entity relevance distance (m)");
    N->interp = (InterpBuffer *)calloc(MAX_ENTITIES, sizeof(InterpBuffer));
    spectate_register();
    spectate_reset();
    spectate_set_sender(spec_send);
    snap_history_init(&N->cl_history);
    N->stats_time = platform_time();
    cmd_register("net_wait_clients", cmd_net_wait_clients, "net_wait_clients [n=1] [timeout frames=3600]: scripts wait until n clients are in (host)");
    cmd_register("net_snapstats", cmd_snapstats, "host: per-client snapshot kB/s since the last call, held zombie updates");
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
