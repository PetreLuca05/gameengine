// JPEG frames over the paired WebSocket (the baseline PhoneStreamSink, see phone_stream.h).
// push() copies the frame into a free slot and encodes it on a worker; the worker hands the
// message to the server queue itself (droppable: a newer frame replaces one still waiting), so
// the render thread never waits on the encoder. Three slots: at 30 fps and ~10 ms per encode at
// 640 x 1136 one is normally enough, the others absorb hitches.
#include "phone_stream.h"
#include "phone_proto.h"
#include "core/jobs.h"
#include "core/dgmath.h"
#include "net/http_ws/hws_server.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

#define SLOTS 3
#define HIST 64

typedef struct Slot {
    SDL_AtomicInt state;      // 0 free, 1 encoding, 2 done (stats not harvested yet)
    struct WsSink *owner;
    u8 *rgba;
    int cap, w, h, ow, oh, quality;
    PhoneFrameInfo info;
    PhoneBuf jpeg;
    double encode_ms;
    int bytes;
    bool sent;
} Slot;

typedef struct WsSink {
    PhoneStreamSink base;
    HwsServer *server;
    SDL_AtomicInt conn;
    PhoneRate rate;
    Slot slots[SLOTS];
    struct { u32 id, send_ms, capture_ms; } hist[HIST];
    PhoneStreamStats st;
    u32 sec_start, sec_frames, sec_bytes;
    double enc_sum;
    int enc_n, last_bytes;
    float page_rtt;
} WsSink;

static void encode_job(void *user) {
    Slot *s = (Slot *)user;
    WsSink *k = s->owner;
    double t0 = SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
    s->jpeg.len = 0;
    u8 hdr[PHONE_FRAME_HEADER];
    phone_write_frame_header(hdr, s->info.flags, (u16)s->ow, (u16)s->oh, (u16)s->quality, s->info.frame_id, s->info.capture_ms, s->info.zoom);
    phone_buf_append(&s->jpeg, hdr, PHONE_FRAME_HEADER);
    PhoneBuf body = { 0 };
    bool ok = phone_jpeg_encode(s->rgba, s->w, s->h, s->w * 4, s->ow, s->oh, s->quality, &body);
    if (ok) phone_buf_append(&s->jpeg, body.data, body.len);
    phone_buf_free(&body);
    s->encode_ms = (SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency() - t0) * 1000.0;
    s->bytes = s->jpeg.len;
    int conn = SDL_GetAtomicInt(&k->conn);
    s->sent = ok && conn >= 0 && hws_send(k->server, conn, HWS_OP_BINARY, s->jpeg.data, s->jpeg.len, true);
    SDL_SetAtomicInt(&s->state, 2);
}

static void harvest(WsSink *k) {
    for (int i = 0; i < SLOTS; i++) {
        Slot *s = &k->slots[i];
        if (SDL_GetAtomicInt(&s->state) != 2) continue;
        k->enc_sum += s->encode_ms;
        k->enc_n++;
        if (s->sent) {
            k->st.frames_sent++;
            k->sec_frames++;
            k->sec_bytes += (u32)s->bytes;
            k->last_bytes = s->bytes;
        }
        SDL_SetAtomicInt(&s->state, 0);
    }
}

static bool ws_push(PhoneStreamSink *base, const u8 *rgba, int w, int h, const PhoneFrameInfo *info) {
    WsSink *k = (WsSink *)base;
    harvest(k);
    k->st.frames_offered++;
    int conn = SDL_GetAtomicInt(&k->conn);
    u32 now = (u32)SDL_GetTicks();
    int queued = conn >= 0 ? hws_queued_bytes(k->server, conn) : -1;
    if (!phone_rate_should_send(&k->rate, now, queued, k->last_bytes)) {
        if (queued >= 0) k->st.frames_skipped++;
        return false;
    }
    Slot *s = NULL;
    for (int i = 0; i < SLOTS && !s; i++)
        if (SDL_GetAtomicInt(&k->slots[i].state) == 0) s = &k->slots[i];
    if (!s) { k->st.frames_skipped++; k->rate.congestion++; return false; }
    int need = w * h * 4;
    if (s->cap < need) {
        free(s->rgba);
        s->rgba = (u8 *)malloc((size_t)need);
        s->cap = s->rgba ? need : 0;
        if (!s->rgba) return false;
    }
    memcpy(s->rgba, rgba, (size_t)need);
    s->w = w;
    s->h = h;
    s->ow = DG_MAX(16, (int)lrintf((float)w * k->rate.scale)) & ~1;
    s->oh = DG_MAX(16, (int)lrintf((float)h * k->rate.scale)) & ~1;
    s->quality = k->rate.quality;
    s->info = *info;
    s->owner = k;
    k->hist[info->frame_id % HIST].id = info->frame_id;
    k->hist[info->frame_id % HIST].send_ms = now;
    k->hist[info->frame_id % HIST].capture_ms = info->capture_ms;
    phone_rate_sent(&k->rate, info->frame_id, now);
    SDL_SetAtomicInt(&s->state, 1);
    jobs_submit(encode_job, NULL, s, NULL);
    return true;
}

static void ws_on_ack(PhoneStreamSink *base, u32 frame_id, u32 now_ms) {
    WsSink *k = (WsSink *)base;
    k->st.frames_acked++;
    float rtt = -1;
    if (k->hist[frame_id % HIST].id == frame_id) {
        rtt = (float)(now_ms - k->hist[frame_id % HIST].send_ms);
        float glass = (float)(now_ms - k->hist[frame_id % HIST].capture_ms) - k->page_rtt * 0.5f;
        k->st.glass_ms = k->st.glass_ms > 0 ? lerpf(k->st.glass_ms, glass, 0.15f) : glass;
    }
    phone_rate_ack(&k->rate, frame_id, rtt);
}

static void ws_update(PhoneStreamSink *base, u32 now_ms) {
    WsSink *k = (WsSink *)base;
    harvest(k);
    phone_rate_update(&k->rate, now_ms);
    if (!k->sec_start) k->sec_start = now_ms;
    if (now_ms - k->sec_start >= 1000) {
        float dt = (float)(now_ms - k->sec_start) / 1000.0f;
        k->st.fps = (float)k->sec_frames / dt;
        k->st.kbps = (float)k->sec_bytes * 8.0f / 1000.0f / dt;
        k->sec_frames = k->sec_bytes = 0;
        k->sec_start = now_ms;
    }
}

static void ws_stats(PhoneStreamSink *base, PhoneStreamStats *out) {
    WsSink *k = (WsSink *)base;
    *out = k->st;
    out->encode_ms = k->enc_n ? (float)(k->enc_sum / k->enc_n) : 0.0f;
    out->rtt_ms = k->rate.rtt_ms;
    out->quality = k->rate.quality;
    out->target_fps = k->rate.fps;
    for (int i = 0; i < SLOTS; i++)
        if (k->slots[i].ow) { out->width = k->slots[i].ow; out->height = k->slots[i].oh; }
}

static void ws_destroy(PhoneStreamSink *base) {
    WsSink *k = (WsSink *)base;
    // Wait for in-flight encodes (they reference the slots).
    for (int i = 0; i < SLOTS; i++)
        while (SDL_GetAtomicInt(&k->slots[i].state) == 1) SDL_Delay(1);
    for (int i = 0; i < SLOTS; i++) {
        free(k->slots[i].rgba);
        phone_buf_free(&k->slots[i].jpeg);
    }
    free(k);
}

PhoneStreamSink *phone_stream_ws_create(HwsServer *server, int conn, float max_fps, int max_quality) {
    WsSink *k = (WsSink *)calloc(1, sizeof(WsSink));
    if (!k) return NULL;
    k->base.impl = k;
    k->base.push = ws_push;
    k->base.on_ack = ws_on_ack;
    k->base.update = ws_update;
    k->base.stats = ws_stats;
    k->base.destroy = ws_destroy;
    k->server = server;
    SDL_SetAtomicInt(&k->conn, conn);
    phone_rate_init(&k->rate, max_fps, max_quality);
    return &k->base;
}

void phone_stream_ws_set_conn(PhoneStreamSink *base, int conn) {
    WsSink *k = (WsSink *)base;
    if (SDL_GetAtomicInt(&k->conn) != conn) {
        u32 fps = (u32)k->rate.max_fps;
        int q = k->rate.max_quality;
        phone_rate_init(&k->rate, (float)fps, q);   // a new page starts from full quality
    }
    SDL_SetAtomicInt(&k->conn, conn);
}

void phone_stream_ws_set_page_rtt(PhoneStreamSink *base, float rtt_ms) {
    WsSink *k = (WsSink *)base;
    k->page_rtt = k->page_rtt > 0 ? lerpf(k->page_rtt, rtt_ms, 0.2f) : rtt_ms;
}
