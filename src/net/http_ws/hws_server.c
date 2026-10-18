// Phone companion server (see hws_server.h): non-blocking sockets + select() on one thread,
// optional TLS (hws_tls.c), HTTP/1.1 requests (one per connection), WebSocket sessions.
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // POSIX / BSD socket and time declarations under -std=c11
#endif
#include "core/base.h"
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static bool sock_would_block(void) { int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static bool sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR; }
#endif

#include "hws_server.h"
#include "hws_tls.h"
#include "core/log.h"
#include "core/platform.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CONNS 32
#define MAX_EVENTS 512
#define MAX_MEDIA 256
#define IN_BUF (HWS_MAX_REQUEST + 4096)
#define HTTP_IDLE_MS 10000u
#define WS_PING_MS 5000u
#define WS_IDLE_MS 20000u
#define FILE_CHUNK 65536
#define FILE_QUEUE_LIMIT (256 * 1024)

typedef struct OutChunk {
    struct OutChunk *next;
    int len, off;
    bool droppable;
    u8 data[];
} OutChunk;

typedef enum ConnState { CS_FREE, CS_TLS, CS_HTTP, CS_WS, CS_DRAIN } ConnState;

typedef struct Conn {
    ConnState state;
    sock_t fd;
    u32 ip;
    u16 serial;                 // handle = slot | serial << 8
    bool helper;                // accepted on the plain HTTP helper port
    HwsTlsConn *tls;
    u8 in[IN_BUF];
    int in_len;
    HwsWsDecoder dec;
    u8 *msg;
    OutChunk *out_head, *out_tail;
    int out_bytes;
    bool close_after;           // close once the queue drains
    int owner;
    u32 generation;
    u64 last_rx_ms, last_ping_ms, opened_ms;
    FILE *file;                 // media download in progress
    u64 file_left;
} Conn;

typedef struct Media { int id, owner; char path[260], name[96]; } Media;

struct HwsServer {
    HwsConfig cfg;
    u8 *page;
    HwsTls *tls;
    sock_t listen_fd, helper_fd;
    SDL_Thread *thread;
    SDL_Mutex *mu;
    SDL_AtomicInt quit;
    Conn conns[MAX_CONNS];
    HwsEvent events[MAX_EVENTS];
    int ev_head, ev_count;
    HwsAuth auth;
    Media media[MAX_MEDIA];
    int media_next;
    HwsStats stats;
};

static u64 now_ms(void) { return SDL_GetTicks(); }

static int conn_handle(const HwsServer *s, const Conn *c) { return (int)(c - s->conns) | (int)c->serial << 8; }

static Conn *conn_get(HwsServer *s, int h) {
    int slot = h & 0xff;
    if (h < 0 || slot >= MAX_CONNS) return NULL;
    Conn *c = &s->conns[slot];
    return c->state != CS_FREE && c->serial == (u16)(h >> 8) ? c : NULL;
}

static void set_nonblocking(sock_t fd) {
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

// ------------------------------------------------------------------ events

static void push_event(HwsServer *s, int type, Conn *c, const u8 *data, int len) {
    if (s->ev_count >= MAX_EVENTS) {
        // Full (the game stopped polling): drop the oldest data event.
        HwsEvent *old = &s->events[s->ev_head];
        hws_event_free(old);
        s->ev_head = (s->ev_head + 1) % MAX_EVENTS;
        s->ev_count--;
    }
    HwsEvent *e = &s->events[(s->ev_head + s->ev_count++) % MAX_EVENTS];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->conn = conn_handle(s, c);
    e->owner = c->owner;
    e->peer_ip = c->ip;
    if (data && len >= 0) {
        e->data = (u8 *)malloc((size_t)len + 1);
        if (e->data) {
            memcpy(e->data, data, (size_t)len);
            e->data[len] = 0;
            e->len = len;
        }
    }
}

bool hws_poll(HwsServer *s, HwsEvent *ev) {
    if (!s) return false;
    SDL_LockMutex(s->mu);
    bool any = s->ev_count > 0;
    if (any) {
        *ev = s->events[s->ev_head];
        memset(&s->events[s->ev_head], 0, sizeof(HwsEvent));
        s->ev_head = (s->ev_head + 1) % MAX_EVENTS;
        s->ev_count--;
    }
    SDL_UnlockMutex(s->mu);
    return any;
}

void hws_event_free(HwsEvent *ev) {
    free(ev->data);
    ev->data = NULL;
    ev->len = 0;
}

// ------------------------------------------------------------------ output queue

static bool queue_raw(HwsServer *s, Conn *c, const void *a, int alen, const void *b, int blen, bool droppable) {
    if (droppable) {
        // Replace a droppable chunk that hasn't started sending (keep only the newest frame).
        OutChunk *prev = NULL;
        for (OutChunk *k = c->out_head; k; prev = k, k = k->next) {
            if (!k->droppable || k->off != 0) continue;
            if (prev) prev->next = k->next;
            else c->out_head = k->next;
            if (c->out_tail == k) c->out_tail = prev;
            c->out_bytes -= k->len;
            free(k);
            s->stats.dropped_frames++;
            break;
        }
    }
    OutChunk *k = (OutChunk *)malloc(sizeof(OutChunk) + (size_t)alen + (size_t)blen);
    if (!k) return false;
    k->next = NULL;
    k->len = alen + blen;
    k->off = 0;
    k->droppable = droppable;
    if (alen) memcpy(k->data, a, (size_t)alen);
    if (blen) memcpy(k->data + alen, b, (size_t)blen);
    if (c->out_tail) c->out_tail->next = k;
    else c->out_head = k;
    c->out_tail = k;
    c->out_bytes += k->len;
    return true;
}

static bool queue_ws(HwsServer *s, Conn *c, int opcode, const void *data, int len, bool droppable) {
    u8 h[10];
    int hl = hws_ws_frame_header(h, opcode, true, (u64)len);
    return queue_raw(s, c, h, hl, data, len, droppable);
}

static void free_queue(Conn *c) {
    while (c->out_head) {
        OutChunk *n = c->out_head->next;
        free(c->out_head);
        c->out_head = n;
    }
    c->out_tail = NULL;
    c->out_bytes = 0;
}

// ------------------------------------------------------------------ raw / TLS I/O

static int io_send(void *ctx, u8 *buf, size_t len) {
    Conn *c = (Conn *)ctx;
#if defined(_WIN32)
    int n = send(c->fd, (const char *)buf, (int)len, 0);
#elif defined(MSG_NOSIGNAL)
    int n = (int)send(c->fd, buf, len, MSG_NOSIGNAL);
#else
    int n = (int)send(c->fd, buf, len, 0);   // macOS: no MSG_NOSIGNAL, SO_NOSIGPIPE on the socket (accept_on)
#endif
    if (n >= 0) return n == 0 ? -1 : n;
    return sock_would_block() ? 0 : -1;
}

static int io_recv(void *ctx, u8 *buf, size_t len) {
    Conn *c = (Conn *)ctx;
#if defined(_WIN32)
    int n = recv(c->fd, (char *)buf, (int)len, 0);
#else
    int n = (int)recv(c->fd, buf, len, 0);
#endif
    if (n > 0) return n;
    if (n == 0) return -1;   // orderly close
    return sock_would_block() ? 0 : -1;
}

static int conn_read(HwsServer *s, Conn *c, u8 *buf, int len) {
    int n = c->tls ? hws_tls_read(c->tls, buf, (size_t)len) : io_recv(c, buf, (size_t)len);
    if (n > 0) s->stats.bytes_in += (u64)n;
    return n;
}

static int conn_write(HwsServer *s, Conn *c, const u8 *buf, int len) {
    int n = c->tls ? hws_tls_write(c->tls, buf, (size_t)len) : io_send(c, (u8 *)buf, (size_t)len);
    if (n > 0) s->stats.bytes_out += (u64)n;
    return n;
}

static void conn_free(HwsServer *s, Conn *c, bool notify) {
    if (c->state == CS_FREE) return;
    if (notify && c->state == CS_WS) push_event(s, HWS_EV_DISCONNECT, c, NULL, -1);
    if (c->state == CS_WS) s->stats.ws_open--;
    hws_tls_conn_free(c->tls);
    c->tls = NULL;
    if (c->fd != SOCK_BAD) sock_close(c->fd);
    c->fd = SOCK_BAD;
    free_queue(c);
    free(c->msg);
    c->msg = NULL;
    if (c->file) fclose(c->file);
    c->file = NULL;
    u16 serial = c->serial;
    memset(c, 0, sizeof(*c));
    c->fd = SOCK_BAD;
    c->serial = (u16)(serial + 1);
    c->state = CS_FREE;
}

// Flushes as much of the queue as the socket takes. false = connection broken.
static bool flush(HwsServer *s, Conn *c) {
    while (c->out_head) {
        OutChunk *k = c->out_head;
        // mbedTLS needs the same buffer on a retry after WANT_WRITE: write the chunk remainder,
        // at most one 16 KB record at a time.
        int want = k->len - k->off;
        if (want > 16384) want = 16384;
        int n = conn_write(s, c, k->data + k->off, want);
        if (n < 0) return false;
        if (n == 0) return true;
        k->off += n;
        c->out_bytes -= n;
        if (k->off >= k->len) {
            c->out_head = k->next;
            if (!c->out_head) c->out_tail = NULL;
            free(k);
        }
    }
    // Media download: refill while the queue is short.
    while (c->file && c->out_bytes < FILE_QUEUE_LIMIT && c->file_left > 0) {
        u8 buf[FILE_CHUNK];
        size_t want = c->file_left < FILE_CHUNK ? (size_t)c->file_left : FILE_CHUNK;
        size_t got = fread(buf, 1, want, c->file);
        if (got == 0) { c->file_left = 0; break; }
        queue_raw(s, c, buf, (int)got, NULL, 0, false);
        c->file_left -= got;
        return flush(s, c);
    }
    if (c->file && c->file_left == 0) {
        fclose(c->file);
        c->file = NULL;
    }
    return true;
}

// ------------------------------------------------------------------ HTTP responses

static void respond(HwsServer *s, Conn *c, int code, const char *status, const char *type, const void *body, size_t len, const char *extra) {
    char h[1024];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n"
                     "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s\r\n",
                     code, status, type, len, extra ? extra : "");
    queue_raw(s, c, h, n, body, (int)len, false);
    c->close_after = true;
    c->state = CS_DRAIN;
}

static void respond_text(HwsServer *s, Conn *c, int code, const char *status, const char *text) {
    respond(s, c, code, status, "text/plain; charset=utf-8", text, strlen(text), NULL);
}

static void client_host(HwsServer *s, char *out, size_t n, u16 port) {
    char ip[20];
    hws_ipv4_format(s->cfg.lan_ip, ip, sizeof(ip));
    snprintf(out, n, "%s:%u", ip, (unsigned)port);
}

static void serve_media(HwsServer *s, Conn *c, const HwsRequest *rq) {
    // /media/<id>/<name>
    int id = atoi(rq->path + 7);
    char sess[64] = { 0 }, dl[8] = { 0 };
    hws_query_param(rq->query, rq->query_len, "s", sess, sizeof(sess));
    hws_query_param(rq->query, rq->query_len, "dl", dl, sizeof(dl));
    int owner = hws_auth_session(&s->auth, sess);
    if (owner < 0) {
        hws_rate_fail(&s->auth, c->ip, now_ms());
        s->stats.auth_failures++;
        respond_text(s, c, 403, "Forbidden", "not paired");
        return;
    }
    Media *m = NULL;
    for (int i = 0; i < MAX_MEDIA; i++)
        if (s->media[i].id == id && id > 0 && s->media[i].owner == owner) m = &s->media[i];
    FILE *f = m ? fopen(m->path, "rb") : NULL;
    if (!f) { respond_text(s, c, 404, "Not Found", "no such media"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char h[1024];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\nCache-Control: private, max-age=3600\r\n"
                     "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n%s%s%s\r\n",
                     hws_mime_for(m->name), size, dl[0] == '1' ? "Content-Disposition: attachment; filename=\"" : "", dl[0] == '1' ? m->name : "",
                     dl[0] == '1' ? "\"\r\n" : "");
    queue_raw(s, c, h, n, NULL, 0, false);
    c->file = f;
    c->file_left = size > 0 ? (u64)size : 0;
    c->close_after = true;
    c->state = CS_DRAIN;
}

static void ws_accept(HwsServer *s, Conn *c, const HwsRequest *rq) {
    char tok[64] = { 0 }, sess[64] = { 0 };
    bool has_t = hws_query_param(rq->query, rq->query_len, "t", tok, sizeof(tok));
    bool has_s = hws_query_param(rq->query, rq->query_len, "s", sess, sizeof(sess));
    u64 now = now_ms();
    int owner = -1;
    bool resumed = false;
    char session[HWS_KEY_CHARS + 1] = { 0 };
    const char *why = "missing token";
    if (has_s && (owner = hws_auth_session(&s->auth, sess)) >= 0) {
        resumed = true;
        memcpy(session, s->auth.owner[owner].session, sizeof(session));
    } else if (has_t) {
        u8 rnd[16];
        hws_random(rnd, sizeof(rnd));
        HwsAuthResult r = hws_auth_pair(&s->auth, tok, now, rnd, &owner, session);
        if (r != HWS_AUTH_OK) owner = -1;
        why = r == HWS_AUTH_EXPIRED ? "token expired: show a new QR code in game" : "invalid token";
    } else if (has_s) {
        why = "session ended: scan the QR code again";
    }
    if (owner < 0) {
        hws_rate_fail(&s->auth, c->ip, now);
        s->stats.auth_failures++;
        respond_text(s, c, 403, "Forbidden", why);
        return;
    }
    // One phone per owner: a fresh pairing (or a resume) replaces the owner's other sockets.
    u32 gen = s->auth.owner[owner].generation;
    for (int i = 0; i < MAX_CONNS; i++) {
        Conn *o = &s->conns[i];
        if (o != c && o->state == CS_WS && o->owner == owner) {
            static const u8 bye[] = { 0x0f, 0xa0 };   // 4000: replaced
            queue_ws(s, o, HWS_OP_CLOSE, bye, 2, false);
            o->close_after = true;
            o->state = CS_DRAIN;
            push_event(s, HWS_EV_DISCONNECT, o, NULL, -1);
            s->stats.ws_open--;
        }
    }
    int kl;
    const char *key = hws_header(rq, "Sec-WebSocket-Key", &kl);
    char acc[29];
    hws_ws_accept_key(key, kl, acc);
    char h[512];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    queue_raw(s, c, h, n, NULL, 0, false);
    c->state = CS_WS;
    c->owner = owner;
    c->generation = gen;
    c->msg = (u8 *)malloc((size_t)s->cfg.max_message);
    hws_ws_decoder_init(&c->dec, c->msg, s->cfg.max_message);
    c->last_ping_ms = now;
    s->stats.ws_open++;
    // The session key goes to the page first (sessionStorage: reconnects / media downloads).
    char hello[160];
    int hl = snprintf(hello, sizeof(hello), "{\"t\":\"session\",\"s\":\"%s\",\"owner\":%d,\"resumed\":%s}", session, owner, resumed ? "true" : "false");
    queue_ws(s, c, HWS_OP_TEXT, hello, hl, false);
    push_event(s, HWS_EV_CONNECT, c, NULL, -1);
    s->events[(s->ev_head + s->ev_count - 1) % MAX_EVENTS].resumed = resumed;
    // Bytes after the request (a pipelined first frame) feed the decoder below.
    memmove(c->in, c->in + rq->header_bytes, (size_t)(c->in_len - rq->header_bytes));
    c->in_len -= rq->header_bytes;
}

static void handle_request(HwsServer *s, Conn *c, const HwsRequest *rq) {
    s->stats.http_requests++;
    bool get = rq->method_len == 3 && memcmp(rq->method, "GET", 3) == 0;
    bool head = rq->method_len == 4 && memcmp(rq->method, "HEAD", 4) == 0;
    if (!get && !head) { respond_text(s, c, 405, "Method Not Allowed", "GET only"); return; }
    if (hws_path_is(rq, "/ca.crt") || hws_path_is(rq, "/ca.cer")) {
        size_t n = 0;
        const u8 *der = hws_tls_ca_der(s->tls, &n);
        if (!der) { respond_text(s, c, 404, "Not Found", "TLS is off"); return; }
        respond(s, c, 200, "OK", "application/x-x509-ca-cert", der, n, "Content-Disposition: attachment; filename=\"deadground-ca.crt\"\r\n");
        return;
    }
    if (c->helper) {
        // Plain HTTP helper: everything else goes to the secure page (path + query kept).
        char host[64], loc[600];
        client_host(s, host, sizeof(host), s->cfg.port);
        snprintf(loc, sizeof(loc), "Location: https://%s%.*s%s%.*s\r\n", host, rq->path_len, rq->path, rq->query_len ? "?" : "", rq->query_len, rq->query);
        respond(s, c, 302, "Found", "text/plain; charset=utf-8", "", 0, loc);
        return;
    }
    if (hws_path_is(rq, "/ws")) {
        if (!hws_is_ws_upgrade(rq)) { respond_text(s, c, 400, "Bad Request", "websocket upgrade expected"); return; }
        ws_accept(s, c, rq);
        return;
    }
    if (hws_path_is(rq, "/") || hws_path_is(rq, "/index.html")) {
        // The page reads ?t= itself; it is the only document served. CSP: no third-party loads.
        respond(s, c, 200, "OK", "text/html; charset=utf-8", head ? NULL : s->page, head ? 0 : s->cfg.page_len,
                "Content-Security-Policy: default-src 'self'; img-src 'self' blob: data:; media-src 'self' blob:; style-src 'self' 'unsafe-inline'; "
                "script-src 'self' 'unsafe-inline'; connect-src 'self' wss: ws:\r\n");
        return;
    }
    if (rq->path_len > 7 && memcmp(rq->path, "/media/", 7) == 0) { serve_media(s, c, rq); return; }
    if (hws_path_is(rq, "/favicon.ico")) { respond(s, c, 204, "No Content", "text/plain", NULL, 0, NULL); return; }
    respond_text(s, c, 404, "Not Found", "not found");
}

// ------------------------------------------------------------------ connection service

static void service_ws_input(HwsServer *s, Conn *c) {
    int off = 0;
    while (off < c->in_len && c->state == CS_WS) {
        int used = 0;
        HwsWsResult r = hws_ws_decode(&c->dec, c->in + off, c->in_len - off, &used);
        off += used;
        if (r == HWS_WS_NEED_MORE) break;
        if (r == HWS_WS_ERROR) {
            static const u8 err[] = { 0x03, 0xea };   // 1002 protocol error
            queue_ws(s, c, HWS_OP_CLOSE, err, 2, false);
            push_event(s, HWS_EV_DISCONNECT, c, NULL, -1);
            s->stats.ws_open--;
            c->state = CS_DRAIN;
            c->close_after = true;
            break;
        }
        if (r == HWS_WS_CONTROL) {
            if (c->dec.opcode == HWS_OP_PING) queue_ws(s, c, HWS_OP_PONG, c->dec.ctrl, c->dec.ctrl_len, false);
            else if (c->dec.opcode == HWS_OP_CLOSE) {
                queue_ws(s, c, HWS_OP_CLOSE, c->dec.ctrl, c->dec.ctrl_len >= 2 ? 2 : 0, false);
                push_event(s, HWS_EV_DISCONNECT, c, NULL, -1);
                s->stats.ws_open--;
                c->state = CS_DRAIN;
                c->close_after = true;
            }
        } else {
            push_event(s, c->dec.msg_opcode == HWS_OP_TEXT ? HWS_EV_TEXT : HWS_EV_BINARY, c, c->dec.msg, c->dec.msg_len);
        }
        hws_ws_decoder_next(&c->dec);
    }
    memmove(c->in, c->in + off, (size_t)(c->in_len - off));
    c->in_len -= off;
}

// One step for a connection that is readable (or has buffered TLS data). false = drop it.
static bool service_read(HwsServer *s, Conn *c) {
    u64 now = now_ms();
    if (c->state == CS_TLS) {
        int r = hws_tls_handshake(c->tls);
        if (r < 0) { s->stats.tls_failures++; return false; }
        if (r == 0) return true;
        c->state = CS_HTTP;
    }
    for (int guard = 0; guard < 64; guard++) {
        if (c->state == CS_DRAIN) return true;
        int space = IN_BUF - c->in_len;
        if (space <= 0) return false;
        int n = conn_read(s, c, c->in + c->in_len, space);
        if (n < 0) return false;
        if (n == 0) break;
        c->in_len += n;
        c->last_rx_ms = now;
        if (c->state == CS_HTTP) {
            HwsRequest rq;
            int pr = hws_parse_request((const char *)c->in, c->in_len, &rq);
            if (pr < 0) { respond_text(s, c, 400, "Bad Request", "malformed request"); return true; }
            if (pr == 1) handle_request(s, c, &rq);
        }
        if (c->state == CS_WS) service_ws_input(s, c);
    }
    return true;
}

static void accept_on(HwsServer *s, sock_t lfd, bool helper) {
    for (int k = 0; k < 8; k++) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        sock_t fd = accept(lfd, (struct sockaddr *)&a, &al);
        if (fd == SOCK_BAD) return;
#ifdef SO_NOSIGPIPE
        {   // a phone closing mid-response must not SIGPIPE the game (Darwin has no MSG_NOSIGNAL)
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
        }
#endif
        u32 ip = ntohl(a.sin_addr.s_addr);
        s->stats.connections++;
        if (!s->cfg.allow_public && !hws_ipv4_is_private(ip) && !hws_ipv4_is_loopback(ip)) {
            s->stats.refused_public++;
            sock_close(fd);
            continue;
        }
        if (!hws_rate_allow(&s->auth, ip, now_ms())) {
            s->stats.throttled++;
            sock_close(fd);
            continue;
        }
        Conn *c = NULL;
        int open = 0;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (s->conns[i].state != CS_FREE) open++;
            else if (!c) c = &s->conns[i];
        }
        if (!c || open >= s->cfg.max_conns) { sock_close(fd); continue; }
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
        int sndbuf = 512 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));
        c->fd = fd;
        c->ip = ip;
        c->helper = helper;
        c->owner = -1;
        c->opened_ms = c->last_rx_ms = now_ms();
        c->state = CS_HTTP;
        if (s->tls && !helper) {
            c->tls = hws_tls_conn_new(s->tls, io_send, io_recv, c);
            if (!c->tls) { conn_free(s, c, false); continue; }
            c->state = CS_TLS;
        }
    }
}

static sock_t open_listener(u32 ip, u16 port, char *err, size_t err_size) {
    sock_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == SOCK_BAD) { snprintf(err, err_size, "socket() failed"); return SOCK_BAD; }
#if !defined(_WIN32)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 16) != 0) {
        char ips[20];
        hws_ipv4_format(ip, ips, sizeof(ips));
        snprintf(err, err_size, "cannot listen on %s:%u (port in use?)", ips, (unsigned)port);
        sock_close(fd);
        return SOCK_BAD;
    }
    set_nonblocking(fd);
    return fd;
}

static int server_thread(void *user) {
    HwsServer *s = (HwsServer *)user;
    while (!SDL_GetAtomicInt(&s->quit)) {
        fd_set rd, wr;
        FD_ZERO(&rd);
        FD_ZERO(&wr);
        sock_t maxfd = s->listen_fd;
        FD_SET(s->listen_fd, &rd);
        if (s->helper_fd != SOCK_BAD) {
            FD_SET(s->helper_fd, &rd);
            if (s->helper_fd > maxfd) maxfd = s->helper_fd;
        }
        bool tls_pending = false;
        SDL_LockMutex(s->mu);
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn *c = &s->conns[i];
            if (c->state == CS_FREE) continue;
            FD_SET(c->fd, &rd);
            if (c->out_head || c->file || c->state == CS_TLS) FD_SET(c->fd, &wr);
            if (c->fd > maxfd) maxfd = c->fd;
            if (c->tls && hws_tls_pending(c->tls)) tls_pending = true;
        }
        SDL_UnlockMutex(s->mu);
        struct timeval tv = { 0, tls_pending ? 0 : 4000 };   // 4 ms: queued game frames go out promptly
        int nsel = select((int)maxfd + 1, &rd, &wr, NULL, &tv);
        if (nsel < 0) {
            platform_sleep_ms(2);
            continue;
        }
        SDL_LockMutex(s->mu);
        if (FD_ISSET(s->listen_fd, &rd)) accept_on(s, s->listen_fd, false);
        if (s->helper_fd != SOCK_BAD && FD_ISSET(s->helper_fd, &rd)) accept_on(s, s->helper_fd, true);
        u64 now = now_ms();
        for (int i = 0; i < MAX_CONNS; i++) {
            Conn *c = &s->conns[i];
            if (c->state == CS_FREE) continue;
            bool ok = true;
            bool readable = FD_ISSET(c->fd, &rd) || (c->tls && hws_tls_pending(c->tls)) || (c->state == CS_TLS && FD_ISSET(c->fd, &wr));
            if (readable) ok = service_read(s, c);
            if (ok && c->state != CS_TLS) ok = flush(s, c);
            if (ok && c->state == CS_WS) {
                if (now - c->last_ping_ms > WS_PING_MS) {
                    queue_ws(s, c, HWS_OP_PING, "dg", 2, false);
                    c->last_ping_ms = now;
                }
                if (now - c->last_rx_ms > WS_IDLE_MS) {
                    log_info("phone: connection timed out");
                    ok = false;
                }
            }
            if (ok && (c->state == CS_HTTP || c->state == CS_TLS) && now - c->opened_ms > HTTP_IDLE_MS) ok = false;
            if (ok && c->state == CS_DRAIN && c->close_after && !c->out_head && !c->file) ok = false;
            if (ok && c->state == CS_DRAIN && now - c->last_rx_ms > 120000u) ok = false;   // stuck downloads
            if (!ok) conn_free(s, c, true);
        }
        SDL_UnlockMutex(s->mu);
    }
    return 0;
}

// ------------------------------------------------------------------ public API

HwsServer *hws_start(const HwsConfig *cfg, char *err, size_t err_size) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // reference counted (ENet initializes it too)
#endif
    HwsServer *s = (HwsServer *)calloc(1, sizeof(HwsServer));
    if (!s) return NULL;
    s->cfg = *cfg;
    if (!s->cfg.max_message) s->cfg.max_message = 64 * 1024;
    if (!s->cfg.max_conns || s->cfg.max_conns > MAX_CONNS) s->cfg.max_conns = 16;
    s->listen_fd = s->helper_fd = SOCK_BAD;
    for (int i = 0; i < MAX_CONNS; i++) s->conns[i].fd = SOCK_BAD;
    hws_auth_init(&s->auth, cfg->token_ttl_ms);
    s->page = (u8 *)malloc(cfg->page_len + 1);
    if (cfg->page_len) memcpy(s->page, cfg->page, cfg->page_len);
    s->media_next = 1;
    if (cfg->tls) {
        s->tls = hws_tls_create(cfg->cert_dir ? cfg->cert_dir : "", cfg->lan_ip, err, err_size);
        if (!s->tls) { hws_stop(s); return NULL; }
    }
    s->listen_fd = open_listener(cfg->bind_ip, cfg->port, err, err_size);
    if (s->listen_fd == SOCK_BAD) { hws_stop(s); return NULL; }
    if (cfg->http_port && cfg->tls) {
        char e2[128];
        s->helper_fd = open_listener(cfg->bind_ip, cfg->http_port, e2, sizeof(e2));
        if (s->helper_fd == SOCK_BAD) log_warn("phone: helper port: %s", e2);
    }
    s->mu = SDL_CreateMutex();
    SDL_SetAtomicInt(&s->quit, 0);
    s->thread = SDL_CreateThread(server_thread, "phone_server", s);
    if (!s->thread) { snprintf(err, err_size, "thread: %s", SDL_GetError()); hws_stop(s); return NULL; }
    return s;
}

void hws_stop(HwsServer *s) {
    if (!s) return;
    if (s->thread) {
        SDL_SetAtomicInt(&s->quit, 1);
        SDL_WaitThread(s->thread, NULL);
    }
    for (int i = 0; i < MAX_CONNS; i++) conn_free(s, &s->conns[i], false);
    if (s->listen_fd != SOCK_BAD) sock_close(s->listen_fd);
    if (s->helper_fd != SOCK_BAD) sock_close(s->helper_fd);
    for (int i = 0; i < MAX_EVENTS; i++) hws_event_free(&s->events[i]);
    hws_tls_destroy(s->tls);
    if (s->mu) SDL_DestroyMutex(s->mu);
    free(s->page);
    free(s);
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool hws_send(HwsServer *s, int conn, int opcode, const void *data, int len, bool droppable) {
    if (!s) return false;
    SDL_LockMutex(s->mu);
    Conn *c = conn_get(s, conn);
    bool ok = c && c->state == CS_WS && queue_ws(s, c, opcode, data, len, droppable);
    SDL_UnlockMutex(s->mu);
    return ok;
}

int hws_queued_bytes(HwsServer *s, int conn) {
    if (!s) return -1;
    SDL_LockMutex(s->mu);
    Conn *c = conn_get(s, conn);
    int n = c && c->state == CS_WS ? c->out_bytes : -1;
    SDL_UnlockMutex(s->mu);
    return n;
}

void hws_close(HwsServer *s, int conn) {
    if (!s) return;
    SDL_LockMutex(s->mu);
    Conn *c = conn_get(s, conn);
    if (c && c->state == CS_WS) {
        static const u8 bye[] = { 0x03, 0xe8 };   // 1000 normal
        queue_ws(s, c, HWS_OP_CLOSE, bye, 2, false);
        c->state = CS_DRAIN;
        c->close_after = true;
        s->stats.ws_open--;
    }
    SDL_UnlockMutex(s->mu);
}

bool hws_issue_token(HwsServer *s, int owner, char out[HWS_KEY_CHARS + 1]) {
    if (!s) return false;
    u8 rnd[16];
    if (!hws_random(rnd, sizeof(rnd))) return false;
    SDL_LockMutex(s->mu);
    const char *t = hws_auth_issue(&s->auth, owner, rnd, now_ms());
    if (t) memcpy(out, t, HWS_KEY_CHARS + 1);
    SDL_UnlockMutex(s->mu);
    return t != NULL;
}

void hws_unpair(HwsServer *s, int owner) {
    if (!s) return;
    SDL_LockMutex(s->mu);
    hws_auth_unpair(&s->auth, owner);
    for (int i = 0; i < MAX_CONNS; i++) {
        Conn *c = &s->conns[i];
        if (c->state == CS_WS && c->owner == owner) {
            static const u8 bye[] = { 0x0f, 0xa1 };   // 4001: unpaired
            queue_ws(s, c, HWS_OP_TEXT, "{\"t\":\"unpaired\"}", 16, false);
            queue_ws(s, c, HWS_OP_CLOSE, bye, 2, false);
            c->state = CS_DRAIN;
            c->close_after = true;
            s->stats.ws_open--;
            push_event(s, HWS_EV_DISCONNECT, c, NULL, -1);
        }
    }
    SDL_UnlockMutex(s->mu);
}

float hws_token_remaining(HwsServer *s, int owner) {
    if (!s) return 0;
    SDL_LockMutex(s->mu);
    float r = hws_auth_token_remaining(&s->auth, owner, now_ms());
    SDL_UnlockMutex(s->mu);
    return r;
}

bool hws_owner_paired(HwsServer *s, int owner) {
    if (!s || owner < 0 || owner >= HWS_MAX_OWNERS) return false;
    SDL_LockMutex(s->mu);
    bool p = s->auth.owner[owner].paired;
    SDL_UnlockMutex(s->mu);
    return p;
}

int hws_media_add(HwsServer *s, int owner, const char *path, const char *name) {
    if (!s) return 0;
    SDL_LockMutex(s->mu);
    Media *m = NULL;
    for (int i = 0; i < MAX_MEDIA && !m; i++)
        if (s->media[i].id == 0) m = &s->media[i];
    if (!m) {   // full: recycle the oldest
        m = &s->media[0];
        for (int i = 1; i < MAX_MEDIA; i++)
            if (s->media[i].id < m->id) m = &s->media[i];
    }
    m->id = s->media_next++;
    m->owner = owner;
    dg_strcpy(m->path, sizeof(m->path), path);
    dg_strcpy(m->name, sizeof(m->name), name);
    int id = m->id;
    SDL_UnlockMutex(s->mu);
    return id;
}

void hws_media_remove(HwsServer *s, int id) {
    if (!s) return;
    SDL_LockMutex(s->mu);
    for (int i = 0; i < MAX_MEDIA; i++)
        if (s->media[i].id == id) memset(&s->media[i], 0, sizeof(Media));
    SDL_UnlockMutex(s->mu);
}

HwsStats hws_stats(HwsServer *s) {
    HwsStats st;
    memset(&st, 0, sizeof(st));
    if (!s) return st;
    SDL_LockMutex(s->mu);
    st = s->stats;
    SDL_UnlockMutex(s->mu);
    return st;
}

const char *hws_ca_fingerprint(HwsServer *s) { return s ? hws_tls_fingerprint(s->tls) : ""; }
const HwsConfig *hws_config(HwsServer *s) { return s ? &s->cfg : NULL; }

void hws_url(HwsServer *s, const char *token, char *out, size_t out_size) {
    char host[64];
    client_host(s, host, sizeof(host), s->cfg.port);
    snprintf(out, out_size, "%s://%s/?t=%s", s->tls ? "https" : "http", host, token ? token : "");
}

// ------------------------------------------------------------------ LAN addresses

// The interface the default route uses: a UDP socket "connected" to a public address (nothing is
// sent) reports its local address.
static u32 default_route_ip(void) {
    sock_t fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == SOCK_BAD) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808u);
    u32 ip = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        struct sockaddr_in l;
        socklen_t ll = sizeof(l);
        if (getsockname(fd, (struct sockaddr *)&l, &ll) == 0) ip = ntohl(l.sin_addr.s_addr);
    }
    sock_close(fd);
    return ip;
}

int hws_lan_addresses(u32 *out, int max) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    u32 cand[32];
    int n = 0;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    IP_ADAPTER_ADDRESSES *aa = (IP_ADAPTER_ADDRESSES *)malloc(size);
    if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &size) == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *p = aa; p && n < 32; p = p->Next) {
            if (p->OperStatus != IfOperStatusUp || p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u && n < 32; u = u->Next)
                if (u->Address.lpSockaddr->sa_family == AF_INET) cand[n++] = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
        }
    }
    free(aa);
#else
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *p = ifs; p && n < 32; p = p->ifa_next)
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && (p->ifa_flags & IFF_UP) && !(p->ifa_flags & IFF_LOOPBACK))
                cand[n++] = ntohl(((struct sockaddr_in *)p->ifa_addr)->sin_addr.s_addr);
        freeifaddrs(ifs);
    }
#endif
    u32 def = default_route_ip();
#if defined(_WIN32)
    WSACleanup();
#endif
    // Usable (private) only, ranked: the default route's interface first, then by range.
    int m = 0;
    for (int i = 0; i < n && m < max; i++) {
        if (hws_ipv4_lan_rank(cand[i]) <= 0) continue;
        bool dup = false;
        for (int k = 0; k < m; k++) dup |= out[k] == cand[i];
        if (!dup) out[m++] = cand[i];
    }
    for (int i = 1; i < m; i++)
        for (int k = i; k > 0; k--) {
            int ra = hws_ipv4_lan_rank(out[k]) + (out[k] == def ? 10 : 0), rb = hws_ipv4_lan_rank(out[k - 1]) + (out[k - 1] == def ? 10 : 0);
            if (ra <= rb) break;
            u32 t = out[k];
            out[k] = out[k - 1];
            out[k - 1] = t;
        }
    return m;
}
