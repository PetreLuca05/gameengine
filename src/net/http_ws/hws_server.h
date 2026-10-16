// Phone companion server: a small HTTPS (or HTTP) + WebSocket server on its own thread.
//
//   GET /                  the companion page (web/phone/index.html, held in memory)
//   GET /ws?t=<token>      WebSocket: pairs with a one-time token (QR code) ...
//   GET /ws?s=<session>    ... or resumes a paired session (page reload)
//   GET /media/<id>/<name>?s=<session>[&dl=1]   a photo / video registered by the game
//   GET /ca.crt            the local root certificate (DER), also on the plain HTTP helper port
// The helper port (http_port) serves /ca.crt and redirects everything else to the HTTPS port.
// No file system paths are reachable: only the page and the registered media list.
//
// Thread model: the server thread owns the sockets and TLS state; the game thread talks to it
// through hws_poll (incoming events) and hws_send (copies into the connection's queue). Every
// call is thread safe. See docs/PHONE.md.
#ifndef DG_HWS_SERVER_H
#define DG_HWS_SERVER_H

#include "core/base.h"
#include "hws_auth.h"
#include "hws_proto.h"

typedef struct HwsServer HwsServer;

typedef struct HwsConfig {
    u16 port;                 // main port (HTTPS, or HTTP when tls is off)
    u16 http_port;            // plain HTTP helper (CA download + redirect); 0 = none
    bool tls;
    u32 bind_ip;              // host order; 0 = all interfaces
    u32 lan_ip;               // advertised address (certificate SAN, URLs)
    bool allow_public;        // accept peers outside private / loopback ranges (default: refused)
    const char *cert_dir;     // certificate store (ends with a separator)
    const void *page;         // companion page bytes (copied)
    size_t page_len;
    u64 token_ttl_ms;         // pairing token lifetime (default 120 s)
    int max_message;          // largest accepted WebSocket message (default 64 KB)
    int max_conns;            // default 16
} HwsConfig;

typedef enum HwsEventType { HWS_EV_CONNECT, HWS_EV_DISCONNECT, HWS_EV_TEXT, HWS_EV_BINARY } HwsEventType;

typedef struct HwsEvent {
    int type;                 // HwsEventType
    int conn;                 // connection handle
    int owner;                // paired owner (local player slot)
    bool resumed;             // CONNECT: resumed a session (?s=) instead of a fresh pairing
    u32 peer_ip;
    u8 *data;                 // TEXT / BINARY payload (NUL terminated), freed by hws_event_free
    int len;
} HwsEvent;

typedef struct HwsStats {
    u64 bytes_in, bytes_out;
    u32 connections, ws_open, http_requests, tls_failures, auth_failures, throttled, refused_public;
    u32 dropped_frames;       // droppable frames replaced by newer ones (backpressure)
} HwsStats;

HwsServer *hws_start(const HwsConfig *cfg, char *err, size_t err_size);
void hws_stop(HwsServer *s);

bool hws_poll(HwsServer *s, HwsEvent *ev);
void hws_event_free(HwsEvent *ev);
// Queues a WebSocket message (copied). droppable: a newer droppable message on the same
// connection replaces this one if it hasn't started sending (live video frames).
bool hws_send(HwsServer *s, int conn, int opcode, const void *data, int len, bool droppable);
int hws_queued_bytes(HwsServer *s, int conn);   // not yet handed to the socket (-1 = gone)
void hws_close(HwsServer *s, int conn);

// Pairing (thread safe wrappers around hws_auth.h). issue: copies the token into out.
bool hws_issue_token(HwsServer *s, int owner, char out[HWS_KEY_CHARS + 1]);
void hws_unpair(HwsServer *s, int owner);       // revokes the session and closes its sockets
float hws_token_remaining(HwsServer *s, int owner);
bool hws_owner_paired(HwsServer *s, int owner);

// Media served at /media/<id>/<name> to the owner's session. Returns the id (> 0) or 0.
int hws_media_add(HwsServer *s, int owner, const char *path, const char *name);
void hws_media_remove(HwsServer *s, int id);

HwsStats hws_stats(HwsServer *s);
const char *hws_ca_fingerprint(HwsServer *s);   // "" without TLS
const HwsConfig *hws_config(HwsServer *s);
// The page URL for a token: scheme://lan_ip:port/?t=<token>
void hws_url(HwsServer *s, const char *token, char *out, size_t out_size);

// LAN addresses (host order) of this machine, best first (hws_ipv4_lan_rank, then the default
// route's interface). Returns the count.
int hws_lan_addresses(u32 *out, int max);

#endif
