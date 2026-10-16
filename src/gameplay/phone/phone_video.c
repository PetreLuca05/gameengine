// H.264 / MP4 phone video recording (see phone_video.h).
#define H264E_SVC_API 0          // must match third_party/impl/tp_video.c
#define H264E_MAX_THREADS 0
#include "phone_video.h"
#include "core/jobs.h"
#include "minih264/minih264e.h"
#include "minimp4/minimp4.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

struct PhoneVideo {
    FILE *f;
    int w, h;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    u8 *yuv;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t wr;
    int kbps, frames;
    bool have_prev;
    u32 first_ms, prev_ms;
    u8 *pending;                 // the previous frame's coded data (its duration is known once the next arrives)
    int pending_len, pending_cap;
    bool ok;
};

static int mp4_write(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *)token;
    if (fseek(f, (long)offset, SEEK_SET) != 0) return 1;
    return fwrite(buffer, 1, size, f) != size;
}

void phone_video_size(int w, int h, int *ew, int *eh) {
    *ew = DG_MAX(16, w & ~15);
    *eh = DG_MAX(16, h & ~15);
}

void phone_rgba_to_i420(const u8 *rgba, int w, int h, int stride, u8 *Y, u8 *U, u8 *V) {
    for (int y = 0; y < h; y++) {
        const u8 *p = rgba + (size_t)y * (size_t)stride;
        u8 *yr = Y + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++, p += 4) yr[x] = (u8)((66 * p[0] + 129 * p[1] + 25 * p[2] + 128 + (16 << 8)) >> 8);
    }
    int cw = w / 2, ch = h / 2;
    for (int y = 0; y < ch; y++) {
        const u8 *r0 = rgba + (size_t)(2 * y) * (size_t)stride, *r1 = r0 + stride;
        for (int x = 0; x < cw; x++) {
            int r = r0[8 * x] + r0[8 * x + 4] + r1[8 * x] + r1[8 * x + 4];
            int g = r0[8 * x + 1] + r0[8 * x + 5] + r1[8 * x + 1] + r1[8 * x + 5];
            int b = r0[8 * x + 2] + r0[8 * x + 6] + r1[8 * x + 2] + r1[8 * x + 6];
            U[(size_t)y * (size_t)cw + (size_t)x] = (u8)((-38 * r - 74 * g + 112 * b + 512 + (128 << 10)) >> 10);
            V[(size_t)y * (size_t)cw + (size_t)x] = (u8)((112 * r - 94 * g - 18 * b + 512 + (128 << 10)) >> 10);
        }
    }
}

PhoneVideo *phone_video_open(const char *path, int w, int h, int kbps, char *err, size_t err_size) {
    PhoneVideo *v = (PhoneVideo *)calloc(1, sizeof(PhoneVideo));
    if (!v) return NULL;
    phone_video_size(w, h, &v->w, &v->h);
    v->kbps = kbps > 0 ? kbps : 4000;
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.width = v->w;
    cp.height = v->h;
    cp.gop = 30;                         // a key frame every second (scrubbing on phones)
    cp.vbv_size_bytes = v->kbps * 1000 / 8 * 2;
    cp.const_input_flag = 1;
    cp.temporal_denoise_flag = 0;
    cp.enableNEON = 1;
    int sp = 0, ss = 0;
    if (H264E_sizeof(&cp, &sp, &ss) != H264E_STATUS_SUCCESS) {
        snprintf(err, err_size, "encoder: unsupported size %dx%d", v->w, v->h);
        free(v);
        return NULL;
    }
    v->enc = (H264E_persist_t *)SDL_aligned_alloc(64, (size_t)sp);
    v->scratch = (H264E_scratch_t *)SDL_aligned_alloc(64, (size_t)ss);
    v->yuv = (u8 *)malloc((size_t)v->w * (size_t)v->h * 3 / 2);
    if (!v->enc || !v->scratch || !v->yuv || H264E_init(v->enc, &cp) != H264E_STATUS_SUCCESS) {
        snprintf(err, err_size, "encoder init failed");
        phone_video_close(v, NULL, NULL);
        return NULL;
    }
    v->f = fopen(path, "wb");
    if (!v->f) {
        snprintf(err, err_size, "cannot write %s", path);
        phone_video_close(v, NULL, NULL);
        return NULL;
    }
    // Non-fragmented, non-sequential: the moov index is written at close (fseek back).
    v->mux = MP4E_open(0, 0, v->f, mp4_write);
    if (!v->mux || mp4_h26x_write_init(&v->wr, v->mux, v->w, v->h, 0) != MP4E_STATUS_OK) {
        snprintf(err, err_size, "mp4 muxer init failed");
        phone_video_close(v, NULL, NULL);
        return NULL;
    }
    v->ok = true;
    return v;
}

static bool flush_pending(PhoneVideo *v, u32 dur_ms) {
    if (!v->pending_len) return true;
    unsigned d90 = (unsigned)DG_CLAMP((int)dur_ms, 1, 1000) * 90u;
    bool ok = mp4_h26x_write_nal(&v->wr, v->pending, v->pending_len, d90) == MP4E_STATUS_OK;
    v->pending_len = 0;
    return ok;
}

bool phone_video_frame(PhoneVideo *v, const u8 *rgba, u32 t_ms) {
    if (!v || !v->ok) return false;
    u8 *Y = v->yuv, *U = Y + (size_t)v->w * (size_t)v->h, *V = U + (size_t)v->w * (size_t)v->h / 4;
    phone_rgba_to_i420(rgba, v->w, v->h, v->w * 4, Y, U, V);
    H264E_io_yuv_t io;
    io.yuv[0] = Y;
    io.yuv[1] = U;
    io.yuv[2] = V;
    io.stride[0] = v->w;
    io.stride[1] = io.stride[2] = v->w / 2;
    H264E_run_param_t rp;
    memset(&rp, 0, sizeof(rp));
    rp.encode_speed = 8;                         // fast presets: real time at 640 x 1136 on one core
    rp.frame_type = 0;
    rp.desired_frame_bytes = v->kbps * 1000 / 8 / 30;
    rp.qp_min = 18;
    rp.qp_max = 42;
    unsigned char *coded = NULL;
    int coded_len = 0;
    if (H264E_encode(v->enc, v->scratch, &rp, &io, &coded, &coded_len) != H264E_STATUS_SUCCESS) return false;
    if (!v->have_prev) {
        v->first_ms = t_ms;
        v->have_prev = true;
    } else if (!flush_pending(v, t_ms - v->prev_ms)) {
        v->ok = false;
        return false;
    }
    if (coded_len > v->pending_cap) {
        u8 *p = (u8 *)realloc(v->pending, (size_t)coded_len);
        if (!p) return false;
        v->pending = p;
        v->pending_cap = coded_len;
    }
    memcpy(v->pending, coded, (size_t)coded_len);
    v->pending_len = coded_len;
    v->prev_ms = t_ms;
    v->frames++;
    return true;
}

bool phone_video_close(PhoneVideo *v, double *seconds, int *frames) {
    if (!v) return false;
    bool ok = v->ok;
    if (v->ok && v->pending_len) {
        u32 last = v->frames > 1 ? (v->prev_ms - v->first_ms) / (u32)(v->frames - 1) : 33u;
        ok = flush_pending(v, DG_MAX(last, 1u));
    }
    if (seconds) *seconds = v->frames ? (double)(v->prev_ms - v->first_ms) / 1000.0 + 1.0 / 30.0 : 0.0;
    if (frames) *frames = v->frames;
    if (v->mux) {
        MP4E_close(v->mux);
        mp4_h26x_write_close(&v->wr);
    }
    if (v->f) ok = (fclose(v->f) == 0) && ok;
    SDL_aligned_free(v->enc);
    SDL_aligned_free(v->scratch);
    free(v->yuv);
    free(v->pending);
    free(v);
    return ok && (!frames || *frames > 0);
}

// ------------------------------------------------------------------ recorder

#define REC_QUEUE 6

struct PhoneRecorder {
    PhoneVideo *v;
    SDL_Mutex *mu;
    struct { u8 *rgba; u32 t; } q[REC_QUEUE];
    int head, count;
    bool worker_busy;
    JobCounter jc;
    int w, h, pushed, dropped;
    u32 first_ms, last_ms;
    u8 *spare[REC_QUEUE];
};

static void rec_job(void *user) {
    PhoneRecorder *r = (PhoneRecorder *)user;
    for (;;) {
        SDL_LockMutex(r->mu);
        if (!r->count) {
            r->worker_busy = false;
            SDL_UnlockMutex(r->mu);
            return;
        }
        u8 *px = r->q[r->head].rgba;
        u32 t = r->q[r->head].t;
        r->q[r->head].rgba = NULL;
        r->head = (r->head + 1) % REC_QUEUE;
        r->count--;
        SDL_UnlockMutex(r->mu);
        phone_video_frame(r->v, px, t);
        SDL_LockMutex(r->mu);
        for (int i = 0; i < REC_QUEUE; i++)
            if (!r->spare[i]) { r->spare[i] = px; px = NULL; break; }
        SDL_UnlockMutex(r->mu);
        free(px);
    }
}

PhoneRecorder *phone_recorder_start(const char *path, int w, int h, int kbps, char *err, size_t err_size) {
    PhoneRecorder *r = (PhoneRecorder *)calloc(1, sizeof(PhoneRecorder));
    if (!r) return NULL;
    r->v = phone_video_open(path, w, h, kbps, err, err_size);
    if (!r->v) { free(r); return NULL; }
    phone_video_size(w, h, &r->w, &r->h);
    r->mu = SDL_CreateMutex();
    return r;
}

bool phone_recorder_push(PhoneRecorder *r, const u8 *rgba, int w, int h, u32 t_ms) {
    if (!r) return false;
    SDL_LockMutex(r->mu);
    if (r->count >= REC_QUEUE) {
        r->dropped++;
        SDL_UnlockMutex(r->mu);
        return false;
    }
    u8 *buf = NULL;
    for (int i = 0; i < REC_QUEUE && !buf; i++)
        if (r->spare[i]) { buf = r->spare[i]; r->spare[i] = NULL; }
    SDL_UnlockMutex(r->mu);
    if (!buf) buf = (u8 *)malloc((size_t)r->w * (size_t)r->h * 4);
    if (!buf) return false;
    // Crop to the encoded size (top-left aligned; the encoder needs multiples of 16).
    for (int y = 0; y < r->h; y++) memcpy(buf + (size_t)y * (size_t)r->w * 4, rgba + (size_t)y * (size_t)w * 4, (size_t)r->w * 4);
    (void)h;
    SDL_LockMutex(r->mu);
    r->q[(r->head + r->count) % REC_QUEUE].rgba = buf;
    r->q[(r->head + r->count) % REC_QUEUE].t = t_ms;
    r->count++;
    if (!r->pushed) r->first_ms = t_ms;
    r->last_ms = t_ms;
    r->pushed++;
    bool start = !r->worker_busy;
    r->worker_busy = true;
    SDL_UnlockMutex(r->mu);
    if (start) jobs_submit(rec_job, NULL, r, &r->jc);
    return true;
}

double phone_recorder_seconds(const PhoneRecorder *r) { return r && r->pushed ? (double)(r->last_ms - r->first_ms) / 1000.0 : 0.0; }

bool phone_recorder_stop(PhoneRecorder *r, double *seconds, int *frames, int *dropped) {
    if (!r) return false;
    jobs_wait(&r->jc);
    // A push racing the last job may have left frames behind: drain them here.
    rec_job(r);
    bool ok = phone_video_close(r->v, seconds, frames);
    if (dropped) *dropped = r->dropped;
    for (int i = 0; i < REC_QUEUE; i++) {
        free(r->spare[i]);
        free(r->q[i].rgba);
    }
    SDL_DestroyMutex(r->mu);
    free(r);
    return ok;
}
