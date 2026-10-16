// Network conditioner: adds artificial latency / jitter / unreliable loss on outgoing packets.
// Applies to whichever transport it wraps, so LAN and Steam behave the same under test.
#include "net_transport.h"
#include "core/engine.h"
#include "core/cvar.h"
#include "core/platform.h"
#include <stdlib.h>

#define COND_QUEUE 4096
#define COND_MAX_PACKET 65536

typedef struct DelayedPacket {
    double release;
    PeerId peer;
    int channel;
    u32 size;
    u8 *data;
} DelayedPacket;

typedef struct CondImpl {
    NetTransport *inner;
    DelayedPacket queue[COND_QUEUE];
    int count;
    Cvar *lag, *jitter, *loss;
    Rng rng;
} CondImpl;

static void release_due(CondImpl *c, bool all) {
    double now = platform_time();
    int keep = 0;
    for (int i = 0; i < c->count; i++) {
        DelayedPacket *p = &c->queue[i];
        if (all || p->release <= now) {
            c->inner->send(c->inner, p->peer, p->channel, p->data, p->size);
            free(p->data);
        } else {
            c->queue[keep++] = *p;
        }
    }
    c->count = keep;
}

static bool cond_listen(NetTransport *t, u16 port, int max) { CondImpl *c = t->impl; return c->inner->listen(c->inner, port, max); }
static bool cond_connect(NetTransport *t, const char *a) { CondImpl *c = t->impl; return c->inner->connect(c->inner, a); }

static bool cond_send(NetTransport *t, PeerId peer, int channel, const void *data, u32 size) {
    CondImpl *c = (CondImpl *)t->impl;
    float lag = c->lag->f, loss = c->loss->f;
    if (channel == CH_UNRELIABLE && loss > 0 && rng_float(&c->rng) * 100.0f < loss) return true; // dropped
    if (lag <= 0 && c->jitter->f <= 0) return c->inner->send(c->inner, peer, channel, data, size);
    if (c->count >= COND_QUEUE || size > COND_MAX_PACKET) return c->inner->send(c->inner, peer, channel, data, size);
    DelayedPacket *p = &c->queue[c->count++];
    float jitter = c->jitter->f > 0 ? rng_range(&c->rng, 0, c->jitter->f) : 0;
    // Reliable packets must stay in order, so they don't get jitter.
    if (channel == CH_RELIABLE) jitter = 0;
    p->release = platform_time() + (lag + jitter) / 1000.0;
    p->peer = peer;
    p->channel = channel;
    p->size = size;
    p->data = (u8 *)malloc(size);
    memcpy(p->data, data, size);
    return true;
}

static int cond_poll(NetTransport *t, NetEvent *out, int max) {
    CondImpl *c = (CondImpl *)t->impl;
    release_due(c, false);
    return c->inner->poll(c->inner, out, max);
}

static void cond_disconnect(NetTransport *t, PeerId peer) { CondImpl *c = t->impl; c->inner->disconnect(c->inner, peer); }

static void cond_flush(NetTransport *t) {
    CondImpl *c = (CondImpl *)t->impl;
    release_due(c, false);
    c->inner->flush(c->inner);
}

static bool cond_stats(NetTransport *t, PeerId peer, float *rtt, float *loss) {
    CondImpl *c = t->impl;
    return c->inner->peer_stats(c->inner, peer, rtt, loss);
}

static bool cond_address(NetTransport *t, PeerId peer, char *out, int size) {
    CondImpl *c = t->impl;
    return c->inner->peer_address && c->inner->peer_address(c->inner, peer, out, size);
}

static void cond_destroy(NetTransport *t) {
    CondImpl *c = (CondImpl *)t->impl;
    release_due(c, true);
    c->inner->destroy(c->inner);
    free(c);
    free(t);
}

NetTransport *transport_conditioner_wrap(NetTransport *inner) {
    if (!inner) return NULL;
    NetTransport *t = (NetTransport *)calloc(1, sizeof(NetTransport));
    CondImpl *c = (CondImpl *)calloc(1, sizeof(CondImpl));
    c->inner = inner;
    c->lag = cvar_reg_float("net_fakelag", 0, 0, 2000, 0, "artificial outgoing latency (ms)");
    c->jitter = cvar_reg_float("net_fakejitter", 0, 0, 500, 0, "artificial jitter on unreliable packets (ms)");
    c->loss = cvar_reg_float("net_fakeloss", 0, 0, 100, 0, "artificial unreliable packet loss (%)");
    c->rng = rng_make((u64)(platform_time() * 1e6));
    t->name = inner->name;
    t->impl = c;
    t->listen = cond_listen;
    t->connect = cond_connect;
    t->send = cond_send;
    t->poll = cond_poll;
    t->disconnect = cond_disconnect;
    t->flush = cond_flush;
    t->peer_stats = cond_stats;
    t->peer_address = cond_address;
    t->destroy = cond_destroy;
    return t;
}
