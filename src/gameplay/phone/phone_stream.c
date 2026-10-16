// Phone stream rate control + JPEG encoding (see phone_stream.h).
#include "phone_stream.h"
#include "core/dgmath.h"
#include "stb_image_write.h"
#include <stdlib.h>

void phone_rate_init(PhoneRate *r, float max_fps, int max_quality) {
    memset(r, 0, sizeof(*r));
    r->max_fps = r->fps = clampf(max_fps, 5.0f, 60.0f);
    r->min_fps = fminf(10.0f, r->max_fps);
    r->max_quality = r->quality = DG_CLAMP(max_quality, 30, 95);
    r->min_quality = DG_MIN(35, r->max_quality);
    r->scale = 1.0f;
    r->queue_budget = 0;
}

bool phone_rate_should_send(PhoneRate *r, u32 now_ms, int queued, int last_frame_bytes) {
    if (queued < 0) return false;
    float interval = 1000.0f / fmaxf(r->fps, 1.0f);
    // Accept up to 3 ms early so a 30 Hz producer on a 60 Hz loop doesn't alias down to 20.
    if (r->last_send_ms && (float)(now_ms - r->last_send_ms) < interval - 3.0f) return false;
    int budget = r->queue_budget > 0 ? r->queue_budget : DG_MAX(2 * last_frame_bytes, 96 * 1024);
    u32 unacked = r->sent_id - r->acked_id;
    if (queued > budget || unacked > 4) {
        r->congestion++;
        return false;
    }
    return true;
}

void phone_rate_sent(PhoneRate *r, u32 id, u32 now_ms) {
    r->sent_id = id;
    r->last_send_ms = now_ms ? now_ms : 1;
}

void phone_rate_ack(PhoneRate *r, u32 id, float rtt) {
    if ((i32)(id - r->acked_id) > 0) r->acked_id = id;
    if (rtt >= 0 && rtt < 10000) r->rtt_ms = r->rtt_ms > 0 ? lerpf(r->rtt_ms, rtt, 0.2f) : rtt;
}

void phone_rate_update(PhoneRate *r, u32 now_ms) {
    if (!r->last_adjust_ms) r->last_adjust_ms = now_ms ? now_ms : 1;
    if (now_ms - r->last_adjust_ms < 1000) return;
    r->last_adjust_ms = now_ms;
    bool slow = r->rtt_ms > 250.0f;
    if (r->congestion > 2 || slow) {
        r->clean_seconds = 0;
        if (r->quality > r->min_quality) r->quality = DG_MAX(r->min_quality, r->quality - 10);
        else if (r->fps > r->min_fps) r->fps = fmaxf(r->min_fps, r->fps - 5.0f);
        else if (r->scale > 0.5f) r->scale = r->scale > 0.8f ? 0.75f : 0.5f;
    } else if (r->congestion == 0) {
        // Recover slowly (one step every 2 clean seconds), in reverse order.
        if (++r->clean_seconds >= 2) {
            r->clean_seconds = 0;
            if (r->scale < 1.0f) r->scale = r->scale < 0.6f ? 0.75f : 1.0f;
            else if (r->fps < r->max_fps) r->fps = fminf(r->max_fps, r->fps + 5.0f);
            else if (r->quality < r->max_quality) r->quality = DG_MIN(r->max_quality, r->quality + 5);
        }
    }
    r->congestion = 0;
}

// ------------------------------------------------------------------ buffers / JPEG

void phone_buf_free(PhoneBuf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

bool phone_buf_append(PhoneBuf *b, const void *p, int n) {
    if (b->len + n > b->cap) {
        int cap = DG_MAX(b->cap * 2, b->len + n + 4096);
        u8 *d = (u8 *)realloc(b->data, (size_t)cap);
        if (!d) return false;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, (size_t)n);
    b->len += n;
    return true;
}

static void jpeg_sink(void *ctx, void *data, int size) { phone_buf_append((PhoneBuf *)ctx, data, size); }

bool phone_jpeg_encode(const u8 *rgba, int w, int h, int stride, int ow, int oh, int quality, PhoneBuf *out) {
    out->len = 0;
    if (!rgba || w <= 0 || h <= 0) return false;
    if (ow <= 0 || oh <= 0 || ow > w || oh > h) { ow = w; oh = h; }
    const u8 *src = rgba;
    u8 *tmp = NULL;
    if (ow != w || oh != h || stride != w * 4) {
        // Box filter downscale (also repacks a padded stride).
        tmp = (u8 *)malloc((size_t)ow * (size_t)oh * 3);
        if (!tmp) return false;
        for (int y = 0; y < oh; y++) {
            int y0 = y * h / oh, y1 = DG_MAX(y0 + 1, (y + 1) * h / oh);
            for (int x = 0; x < ow; x++) {
                int x0 = x * w / ow, x1 = DG_MAX(x0 + 1, (x + 1) * w / ow);
                u32 acc[3] = { 0, 0, 0 }, n = 0;
                for (int yy = y0; yy < y1; yy++) {
                    const u8 *row = rgba + (size_t)yy * (size_t)stride;
                    for (int xx = x0; xx < x1; xx++) {
                        acc[0] += row[xx * 4];
                        acc[1] += row[xx * 4 + 1];
                        acc[2] += row[xx * 4 + 2];
                        n++;
                    }
                }
                u8 *d = tmp + ((size_t)y * (size_t)ow + (size_t)x) * 3;
                for (int c = 0; c < 3; c++) d[c] = (u8)((acc[c] + n / 2) / n);
            }
        }
        src = tmp;
    }
    int ok = stbi_write_jpg_to_func(jpeg_sink, out, ow, oh, tmp ? 3 : 4, src, DG_CLAMP(quality, 1, 100));
    free(tmp);
    return ok && out->len > 0;
}

bool phone_jpeg_info(const u8 *d, int len, int *w, int *h) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8 || d[len - 2] != 0xFF || d[len - 1] != 0xD9) return false;
    int p = 2;
    while (p + 4 <= len) {
        if (d[p] != 0xFF) return false;
        u8 m = d[p + 1];
        int seg = d[p + 2] << 8 | d[p + 3];
        if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (p + 9 > len) return false;
            if (h) *h = d[p + 5] << 8 | d[p + 6];
            if (w) *w = d[p + 7] << 8 | d[p + 8];
            return true;
        }
        if (m == 0xDA) return false;   // scan before a frame header
        p += 2 + seg;
    }
    return false;
}
