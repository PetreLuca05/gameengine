// LAN / direct-IP transport over ENet (reliable + unreliable channels, fragmentation).
#include "net_transport.h"
#include "core/log.h"
#include "core/cvar.h"
#include <enet/enet.h>
#include <stdlib.h>
#include <stdio.h>

#define LAN_MAX_PEERS 16
#define MAX_HELD_PACKETS 256

typedef struct LanImpl {
    ENetHost *host;
    ENetPeer *peers[LAN_MAX_PEERS];
    ENetPacket *held[MAX_HELD_PACKETS];   // received packets alive until next poll
    int held_count;
    bool is_server;
} LanImpl;

static int peer_slot(LanImpl *l, ENetPeer *p) {
    for (int i = 0; i < LAN_MAX_PEERS; i++) if (l->peers[i] == p) return i;
    for (int i = 0; i < LAN_MAX_PEERS; i++) if (!l->peers[i]) { l->peers[i] = p; return i; }
    return -1;
}

static bool lan_listen(NetTransport *t, u16 port, int max_peers) {
    LanImpl *l = (LanImpl *)t->impl;
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    l->host = enet_host_create(&addr, (size_t)DG_MIN(max_peers, LAN_MAX_PEERS), CH_COUNT, 0, 0);
    if (!l->host) { log_warn("lan: UDP port %u is busy (another host on this machine?) - not accepting LAN players", port); return false; }
    l->is_server = true;
    log_info("lan: listening on UDP port %u", port);
    return true;
}

static bool lan_connect(NetTransport *t, const char *address) {
    LanImpl *l = (LanImpl *)t->impl;
    char host[128];
    dg_strcpy(host, sizeof(host), address);
    // No port given: our own net_port (the same port both ends listen on by default).
    const Cvar *pc = cvar_find("net_port");
    u16 port = pc && pc->i > 0 ? (u16)pc->i : 27015;
    char *colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (u16)atoi(colon + 1); }
    ENetAddress addr;
    if (enet_address_set_host(&addr, host[0] ? host : "127.0.0.1") != 0) { log_error("lan: cannot resolve '%s'", host); return false; }
    addr.port = port;
    l->host = enet_host_create(NULL, 1, CH_COUNT, 0, 0);
    if (!l->host) { log_error("lan: cannot create client host"); return false; }
    ENetPeer *p = enet_host_connect(l->host, &addr, CH_COUNT, 0);
    if (!p) { log_error("lan: connect failed"); return false; }
    enet_peer_timeout(p, 0, 4000, 8000);
    l->peers[0] = p;
    log_info("lan: connecting to %s:%u", host, port);
    return true;
}

static bool lan_send(NetTransport *t, PeerId peer, int channel, const void *data, u32 size) {
    LanImpl *l = (LanImpl *)t->impl;
    if (peer < 0 || peer >= LAN_MAX_PEERS || !l->peers[peer]) return false;
    u32 flags = channel == CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
    ENetPacket *pkt = enet_packet_create(data, size, flags);
    return enet_peer_send(l->peers[peer], (enet_uint8)channel, pkt) == 0;
}

static int lan_poll(NetTransport *t, NetEvent *out, int max) {
    LanImpl *l = (LanImpl *)t->impl;
    for (int i = 0; i < l->held_count; i++) enet_packet_destroy(l->held[i]);
    l->held_count = 0;
    if (!l->host) return 0;
    int n = 0;
    ENetEvent ev;
    while (n < max && l->held_count < MAX_HELD_PACKETS && enet_host_service(l->host, &ev, 0) > 0) {
        int slot = peer_slot(l, ev.peer);
        if (slot < 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
            continue;
        }
        NetEvent *e = &out[n];
        memset(e, 0, sizeof(*e));
        e->peer = slot;
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            e->type = NETEV_CONNECT;
            enet_peer_timeout(ev.peer, 0, 4000, 8000);
            n++;
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            e->type = NETEV_DISCONNECT;
            l->peers[slot] = NULL;
            n++;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            e->type = NETEV_DATA;
            e->channel = ev.channelID;
            e->data = ev.packet->data;
            e->size = (u32)ev.packet->dataLength;
            l->held[l->held_count++] = ev.packet;
            n++;
            break;
        default: break;
        }
    }
    return n;
}

static void lan_disconnect(NetTransport *t, PeerId peer) {
    LanImpl *l = (LanImpl *)t->impl;
    if (peer < 0 || peer >= LAN_MAX_PEERS || !l->peers[peer]) return;
    enet_peer_disconnect_later(l->peers[peer], 0);
}

static void lan_flush(NetTransport *t) {
    LanImpl *l = (LanImpl *)t->impl;
    if (l->host) enet_host_flush(l->host);
}

static bool lan_stats(NetTransport *t, PeerId peer, float *rtt, float *loss) {
    LanImpl *l = (LanImpl *)t->impl;
    if (peer < 0 || peer >= LAN_MAX_PEERS || !l->peers[peer]) return false;
    *rtt = (float)l->peers[peer]->roundTripTime;
    *loss = (float)l->peers[peer]->packetLoss / (float)ENET_PEER_PACKET_LOSS_SCALE * 100.0f;
    return true;
}

static bool lan_address(NetTransport *t, PeerId peer, char *out, int size) {
    LanImpl *l = (LanImpl *)t->impl;
    if (peer < 0 || peer >= LAN_MAX_PEERS || !l->peers[peer]) return false;
    return enet_address_get_host_ip(&l->peers[peer]->address, out, (size_t)size) == 0;
}

static void lan_destroy(NetTransport *t) {
    LanImpl *l = (LanImpl *)t->impl;
    for (int i = 0; i < l->held_count; i++) enet_packet_destroy(l->held[i]);
    if (l->host) {
        for (int i = 0; i < LAN_MAX_PEERS; i++) if (l->peers[i]) enet_peer_disconnect_now(l->peers[i], 0);
        enet_host_flush(l->host);
        enet_host_destroy(l->host);
    }
    free(l);
    free(t);
}

NetTransport *transport_lan_create(void) {
    NetTransport *t = (NetTransport *)calloc(1, sizeof(NetTransport));
    LanImpl *l = (LanImpl *)calloc(1, sizeof(LanImpl));
    t->name = "LAN (ENet)";
    t->impl = l;
    t->listen = lan_listen;
    t->connect = lan_connect;
    t->send = lan_send;
    t->poll = lan_poll;
    t->disconnect = lan_disconnect;
    t->flush = lan_flush;
    t->peer_stats = lan_stats;
    t->peer_address = lan_address;
    t->destroy = lan_destroy;
    return t;
}
