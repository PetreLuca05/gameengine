// Transport abstraction: one interface, two implementations (LAN/UDP via ENet, Steam
// Networking Sockets). The session layer (net.c) only talks to this.
#ifndef DG_NET_TRANSPORT_H
#define DG_NET_TRANSPORT_H

#include "core/base.h"

typedef int PeerId;

enum { CH_UNRELIABLE = 0, CH_RELIABLE = 1, CH_COUNT = 2 };

typedef enum NetEventType { NETEV_NONE, NETEV_CONNECT, NETEV_DISCONNECT, NETEV_DATA } NetEventType;

typedef struct NetEvent {
    NetEventType type;
    PeerId peer;
    int channel;
    const u8 *data;   // valid until the next poll()
    u32 size;
} NetEvent;

typedef struct NetTransport NetTransport;
struct NetTransport {
    const char *name;
    void *impl;
    bool (*listen)(NetTransport *t, u16 port, int max_peers);
    bool (*connect)(NetTransport *t, const char *address);      // async: NETEV_CONNECT on success
    bool (*send)(NetTransport *t, PeerId peer, int channel, const void *data, u32 size);
    int (*poll)(NetTransport *t, NetEvent *out, int max);
    void (*disconnect)(NetTransport *t, PeerId peer);
    void (*flush)(NetTransport *t);
    bool (*peer_stats)(NetTransport *t, PeerId peer, float *rtt_ms, float *loss_pct);
    // Who a peer is: "ip" (LAN) or the SteamID64 (Steam). Lobby kick bans and host migration.
    bool (*peer_address)(NetTransport *t, PeerId peer, char *out, int size);
    void (*destroy)(NetTransport *t);
};

NetTransport *transport_lan_create(void);
NetTransport *transport_steam_create(void);    // NULL when built without Steam / Steam not running
// Wraps a transport with simulated latency/loss (net_fakelag, net_fakeloss cvars).
NetTransport *transport_conditioner_wrap(NetTransport *inner);

#endif
