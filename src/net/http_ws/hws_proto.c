// Pure HTTP / WebSocket protocol helpers (see hws_proto.h).
#include "hws_proto.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static bool ieq(const char *a, int alen, const char *b) {
    int bl = (int)strlen(b);
    if (alen != bl) return false;
    for (int i = 0; i < alen; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// ------------------------------------------------------------------ requests

int hws_parse_request(const char *buf, int len, HwsRequest *req) {
    memset(req, 0, sizeof(*req));
    // Find the end of the header block.
    int end = -1;
    for (int i = 3; i < len; i++)
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') { end = i + 1; break; }
    if (end < 0) return len >= HWS_MAX_REQUEST ? -1 : 0;
    if (end > HWS_MAX_REQUEST) return -1;
    req->header_bytes = end;
    // Request line: METHOD SP target SP HTTP/1.x CRLF
    int p = 0;
    req->method = buf;
    while (p < end && buf[p] != ' ') {
        if (!isupper((unsigned char)buf[p])) return -1;
        p++;
    }
    req->method_len = p;
    if (p == 0 || p >= end || p > 16) return -1;
    p++;
    int t0 = p;
    while (p < end && buf[p] != ' ' && buf[p] != '\r') {
        if ((unsigned char)buf[p] < 0x21 || (unsigned char)buf[p] > 0x7e) return -1;
        p++;
    }
    if (p >= end || buf[p] != ' ' || p == t0 || buf[t0] != '/') return -1;
    req->path = buf + t0;
    req->path_len = p - t0;
    for (int i = t0; i < p; i++)
        if (buf[i] == '?') {
            req->path_len = i - t0;
            req->query = buf + i + 1;
            req->query_len = p - i - 1;
            break;
        }
    if (!req->query) { req->query = buf + p; req->query_len = 0; }
    p++;
    if (end - p < 10 || memcmp(buf + p, "HTTP/1.", 7) != 0 || !isdigit((unsigned char)buf[p + 7])) return -1;
    req->version_minor = buf[p + 7] - '0';
    p += 8;
    if (buf[p] != '\r' || buf[p + 1] != '\n') return -1;
    p += 2;
    // Headers.
    while (p < end - 2) {
        int ls = p;
        while (p < end && buf[p] != '\r') p++;
        if (p + 1 >= end || buf[p + 1] != '\n') return -1;
        int le = p;
        p += 2;
        int colon = -1;
        for (int i = ls; i < le; i++)
            if (buf[i] == ':') { colon = i; break; }
        if (colon <= ls) return -1;   // no name (obsolete line folding is rejected too)
        for (int i = ls; i < colon; i++)
            if (buf[i] == ' ' || buf[i] == '\t') return -1;
        if (req->header_count >= HWS_MAX_HEADERS) return -1;
        int vs = colon + 1, ve = le;
        while (vs < ve && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
        while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;
        HwsHeader *h = &req->headers[req->header_count++];
        h->name = buf + ls;
        h->name_len = colon - ls;
        h->value = buf + vs;
        h->value_len = ve - vs;
    }
    return 1;
}

const char *hws_header(const HwsRequest *req, const char *name, int *len) {
    for (int i = 0; i < req->header_count; i++)
        if (ieq(req->headers[i].name, req->headers[i].name_len, name)) {
            if (len) *len = req->headers[i].value_len;
            return req->headers[i].value;
        }
    if (len) *len = 0;
    return NULL;
}

bool hws_header_has_token(const HwsRequest *req, const char *name, const char *token) {
    for (int i = 0; i < req->header_count; i++) {
        const HwsHeader *h = &req->headers[i];
        if (!ieq(h->name, h->name_len, name)) continue;
        int s = 0;
        while (s < h->value_len) {
            while (s < h->value_len && (h->value[s] == ' ' || h->value[s] == ',')) s++;
            int e = s;
            while (e < h->value_len && h->value[e] != ',') e++;
            int te = e;
            while (te > s && h->value[te - 1] == ' ') te--;
            if (te > s && ieq(h->value + s, te - s, token)) return true;
            s = e;
        }
    }
    return false;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hws_query_param(const char *query, int query_len, const char *key, char *out, size_t out_size) {
    int klen = (int)strlen(key);
    int p = 0;
    while (p < query_len) {
        int s = p;
        while (p < query_len && query[p] != '&') p++;
        int e = p;
        p++;
        int eq = s;
        while (eq < e && query[eq] != '=') eq++;
        if (eq - s != klen || memcmp(query + s, key, (size_t)klen) != 0) continue;
        size_t n = 0;
        for (int i = eq + 1; i < e; i++) {
            char c = query[i];
            if (c == '+') c = ' ';
            else if (c == '%') {
                if (i + 2 >= e) return false;
                int hi = hexval(query[i + 1]), lo = hexval(query[i + 2]);
                if (hi < 0 || lo < 0) return false;
                c = (char)(hi * 16 + lo);
                i += 2;
            }
            if (n + 1 >= out_size) return false;
            out[n++] = c;
        }
        if (out_size) out[n] = 0;
        return true;
    }
    return false;
}

bool hws_path_is(const HwsRequest *req, const char *path) {
    int n = (int)strlen(path);
    return req->path_len == n && memcmp(req->path, path, (size_t)n) == 0;
}

// ------------------------------------------------------------------ SHA-1 / base64

typedef struct Sha1 { u32 h[5]; u64 len; u8 buf[64]; int n; } Sha1;

static u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(Sha1 *s, const u8 *b) {
    u32 w[80];
    for (int i = 0; i < 16; i++) w[i] = (u32)b[i * 4] << 24 | (u32)b[i * 4 + 1] << 16 | (u32)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20) { f = (bb & c) | (~bb & d); k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = bb ^ c ^ d; k = 0xCA62C1D6; }
        u32 t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void hws_sha1(const void *data, size_t len, u8 out[20]) {
    Sha1 s = { { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 }, 0, { 0 }, 0 };
    const u8 *p = (const u8 *)data;
    s.len = (u64)len * 8;
    while (len >= 64) { sha1_block(&s, p); p += 64; len -= 64; }
    memcpy(s.buf, p, len);
    s.n = (int)len;
    s.buf[s.n++] = 0x80;
    if (s.n > 56) {
        memset(s.buf + s.n, 0, (size_t)(64 - s.n));
        sha1_block(&s, s.buf);
        s.n = 0;
    }
    memset(s.buf + s.n, 0, (size_t)(56 - s.n));
    for (int i = 0; i < 8; i++) s.buf[56 + i] = (u8)(s.len >> (56 - 8 * i));
    sha1_block(&s, s.buf);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (u8)(s.h[i] >> 24);
        out[i * 4 + 1] = (u8)(s.h[i] >> 16);
        out[i * 4 + 2] = (u8)(s.h[i] >> 8);
        out[i * 4 + 3] = (u8)s.h[i];
    }
}

int hws_base64_encode(const u8 *src, int len, char *dst, int dst_size) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int need = (len + 2) / 3 * 4 + 1;
    if (dst_size < need) { if (dst_size) dst[0] = 0; return 0; }
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        u32 v = (u32)src[i] << 16 | (i + 1 < len ? (u32)src[i + 1] << 8 : 0) | (i + 2 < len ? src[i + 2] : 0);
        dst[o++] = T[(v >> 18) & 63];
        dst[o++] = T[(v >> 12) & 63];
        dst[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
        dst[o++] = i + 2 < len ? T[v & 63] : '=';
    }
    dst[o] = 0;
    return o;
}

void hws_ws_accept_key(const char *key, int key_len, char out[29]) {
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char tmp[128];
    if (key_len < 0 || key_len > 64) key_len = 0;
    memcpy(tmp, key, (size_t)key_len);
    memcpy(tmp + key_len, GUID, sizeof(GUID) - 1);
    u8 dig[20];
    hws_sha1(tmp, (size_t)key_len + sizeof(GUID) - 1, dig);
    hws_base64_encode(dig, 20, out, 29);
}

bool hws_is_ws_upgrade(const HwsRequest *req) {
    if (req->method_len != 3 || memcmp(req->method, "GET", 3) != 0 || req->version_minor < 1) return false;
    if (!hws_header_has_token(req, "Upgrade", "websocket") || !hws_header_has_token(req, "Connection", "Upgrade")) return false;
    int vl;
    const char *v = hws_header(req, "Sec-WebSocket-Version", &vl);
    if (!v || vl != 2 || memcmp(v, "13", 2) != 0) return false;
    int kl;
    const char *k = hws_header(req, "Sec-WebSocket-Key", &kl);
    return k && kl == 24;   // base64 of 16 bytes
}

// ------------------------------------------------------------------ frames

int hws_ws_frame_header(u8 out[10], int opcode, bool fin, u64 n) {
    out[0] = (u8)((fin ? 0x80 : 0) | (opcode & 15));
    if (n < 126) { out[1] = (u8)n; return 2; }
    if (n <= 0xffff) {
        out[1] = 126;
        out[2] = (u8)(n >> 8);
        out[3] = (u8)n;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (u8)(n >> (56 - 8 * i));
    return 10;
}

void hws_ws_decoder_init(HwsWsDecoder *d, u8 *storage, int max_msg) {
    memset(d, 0, sizeof(*d));
    d->msg = storage;
    d->max_msg = max_msg;
    d->hdr_need = 2;
}

void hws_ws_decoder_next(HwsWsDecoder *d) {
    if (!d->in_fragmented) d->msg_len = 0;
    d->ctrl_len = 0;
    d->hdr_have = 0;
    d->hdr_need = 2;
    d->payload_len = d->payload_have = 0;
}

HwsWsResult hws_ws_decode(HwsWsDecoder *d, const u8 *data, int len, int *used) {
    int p = 0;
    *used = 0;
    for (;;) {
        // Header.
        while (d->hdr_have < d->hdr_need) {
            if (p >= len) { *used = p; return HWS_WS_NEED_MORE; }
            d->hdr[d->hdr_have++] = data[p++];
            if (d->hdr_have == 2) {
                d->fin = (d->hdr[0] & 0x80) != 0;
                d->opcode = d->hdr[0] & 15;
                d->masked = (d->hdr[1] & 0x80) != 0;
                int l7 = d->hdr[1] & 127;
                if (d->hdr[0] & 0x70) { *used = p; return HWS_WS_ERROR; }   // no extensions negotiated
                if (!d->masked) { *used = p; return HWS_WS_ERROR; }
                d->hdr_need = 2 + (l7 == 126 ? 2 : (l7 == 127 ? 8 : 0)) + 4;
            }
        }
        if (d->payload_have == 0 && d->hdr_need > 0) {
            int l7 = d->hdr[1] & 127, o = 2;
            u64 n = (u64)l7;
            if (l7 == 126) { n = (u64)d->hdr[2] << 8 | d->hdr[3]; o = 4; }
            else if (l7 == 127) {
                n = 0;
                for (int i = 0; i < 8; i++) n = n << 8 | d->hdr[2 + i];
                o = 10;
            }
            memcpy(d->mask, d->hdr + o, 4);
            d->payload_len = n;
            bool ctrl = d->opcode >= 8;
            if (ctrl && (n > 125 || !d->fin)) { *used = p; return HWS_WS_ERROR; }
            if (!ctrl) {
                if (d->opcode == HWS_OP_CONT && !d->in_fragmented) { *used = p; return HWS_WS_ERROR; }
                if (d->opcode != HWS_OP_CONT && d->in_fragmented) { *used = p; return HWS_WS_ERROR; }
                if (d->opcode != HWS_OP_CONT && d->opcode != HWS_OP_TEXT && d->opcode != HWS_OP_BINARY) { *used = p; return HWS_WS_ERROR; }
                if ((u64)d->msg_len + n > (u64)d->max_msg) { *used = p; return HWS_WS_ERROR; }
                if (d->opcode != HWS_OP_CONT) d->msg_opcode = d->opcode;
            } else if (d->opcode != HWS_OP_CLOSE && d->opcode != HWS_OP_PING && d->opcode != HWS_OP_PONG) {
                *used = p;
                return HWS_WS_ERROR;
            }
            d->hdr_need = -1;   // header done marker (payload_have counts from here)
        }
        // Payload.
        bool ctrl = d->opcode >= 8;
        while (d->payload_have < d->payload_len) {
            if (p >= len) { *used = p; return HWS_WS_NEED_MORE; }
            u64 take = d->payload_len - d->payload_have;
            if (take > (u64)(len - p)) take = (u64)(len - p);
            for (u64 i = 0; i < take; i++) {
                u8 b = data[p + (int)i] ^ d->mask[(d->payload_have + i) & 3];
                if (ctrl) d->ctrl[d->ctrl_len++] = b;
                else d->msg[d->msg_len++] = b;
            }
            p += (int)take;
            d->payload_have += take;
        }
        *used = p;
        if (ctrl) return HWS_WS_CONTROL;
        if (d->fin) {
            d->in_fragmented = false;
            return HWS_WS_MESSAGE;
        }
        d->in_fragmented = true;
        // Next fragment.
        d->hdr_have = 0;
        d->hdr_need = 2;
        d->payload_len = d->payload_have = 0;
    }
}

int hws_ws_client_frame(u8 *out, int out_size, int opcode, const void *payload, int len, u32 mask_key) {
    u8 h[10];
    int hl = hws_ws_frame_header(h, opcode, true, (u64)len);
    if (hl + 4 + len > out_size) return -1;
    memcpy(out, h, (size_t)hl);
    out[1] |= 0x80;
    u8 m[4] = { (u8)(mask_key >> 24), (u8)(mask_key >> 16), (u8)(mask_key >> 8), (u8)mask_key };
    memcpy(out + hl, m, 4);
    const u8 *s = (const u8 *)payload;
    for (int i = 0; i < len; i++) out[hl + 4 + i] = s[i] ^ m[i & 3];
    return hl + 4 + len;
}

// ------------------------------------------------------------------ addresses

bool hws_ipv4_parse(const char *s, u32 *out) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*s)) return false;
        int n = 0, digits = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (*s++ - '0');
            if (++digits > 3 || n > 255) return false;
        }
        v = v << 8 | (u32)n;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *out = v;
    return true;
}

void hws_ipv4_format(u32 ip, char *out, size_t out_size) {
    snprintf(out, out_size, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

bool hws_ipv4_is_loopback(u32 ip) { return (ip >> 24) == 127; }

bool hws_ipv4_is_private(u32 ip) {
    return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1) || (ip >> 16) == ((192u << 8) | 168) || (ip >> 16) == ((169u << 8) | 254);
}

int hws_ipv4_lan_rank(u32 ip) {
    if ((ip >> 16) == ((192u << 8) | 168)) return 4;
    if ((ip >> 24) == 10) return 3;
    if ((ip >> 20) == ((172u << 4) | 1)) return 2;
    if ((ip >> 16) == ((169u << 8) | 254)) return 1;
    return 0;
}

const char *hws_mime_for(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *mime; } M[] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" },
        { ".mp4", "video/mp4" }, { ".crt", "application/x-x509-ca-cert" }, { ".cer", "application/x-x509-ca-cert" },
        { ".svg", "image/svg+xml" }, { ".webmanifest", "application/manifest+json" },
    };
    for (int i = 0; i < ARRAY_COUNT(M); i++) {
        size_t n = strlen(M[i].ext);
        if (strlen(dot) == n) {
            bool eq = true;
            for (size_t k = 0; k < n; k++)
                if (tolower((unsigned char)dot[k]) != M[i].ext[k]) eq = false;
            if (eq) return M[i].mime;
        }
    }
    return "application/octet-stream";
}
