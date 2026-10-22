// Dual transport (docs/MENU_LOBBY.md "Party"): a host listens on Steam (P2P relay) and the LAN (ENet)
// at once, so friends join through Steam and players on the local network by LAN discovery / IP, in
// the same game. Peers interleave: Steam's slot k is 2k, the LAN's slot k is 2k + 1. A side that fails
// to listen (Steam not running, the LAN port taken) leaves the other one hosting.
#include "net_transport.h"
#include "core/log.h"
#include <stdlib.h>

typedef struct DualImpl {
    NetTransport *t[2];   // 0 Steam, 1 LAN (NULL: that side is off)
    bool on[2];
} DualImpl;

static NetTransport *side(NetTransport *t, PeerId peer, PeerId *inner) {
    DualImpl *d = (DualImpl *)t->impl;
    int k = peer & 1;
    *inner = peer >> 1;
    return peer >= 0 && d->on[k] ? d->t[k] : NULL;
}

static bool dual_listen(NetTransport *t, u16 port, int max_peers) {
    DualImpl *d = (DualImpl *)t->impl;
    for (int k = 0; k < 2; k++) {
        d->on[k] = d->t[k] && d->t[k]->listen(d->t[k], port, max_peers);
        if (d->t[k] && !d->on[k]) log_warn("net: %s side not listening; hosting on the other only", d->t[k]->name);
    }
    return d->on[0] || d->on[1];
}

static bool dual_connect(NetTransport *t, const char *address) {
    (void)t;
    (void)address;
    return false;   // hosting only: a client connects through one transport
}

static bool dual_send(NetTransport *t, PeerId peer, int channel, const void *data, u32 size) {
    PeerId in;
    NetTransport *s = side(t, peer, &in);
    return s && s->send(s, in, channel, data, size);
}

static int dual_poll(NetTransport *t, NetEvent *out, int max) {
    DualImpl *d = (DualImpl *)t->impl;
    int n = 0;
    for (int k = 0; k < 2 && n < max; k++) {
        if (!d->on[k]) continue;
        int got = d->t[k]->poll(d->t[k], out + n, max - n);
        for (int i = 0; i < got; i++) out[n + i].peer = out[n + i].peer * 2 + k;
        n += got;
    }
    return n;
}

static void dual_disconnect(NetTransport *t, PeerId peer) {
    PeerId in;
    NetTransport *s = side(t, peer, &in);
    if (s) s->disconnect(s, in);
}

static void dual_flush(NetTransport *t) {
    DualImpl *d = (DualImpl *)t->impl;
    for (int k = 0; k < 2; k++) if (d->on[k]) d->t[k]->flush(d->t[k]);
}

static bool dual_stats(NetTransport *t, PeerId peer, float *rtt, float *loss) {
    PeerId in;
    NetTransport *s = side(t, peer, &in);
    return s && s->peer_stats && s->peer_stats(s, in, rtt, loss);
}

static bool dual_address(NetTransport *t, PeerId peer, char *out, int size) {
    PeerId in;
    NetTransport *s = side(t, peer, &in);
    return s && s->peer_address && s->peer_address(s, in, out, size);
}

static void dual_timeout(NetTransport *t, PeerId peer, u32 ms) {
    PeerId in;
    NetTransport *s = side(t, peer, &in);
    if (s && s->set_peer_timeout) s->set_peer_timeout(s, in, ms);
}

static void dual_destroy(NetTransport *t) {
    DualImpl *d = (DualImpl *)t->impl;
    for (int k = 0; k < 2; k++) if (d->t[k]) d->t[k]->destroy(d->t[k]);
    free(d);
    free(t);
}

NetTransport *transport_dual_create(NetTransport *steam, NetTransport *lan) {
    if (!steam) return lan;
    if (!lan) return steam;
    NetTransport *t = (NetTransport *)calloc(1, sizeof(NetTransport));
    DualImpl *d = (DualImpl *)calloc(1, sizeof(DualImpl));
    if (!t || !d) { free(t); free(d); steam->destroy(steam); return lan; }
    d->t[0] = steam;
    d->t[1] = lan;
    t->name = "Steam P2P + LAN (ENet)";
    t->impl = d;
    t->listen = dual_listen;
    t->connect = dual_connect;
    t->send = dual_send;
    t->poll = dual_poll;
    t->disconnect = dual_disconnect;
    t->flush = dual_flush;
    t->peer_stats = dual_stats;
    t->peer_address = dual_address;
    t->set_peer_timeout = dual_timeout;
    t->destroy = dual_destroy;
    return t;
}
