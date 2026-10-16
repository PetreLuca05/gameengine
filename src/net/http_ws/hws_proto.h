// Pure HTTP/1.1 + WebSocket (RFC 6455) protocol pieces of the phone companion server: request
// parsing, the upgrade handshake key, frame encode / incremental decode, query strings, base64,
// address classification. No sockets, no allocation beyond what the caller passes in, so every
// piece is unit tested (tests/test_phone.c). The server is src/net/http_ws/hws_server.c.
#ifndef DG_HWS_PROTO_H
#define DG_HWS_PROTO_H

#include "core/base.h"

#define HWS_MAX_HEADERS 32
#define HWS_MAX_REQUEST 8192        // request line + headers (bytes); larger requests are rejected

typedef struct HwsHeader { const char *name, *value; int name_len, value_len; } HwsHeader;

// A parsed request; every string points into the caller's buffer (not NUL terminated, see *_len).
typedef struct HwsRequest {
    const char *method, *path, *query;   // path without the query; query without '?' (may be empty)
    int method_len, path_len, query_len;
    int version_minor;                    // HTTP/1.x
    HwsHeader headers[HWS_MAX_HEADERS];
    int header_count;
    int header_bytes;                     // bytes consumed by the request line + headers + CRLFCRLF
} HwsRequest;

// 1 = complete (req filled), 0 = need more bytes, -1 = malformed / too large.
int hws_parse_request(const char *buf, int len, HwsRequest *req);
// Header value by case-insensitive name (NULL if absent); *len receives its length.
const char *hws_header(const HwsRequest *req, const char *name, int *len);
// True if a comma separated header contains the token (case-insensitive), e.g. Connection: Upgrade.
bool hws_header_has_token(const HwsRequest *req, const char *name, const char *token);
// Query parameter value copied (percent-decoded) into out; false if absent.
bool hws_query_param(const char *query, int query_len, const char *key, char *out, size_t out_size);
bool hws_path_is(const HwsRequest *req, const char *path);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(sha1(key + GUID)), 28 chars + NUL.
void hws_ws_accept_key(const char *key, int key_len, char out[29]);
// True if the request is a valid WebSocket upgrade (GET, Upgrade: websocket, Connection: Upgrade,
// version 13, 16-byte key).
bool hws_is_ws_upgrade(const HwsRequest *req);

int hws_base64_encode(const u8 *src, int len, char *dst, int dst_size);   // returns chars written (NUL terminated)
void hws_sha1(const void *data, size_t len, u8 out[20]);

// ------------------------------------------------------------------ WebSocket frames
enum { HWS_OP_CONT = 0, HWS_OP_TEXT = 1, HWS_OP_BINARY = 2, HWS_OP_CLOSE = 8, HWS_OP_PING = 9, HWS_OP_PONG = 10 };

// Server -> client frame header (never masked). Returns the header size (2, 4 or 10 bytes).
int hws_ws_frame_header(u8 out[10], int opcode, bool fin, u64 payload_len);

// Incremental client -> server frame decoder. Feed bytes; complete messages (fragments joined,
// payload unmasked) come out one at a time. Client frames must be masked (RFC 6455 5.1).
typedef struct HwsWsDecoder {
    u8 *msg;            // reassembled message (caller-owned storage, max_msg bytes)
    int max_msg;
    int msg_len;
    int msg_opcode;     // of the first fragment
    bool in_fragmented;
    // current frame
    u8 hdr[14];
    int hdr_have, hdr_need;
    u64 payload_len, payload_have;
    int opcode;
    bool fin, masked;
    u8 mask[4];
    // control frames are kept apart (they may arrive between fragments)
    u8 ctrl[125];
    int ctrl_len;
} HwsWsDecoder;

typedef enum HwsWsResult { HWS_WS_NEED_MORE, HWS_WS_MESSAGE, HWS_WS_CONTROL, HWS_WS_ERROR } HwsWsResult;

void hws_ws_decoder_init(HwsWsDecoder *d, u8 *storage, int max_msg);
// Consumes bytes from data[0..len) and stops at the first complete message (or control frame).
// *used receives the bytes consumed. On HWS_WS_MESSAGE: d->msg / msg_len / msg_opcode; on
// HWS_WS_CONTROL: d->opcode, d->ctrl / ctrl_len. Call hws_ws_decoder_next() before feeding more.
HwsWsResult hws_ws_decode(HwsWsDecoder *d, const u8 *data, int len, int *used);
void hws_ws_decoder_next(HwsWsDecoder *d);
// Client-side helper (tests, tools): a masked client frame. Returns bytes written or -1.
int hws_ws_client_frame(u8 *out, int out_size, int opcode, const void *payload, int len, u32 mask_key);

// ------------------------------------------------------------------ addresses
// IPv4 in host byte order: a.b.c.d = (a << 24) | ...
bool hws_ipv4_parse(const char *s, u32 *out);
void hws_ipv4_format(u32 ip, char *out, size_t out_size);
bool hws_ipv4_is_private(u32 ip);    // 10/8, 172.16/12, 192.168/16, 169.254/16 (link local)
bool hws_ipv4_is_loopback(u32 ip);   // 127/8
// Ranks LAN interface candidates for the QR URL: 192.168 > 10 > 172.16 > link local; 0 = unusable.
int hws_ipv4_lan_rank(u32 ip);

// Content type for a file name by extension ("application/octet-stream" if unknown).
const char *hws_mime_for(const char *name);

#endif
