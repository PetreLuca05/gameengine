// Live feed of the in-game phone camera to the companion page.
//
// PhoneStreamSink is the transport seam: the game hands it RGBA frames read back from the phone
// render target (top row first, display encoded) and it delivers them. The baseline sink is
// JPEG frames over the paired WebSocket (phone_stream_ws.c: encode on the job system, newest frame
// wins under backpressure). A WebRTC sink (H.264 / VP8 track + a data channel for orientation)
// can replace it without touching the game side: see docs/PHONE.md "Upgrading to WebRTC".
//
// The pure parts here (rate control, JPEG encode with downscale) are unit tested.
#ifndef DG_PHONE_STREAM_H
#define DG_PHONE_STREAM_H

#include "core/base.h"

typedef struct PhoneFrameInfo {
    u32 frame_id;
    u32 capture_ms;        // game clock when the frame was rendered
    float zoom;
    u8 flags;              // PHONE_MSG_FRAME flags (bit0 recording)
} PhoneFrameInfo;

typedef struct PhoneStreamStats {
    u32 frames_offered, frames_sent, frames_skipped, frames_acked;
    float fps;             // sent per second (last second)
    float kbps;            // bytes handed to the transport (last second) * 8 / 1000
    float encode_ms;       // mean JPEG encode time
    float rtt_ms;          // ack round trip (game send -> page display -> ack)
    float glass_ms;        // estimated capture -> display (render-to-ack minus half the ack trip)
    int quality, width, height;
    float target_fps;
} PhoneStreamStats;

typedef struct PhoneStreamSink {
    void *impl;
    // Offers a frame (rgba: w * h * 4, stride w * 4). The sink copies what it keeps and may skip
    // it (rate control). Returns true if accepted.
    bool (*push)(struct PhoneStreamSink *s, const u8 *rgba, int w, int h, const PhoneFrameInfo *info);
    void (*on_ack)(struct PhoneStreamSink *s, u32 frame_id, u32 now_ms);
    void (*update)(struct PhoneStreamSink *s, u32 now_ms);     // main thread, every frame
    void (*stats)(struct PhoneStreamSink *s, PhoneStreamStats *out);
    void (*destroy)(struct PhoneStreamSink *s);
} PhoneStreamSink;

// ------------------------------------------------------------------ rate control (pure)
// Adapts JPEG quality first, then the frame rate, then the resolution, from congestion signals:
// transport queue above budget, too many unacknowledged frames, slow acks. Recovers in reverse.
typedef struct PhoneRate {
    int max_quality, min_quality, quality;
    float max_fps, min_fps, fps;
    float scale;                 // 1, 0.75, 0.5 of the stream resolution
    u32 last_send_ms, last_adjust_ms;
    u32 sent_id, acked_id;       // last frame ids
    int congestion;              // signals since the last adjustment
    int clean_seconds;           // consecutive seconds without congestion
    float rtt_ms;
    int queue_budget;            // bytes allowed waiting in the transport (default 2 frames)
} PhoneRate;

void phone_rate_init(PhoneRate *r, float max_fps, int max_quality);
// May a frame be sent now? queued_bytes: the transport's backlog (-1 = no connection).
bool phone_rate_should_send(PhoneRate *r, u32 now_ms, int queued_bytes, int last_frame_bytes);
void phone_rate_sent(PhoneRate *r, u32 frame_id, u32 now_ms);
void phone_rate_ack(PhoneRate *r, u32 frame_id, float rtt_ms);
void phone_rate_update(PhoneRate *r, u32 now_ms);   // once a frame: the 1 s adjustment step

// ------------------------------------------------------------------ JPEG (pure)
typedef struct PhoneBuf { u8 *data; int len, cap; } PhoneBuf;
void phone_buf_free(PhoneBuf *b);
bool phone_buf_append(PhoneBuf *b, const void *p, int n);
// rgba (stride bytes per row, top row first) -> JPEG of out_w x out_h (box downscale when smaller).
bool phone_jpeg_encode(const u8 *rgba, int w, int h, int stride, int out_w, int out_h, int quality, PhoneBuf *out);
// Minimal JPEG header check (SOI, SOF0/2 dimensions, EOI). Tests and the gallery.
bool phone_jpeg_info(const u8 *data, int len, int *w, int *h);

// JPEG-over-WebSocket sink (phone_stream_ws.c).
struct HwsServer;
PhoneStreamSink *phone_stream_ws_create(struct HwsServer *server, int conn, float max_fps, int max_quality);
void phone_stream_ws_set_conn(PhoneStreamSink *s, int conn);   // -1 = no page connected
void phone_stream_ws_set_page_rtt(PhoneStreamSink *s, float rtt_ms);   // the page's ping round trip (glass-to-glass estimate)

#endif
