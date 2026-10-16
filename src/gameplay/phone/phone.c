// In-game phone + companion page (see phone.h, docs/PHONE.md).
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE   // localtime_r under -std=c11
#endif
#include "phone_internal.h"
#include "phone_proto.h"
#include "anim/character_rt.h"
#include "core/engine.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/platform.h"
#include "game/game.h"
#include "game/usercmd.h"
#include "gameplay/items.h"
#include "gameplay/weapons.h"
#include "input/input.h"
#include "net/http_ws/hws_server.h"
#include "net/http_ws/hws_tls.h"
#include "player/hands.h"
#include "audio/audio.h"
#include "ui/console.h"
#include "render/phone_view.h"
#include "cJSON.h"
#include "stb_image_write.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_ZOOM 8.0f
#define SAMPLE_TIMEOUT 0.5   // s without orientation samples -> back to the mouse aim

static PhoneState PH;
PhoneState *phone_state(void) { return &PH; }

static u32 now_ms(void) { return (u32)SDL_GetTicks(); }

static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(PH.toast, sizeof(PH.toast), fmt, ap);
    va_end(ap);
    PH.toast_t = g_engine.real_time;
    log_info("phone: %s", PH.toast);
}

static void media_dir(char *out, size_t n, bool video) {
    snprintf(out, n, "%s%s", platform_pref_dir(), video ? "videos/phone/" : "screenshots/phone/");
}

static void ensure_dirs(void) {
    char d[512];
    snprintf(d, sizeof(d), "%sscreenshots", platform_pref_dir());
    SDL_CreateDirectory(d);
    media_dir(d, sizeof(d), false);
    SDL_CreateDirectory(d);
    snprintf(d, sizeof(d), "%svideos", platform_pref_dir());
    SDL_CreateDirectory(d);
    media_dir(d, sizeof(d), true);
    SDL_CreateDirectory(d);
}

// ------------------------------------------------------------------ page messages

static void send_text(const char *s) {
    if (PH.server && PH.conn >= 0) hws_send(PH.server, PH.conn, HWS_OP_TEXT, s, (int)strlen(s), false);
}

static void game_clock(char *out, size_t n, int *day, float *hours) {
    World *w = game_world();
    float h = w ? world_time_hours(w) : 12.0f;
    int d = w ? world_day_number(w) + 1 : 1;   // 1-based like the HUD
    int hh = (int)h, mm = (int)((h - (float)hh) * 60.0f);
    snprintf(out, n, "%02d:%02d", hh % 24, mm % 60);
    if (day) *day = d;
    if (hours) *hours = h;
}

static void send_status(void) {
    if (PH.conn < 0) return;
    PhoneStreamStats st;
    memset(&st, 0, sizeof(st));
    if (PH.sink) PH.sink->stats(PH.sink, &st);
    char clock[16];
    int day;
    game_clock(clock, sizeof(clock), &day, NULL);
    char b[640];
    snprintf(b, sizeof(b),
             "{\"t\":\"status\",\"v\":%d,\"holding\":%s,\"camera\":%s,\"zoom\":%.2f,\"max_zoom\":%.0f,\"rec\":%s,\"rec_s\":%.1f,\"rec_max\":%d,"
             "\"landscape\":%s,\"calibrated\":%s,\"driven\":%s,\"fps\":%.1f,\"kbps\":%.0f,\"q\":%d,\"w\":%d,\"h\":%d,\"glass\":%.0f,\"encode\":%.1f,"
             "\"clock\":\"%s\",\"day\":%d,\"filter\":\"%s\",\"photos\":%d,\"front\":%s}",
             PHONE_PROTO_VERSION, PH.holding ? "true" : "false", PH.camera ? "true" : "false", PH.zoom, MAX_ZOOM, PH.rec ? "true" : "false",
             PH.rec ? g_engine.real_time - PH.rec_start : 0.0, PH.cv.record_max->i, PH.landscape ? "true" : "false", PH.calibrated ? "true" : "false",
             PH.driven ? "true" : "false", st.fps, st.kbps, st.quality, st.width, st.height, st.glass_ms, st.encode_ms, clock, day,
             phone_filter_name(PH.cv.filter->i), PH.gallery.count, PH.front ? "true" : "false");
    send_text(b);
}

static int register_media(PhoneGalleryItem *it) {
    if (!PH.server) return 0;
    if (!it->server_id) it->server_id = hws_media_add(PH.server, 0, it->path, it->name);
    return it->server_id;
}

static void media_json(PhoneGalleryItem *it, char *b, size_t n) {
    int id = register_media(it);
    // Thumbnail: photos are their own, videos have <stem>.thumb.jpg (registered on demand).
    char thumb[200] = "";
    if (!it->video) snprintf(thumb, sizeof(thumb), "/media/%d/%s", id, it->name);
    else if (PH.server && platform_file_exists(it->thumb)) {
        char tn[128];
        snprintf(tn, sizeof(tn), "%.*s.thumb.jpg", (int)strlen(it->name) - 4, it->name);
        int tid = hws_media_add(PH.server, 0, it->thumb, tn);
        snprintf(thumb, sizeof(thumb), "/media/%d/%s", tid, tn);
    }
    snprintf(b, n, "{\"id\":%d,\"kind\":\"%s\",\"name\":\"%s\",\"url\":\"/media/%d/%s\",\"thumb\":\"%s\",\"w\":%d,\"h\":%d,\"seconds\":%.1f,\"time\":\"%s\"}", id,
             it->video ? "video" : "photo", it->name, id, it->name, thumb, it->meta.width, it->meta.height, it->meta.seconds, it->meta.created);
}

static void send_gallery(void) {
    if (PH.conn < 0) return;
    size_t cap = 256 + (size_t)PH.gallery.count * 420;
    char *b = (char *)malloc(cap);
    if (!b) return;
    size_t n = (size_t)snprintf(b, cap, "{\"t\":\"gallery\",\"items\":[");
    for (int i = 0; i < PH.gallery.count && i < 120; i++) {
        char e[420];
        media_json(&PH.gallery.items[i], e, sizeof(e));
        n += (size_t)snprintf(b + n, cap - n, "%s%s", i ? "," : "", e);
    }
    snprintf(b + n, cap - n, "]}");
    send_text(b);
    free(b);
}

static void send_media(PhoneGalleryItem *it) {
    if (PH.conn < 0) return;
    char e[420], b[480];
    media_json(it, e, sizeof(e));
    snprintf(b, sizeof(b), "{\"t\":\"media\",\"item\":%s}", e);
    send_text(b);
}

// ------------------------------------------------------------------ server

static bool load_page(void) {
    if (PH.page) return true;
    char p[512];
    platform_data_path("web/phone/index.html", p, sizeof(p));
    PH.page = (u8 *)platform_read_file(p, &PH.page_len);
    if (!PH.page) log_warn("phone: companion page %s missing", p);
    return PH.page != NULL;
}

static void server_stop(void) {
    if (!PH.server) return;
    if (PH.sink) { PH.sink->destroy(PH.sink); PH.sink = NULL; }
    hws_stop(PH.server);
    PH.server = NULL;
    PH.conn = -1;
    PH.token[0] = PH.url[0] = 0;
    for (int i = 0; i < PH.gallery.count; i++) PH.gallery.items[i].server_id = 0;
    log_info("phone: server stopped");
}

static bool server_start(void) {
    if (PH.server) return true;
    if (!load_page()) { snprintf(PH.server_err, sizeof(PH.server_err), "web/phone/index.html missing"); return false; }
    u32 ips[8];
    int n = hws_lan_addresses(ips, 8);
    const char *bind = PH.cv.bind->str;
    u32 bind_ip = 0, lan = n > 0 ? ips[0] : 0x7F000001u;
    if (!strcmp(bind, "any")) bind_ip = 0;
    else if (!strcmp(bind, "auto") || !bind[0]) bind_ip = lan;
    else if (!hws_ipv4_parse(bind, &bind_ip)) bind_ip = lan;
    else lan = bind_ip;
    if (n == 0 && (!strcmp(bind, "auto") || !bind[0])) log_warn("phone: no LAN interface found: listening on 127.0.0.1 only");
    HwsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = (u16)PH.cv.port->i;
    cfg.http_port = (u16)(PH.cv.port->i + 1);
    cfg.tls = PH.cv.tls->i != 0;
    cfg.bind_ip = bind_ip;
    cfg.lan_ip = lan;
    char certs[512];
    snprintf(certs, sizeof(certs), "%sphone/certs/", platform_pref_dir());
    char pd[512];
    snprintf(pd, sizeof(pd), "%sphone", platform_pref_dir());
    SDL_CreateDirectory(pd);
    cfg.cert_dir = certs;
    cfg.page = PH.page;
    cfg.page_len = PH.page_len;
    cfg.token_ttl_ms = (u64)(PH.cv.token_ttl->f * 1000.0f);
    PH.server = hws_start(&cfg, PH.server_err, sizeof(PH.server_err));
    if (!PH.server) { log_warn("phone: server failed: %s", PH.server_err); return false; }
    PH.server_err[0] = 0;
    PH.lan_ip = lan;
    char ip[20];
    hws_ipv4_format(lan, ip, sizeof(ip));
    snprintf(PH.ip_text, sizeof(PH.ip_text), "%s:%d", ip, PH.cv.port->i);
    PH.conn = -1;
    log_info("phone: server on %s://%s (CA %s)", cfg.tls ? "https" : "http", PH.ip_text, hws_ca_fingerprint(PH.server));
    return true;
}

static void pair_begin(void) {
    if (!PH.cv.pairing->i) { toast("Pairing is disabled (Settings > Phone)"); return; }
    if (!server_start()) { toast("Phone server: %s", PH.server_err); return; }
    if (!hws_issue_token(PH.server, 0, PH.token)) { toast("Pairing: no random source"); return; }
    hws_url(PH.server, PH.token, PH.url, sizeof(PH.url));
    phone_qr_encode(PH.url, &PH.qr);
    if (PH.qr_tex) render_texture_destroy(PH.qr_tex);
    PH.qr_tex = 0;
    int qw = 0;
    u8 *px = phone_qr_rgba(&PH.qr, 6, 3, &qw);
    if (px) {
        PH.qr_tex = render_texture_create(qw, qw, px, false, false);
        free(px);
    }
    PH.screen = PSCR_PAIR;
    log_info("phone: pair: %s  (expires in %.0f s)", PH.url, PH.cv.token_ttl->f);
}

static void unpair(void) {
    if (PH.server) hws_unpair(PH.server, 0);
    PH.conn = -1;
    PH.calibrated = false;
    if (PH.sink) phone_stream_ws_set_conn(PH.sink, -1);
    toast("Phone unpaired");
}

// ------------------------------------------------------------------ capture

typedef struct PhotoJob {
    u8 *px;
    int w, h;
    int rotate;                  // 0, +1 (90 cw), -1 (90 ccw): landscape photos stand upright
    PhoneShotMeta meta;
    char path[640], side[640];
    bool ok;
    u8 thumb[PHONE_THUMB_W * PHONE_THUMB_H * 4];
    PhoneBuf jpeg;
} PhotoJob;

static u8 *rotate90(const u8 *px, int w, int h, int dir) {
    u8 *o = (u8 *)malloc((size_t)w * (size_t)h * 4);
    if (!o) return NULL;
    // Output is h x w.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int ox = dir > 0 ? h - 1 - y : y, oy = dir > 0 ? x : w - 1 - x;
            memcpy(o + ((size_t)oy * (size_t)h + (size_t)ox) * 4, px + ((size_t)y * (size_t)w + (size_t)x) * 4, 4);
        }
    return o;
}

static void photo_job(void *u) {
    PhotoJob *j = (PhotoJob *)u;
    if (j->rotate) {
        u8 *r = rotate90(j->px, j->w, j->h, j->rotate);
        if (r) {
            free(j->px);
            j->px = r;
            int t = j->w;
            j->w = j->h;
            j->h = t;
        }
    }
    j->meta.width = j->w;
    j->meta.height = j->h;
    phone_photo_look(j->px, j->w, j->h, 0.35f, 0.25f, phone_filter_from_name(j->meta.filter));
    j->ok = phone_jpeg_encode(j->px, j->w, j->h, j->w * 4, j->w, j->h, 92, &j->jpeg) && platform_write_file(j->path, j->jpeg.data, (size_t)j->jpeg.len);
    if (j->ok) {
        char *js = phone_meta_to_json(&j->meta);
        if (js) {
            platform_write_file(j->side, js, strlen(js));
            free(js);
        }
    }
    phone_ui_thumb(j->px, j->w, j->h, j->w * 4, j->thumb);
}

static void photo_done(void *u) {
    PhotoJob *j = (PhotoJob *)u;
    if (j->ok) {
        PhoneGalleryItem *it = phone_gallery_add(&PH.gallery, j->path, false, &j->meta);
        memcpy(PH.last_thumb, j->thumb, sizeof(PH.last_thumb));
        PH.have_last_thumb = true;
        if (PH.thumbs[PHONE_GALLERY_THUMBS - 1].path[0]) memset(&PH.thumbs[PHONE_GALLERY_THUMBS - 1], 0, sizeof(PhoneThumb));
        memmove(&PH.thumbs[1], &PH.thumbs[0], sizeof(PhoneThumb) * (PHONE_GALLERY_THUMBS - 1));
        memset(&PH.thumbs[0], 0, sizeof(PhoneThumb));
        dg_strcpy(PH.thumbs[0].path, sizeof(PH.thumbs[0].path), j->path);
        memcpy(PH.thumbs[0].px, j->thumb, sizeof(j->thumb));
        PH.thumbs[0].loaded = true;
        log_info("phone: photo %s (%dx%d)", j->path, j->w, j->h);
        if (PH.conn >= 0 && PH.server) {
            int id = register_media(it);
            send_media(it);
            // The JPEG itself for "save to phone" (a blob on the page, no second download).
            u8 hdr[8] = { PHONE_MSG_PHOTO, 0, 0, 0, 0, 0, 0, 0 };
            phone_put_u32(hdr + 4, (u32)id);
            u8 *msg = (u8 *)malloc((size_t)j->jpeg.len + 8);
            if (msg) {
                memcpy(msg, hdr, 8);
                memcpy(msg + 8, j->jpeg.data, (size_t)j->jpeg.len);
                hws_send(PH.server, PH.conn, HWS_OP_BINARY, msg, j->jpeg.len + 8, false);
                free(msg);
            }
        }
    } else {
        toast("Photo failed: cannot write %s", j->path);
    }
    phone_buf_free(&j->jpeg);
    free(j->px);
    free(j);
}

static void fill_meta(PhoneShotMeta *m, const char *kind, const char *file) {
    memset(m, 0, sizeof(*m));
    dg_strcpy(m->kind, sizeof(m->kind), kind);
    dg_strcpy(m->file, sizeof(m->file), file);
    m->zoom = PH.zoom;
    dg_strcpy(m->filter, sizeof(m->filter), phone_filter_name(PH.cv.filter->i));
    World *w = game_world();
    if (w) {
        dg_strcpy(m->map, sizeof(m->map), w->level.name);
        dg_strcpy(m->weather, sizeof(m->weather), weather_name(w));
    }
    m->position = PH.lens_pos;
    vec3 f = quat_rotate(PH.lens_rot, v3(0, 0, -1));
    m->yaw_deg = atan2f(-f.x, -f.z) * DG_RAD2DEG;
    m->pitch_deg = asinf(clampf(f.y, -1, 1)) * DG_RAD2DEG;
    char clock[16];
    game_clock(clock, sizeof(clock), &m->day, &m->tod_hours);
    snprintf(m->game_time, sizeof(m->game_time), "Day %d  %s", m->day, clock);
    time_t t = time(NULL);
    struct tm tm;
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(m->created, sizeof(m->created), "%Y-%m-%dT%H:%M:%S", &tm);
}

static void take_photo(const u8 *rgba, int w, int h) {
    ensure_dirs();
    PhotoJob *j = (PhotoJob *)calloc(1, sizeof(PhotoJob));
    if (!j) return;
    j->px = (u8 *)malloc((size_t)w * (size_t)h * 4);
    if (!j->px) { free(j); return; }
    memcpy(j->px, rgba, (size_t)w * (size_t)h * 4);
    j->w = w;
    j->h = h;
    if (PH.landscape) {
        vec3 a = phone_euler_ypr(PH.rel);
        j->rotate = a.z > 0 ? 1 : -1;
    }
    char dir[512], stem[64];
    media_dir(dir, sizeof(dir), false);
    phone_media_stem(dir, ".jpg", (i64)time(NULL), stem, sizeof(stem));
    snprintf(j->path, sizeof(j->path), "%s%s.jpg", dir, stem);
    snprintf(j->side, sizeof(j->side), "%s%s.json", dir, stem);
    char file[96];
    snprintf(file, sizeof(file), "%s.jpg", stem);
    fill_meta(&j->meta, "photo", file);
    jobs_submit(photo_job, photo_done, j, NULL);
    PH.flash = 1.0f;
    audio_play_2d("click", 0.6f, SND_SFX);
}

static void record_start(void) {
    if (PH.rec) return;
    ensure_dirs();
    char dir[512], stem[64], err[160] = "";
    media_dir(dir, sizeof(dir), true);
    phone_media_stem(dir, ".mp4", (i64)time(NULL), stem, sizeof(stem));
    snprintf(PH.rec_path, sizeof(PH.rec_path), "%s%s.mp4", dir, stem);
    PH.rec = phone_recorder_start(PH.rec_path, PH.cv.res_w->i, PH.cv.res_h->i, PH.cv.video_kbps->i, err, sizeof(err));
    if (!PH.rec) { toast("Recording failed: %s", err); return; }
    PH.rec_start = g_engine.real_time;
    PH.rec_thumb_done = false;
    toast("Recording");
    audio_play_2d("click", 0.5f, SND_SFX);
}

static void record_stop(void) {
    if (!PH.rec) return;
    double secs = 0;
    int frames = 0, dropped = 0;
    bool ok = phone_recorder_stop(PH.rec, (u32)((g_engine.real_time - PH.rec_start) * 1000.0), &secs, &frames, &dropped);
    PH.rec = NULL;
    if (!ok) { toast("Recording failed"); return; }
    char name[96];
    const char *slash = strrchr(PH.rec_path, '/');
    dg_strcpy(name, sizeof(name), slash ? slash + 1 : PH.rec_path);
    PhoneShotMeta m;
    fill_meta(&m, "video", name);
    int ew, eh;
    phone_video_size(PH.cv.res_w->i, PH.cv.res_h->i, &ew, &eh);
    m.width = ew;
    m.height = eh;
    m.seconds = secs;
    m.frames = frames;
    char side[700];
    snprintf(side, sizeof(side), "%.*s.json", (int)strlen(PH.rec_path) - 4, PH.rec_path);
    char *js = phone_meta_to_json(&m);
    if (js) {
        platform_write_file(side, js, strlen(js));
        free(js);
    }
    PhoneGalleryItem *it = phone_gallery_add(&PH.gallery, PH.rec_path, true, &m);
    log_info("phone: video %s: %.1f s, %d frames (%d dropped)", PH.rec_path, secs, frames, dropped);
    toast("Video saved (%.0f s)", secs);
    memmove(&PH.thumbs[1], &PH.thumbs[0], sizeof(PhoneThumb) * (PHONE_GALLERY_THUMBS - 1));
    memset(&PH.thumbs[0], 0, sizeof(PhoneThumb));
    dg_strcpy(PH.thumbs[0].path, sizeof(PH.thumbs[0].path), PH.rec_path);
    memcpy(PH.thumbs[0].px, PH.last_thumb, sizeof(PH.last_thumb));
    PH.thumbs[0].loaded = PH.have_last_thumb;
    PH.thumbs[0].video = true;
    PH.thumbs[0].seconds = secs;
    send_media(it);
}

// First recorded frame: the video's gallery thumbnail (<stem>.thumb.jpg).
static void record_thumb(const u8 *rgba, int w, int h) {
    PH.rec_thumb_done = true;
    int tw = 270, th = (int)((float)tw * (float)h / (float)w);
    PhoneBuf b = { 0 };
    char path[700];
    snprintf(path, sizeof(path), "%.*s.thumb.jpg", (int)strlen(PH.rec_path) - 4, PH.rec_path);
    if (phone_jpeg_encode(rgba, w, h, w * 4, tw, th, 80, &b)) platform_write_file(path, b.data, (size_t)b.len);
    phone_buf_free(&b);
    phone_ui_thumb(rgba, w, h, w * 4, PH.last_thumb);
    PH.have_last_thumb = true;
}

// ------------------------------------------------------------------ events from the page

static void calibrate(void) {
    PH.calib_pending = true;
    toast("Calibrated: hold the phone upright, facing forward");
}

static void handle_text(const char *s) {
    cJSON *j = cJSON_Parse(s);
    if (!j) return;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "c");
    const char *cmd = cJSON_IsString(c) ? c->valuestring : "";
    if (!strcmp(cmd, "hello")) {
        const cJSON *ua = cJSON_GetObjectItemCaseSensitive(j, "ua");
        dg_strcpy(PH.page_ua, sizeof(PH.page_ua), cJSON_IsString(ua) ? ua->valuestring : "");
        send_status();
        send_gallery();
    } else if (!strcmp(cmd, "shutter")) {
        if (PH.holding) PH.photo_pending = true;
    } else if (!strcmp(cmd, "record")) {
        if (PH.rec) record_stop();
        else if (PH.holding) record_start();
    } else if (!strcmp(cmd, "zoom")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
        if (cJSON_IsNumber(v)) PH.zoom_target = clampf((float)v->valuedouble, 1.0f, MAX_ZOOM);
    } else if (!strcmp(cmd, "calibrate") || !strcmp(cmd, "recenter")) {
        calibrate();
    } else if (!strcmp(cmd, "unpair")) {
        unpair();
    } else if (!strcmp(cmd, "ack")) {
        const cJSON *f = cJSON_GetObjectItemCaseSensitive(j, "f");
        if (cJSON_IsNumber(f) && PH.sink) PH.sink->on_ack(PH.sink, (u32)f->valuedouble, now_ms());
    } else if (!strcmp(cmd, "ping")) {
        const cJSON *pt = cJSON_GetObjectItemCaseSensitive(j, "pt");
        const cJSON *rtt = cJSON_GetObjectItemCaseSensitive(j, "rtt");
        if (cJSON_IsNumber(rtt)) {
            PH.page_rtt = (float)rtt->valuedouble;
            if (PH.sink) phone_stream_ws_set_page_rtt(PH.sink, PH.page_rtt);
        }
        char b[96];
        snprintf(b, sizeof(b), "{\"t\":\"pong\",\"pt\":%.0f,\"gt\":%u}", cJSON_IsNumber(pt) ? pt->valuedouble : 0.0, now_ms());
        send_text(b);
    } else if (!strcmp(cmd, "gallery")) {
        send_gallery();
    } else if (!strcmp(cmd, "perm")) {
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(j, "orientation");
        PH.page_orientation = cJSON_IsTrue(o);
        if (!PH.page_orientation) toast("The phone browser denied motion access (see docs/PHONE.md)");
    } else if (!strcmp(cmd, "camera")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
        PH.front = cJSON_IsString(v) ? !strcmp(v->valuestring, "front") : !PH.front;
    } else if (!strcmp(cmd, "filter")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
        if (cJSON_IsString(v)) cvar_set_int(PH.cv.filter, phone_filter_from_name(v->valuestring));
    }
    cJSON_Delete(j);
}

static void handle_orient(const u8 *d, int len) {
    PhoneOrientMsg m;
    if (!phone_parse_orient(d, len, &m)) return;
    PH.dev_game = phone_orient_game(phone_quat_from_i16(m.q));
    PH.sample_t = g_engine.real_time;
    PH.samples++;
    if (!PH.calibrated) PH.calib_pending = true;   // first sample: the current heading is forward
}

static void poll_server(void) {
    HwsEvent ev;
    while (PH.server && hws_poll(PH.server, &ev)) {
        switch (ev.type) {
        case HWS_EV_CONNECT: {
            PH.conn = ev.conn;
            if (!PH.sink) PH.sink = phone_stream_ws_create(PH.server, ev.conn, (float)PH.cv.fps->i, PH.cv.quality->i);
            phone_stream_ws_set_conn(PH.sink, ev.conn);
            PH.calibrated = false;
            PH.screen = PSCR_CAMERA;
            char ip[20];
            hws_ipv4_format(ev.peer_ip, ip, sizeof(ip));
            toast("Phone %s (%s)", ev.resumed ? "reconnected" : "paired", ip);
        } break;
        case HWS_EV_DISCONNECT:
            if (ev.conn == PH.conn) {
                PH.conn = -1;
                if (PH.sink) phone_stream_ws_set_conn(PH.sink, -1);
                toast("Phone disconnected");
            }
            break;
        case HWS_EV_TEXT: if (ev.conn == PH.conn) handle_text((const char *)ev.data); break;
        case HWS_EV_BINARY: if (ev.conn == PH.conn) handle_orient(ev.data, ev.len); break;
        default: break;
        }
        hws_event_free(&ev);
    }
}

// ------------------------------------------------------------------ orientation

static void update_orientation(float dt) {
    bool fresh = PH.conn >= 0 && PH.samples > 0 && g_engine.real_time - PH.sample_t < SAMPLE_TIMEOUT;
    quat target;
    if (PH.fake) {
        // phone_fake_orientation <pitch> <yaw> <roll>: relative to the view yaw, degrees.
        target = phone_from_ypr(v3(PH.fake_ypr.y * DG_DEG2RAD, PH.fake_ypr.x * DG_DEG2RAD, PH.fake_ypr.z * DG_DEG2RAD));
    } else if (fresh) {
        if (PH.calib_pending) {
            PH.calib_yaw = phone_heading(PH.dev_game);
            PH.calib_pending = false;
            PH.calibrated = true;
            phone_one_euro_init(&PH.filter, 0.6f + 3.4f * (1.0f - PH.cv.smoothing->f), 0.5f, 1.0f);
        }
        target = phone_apply_calibration(PH.dev_game, PH.calib_yaw);
        target = phone_shape(target, PH.cv.sensitivity->f, PH.cv.invert_yaw->i != 0, PH.cv.invert_pitch->i != 0, PH.cv.invert_roll->i != 0);
    } else {
        PH.driven = false;
        return;
    }
    if (!PH.filter.init || PH.fake) phone_one_euro_init(&PH.filter, 0.6f + 3.4f * (1.0f - PH.cv.smoothing->f), 0.5f, 1.0f);
    float cut = 0.6f + 3.4f * (1.0f - PH.cv.smoothing->f);
    PH.filter.min_cutoff = cut;
    PH.rel = phone_one_euro(&PH.filter, target, dt);
    PH.driven = true;
    PH.rot_packed = phone_quat_pack(PH.rel);
    // Landscape from the roll (hysteresis), unless toggled by hand.
    float roll = fabsf(phone_euler_ypr(PH.rel).z) * DG_RAD2DEG;
    if (!PH.landscape && roll > 55.0f) PH.landscape = true;
    else if (PH.landscape && roll < 35.0f) PH.landscape = false;
}

// ------------------------------------------------------------------ public

void phone_fill_cmd(UserCmd *c) {
    if (!PH.init || !PH.holding) return;
    u8 f = 0;
    if (PH.landscape) f |= CHAR_PHONE_LANDSCAPE;
    if (PH.driven) f |= CHAR_PHONE_DRIVEN;
    if (PH.rec) f |= CHAR_PHONE_REC;
    if (PH.front) f |= CHAR_PHONE_FRONT;
    c->phone_flags = f;
    c->phone_rot = PH.driven ? PH.rot_packed : 0;
    c->phone_zoom = (u8)DG_CLAMP((int)lrintf(PH.zoom_target * 32.0f), 32, 255);   // spectators: the same lens
}

bool phone_wheel(int dir) {
    if (!PH.init || !PH.camera) return false;
    PH.zoom_target = clampf(PH.zoom_target * (dir > 0 ? 1.25f : 0.8f), 1.0f, MAX_ZOOM);
    return true;
}

bool phone_holding(void) { return PH.holding; }
bool phone_camera_mode(void) { return PH.camera; }
bool phone_paired(void) { return PH.conn >= 0; }

const char *phone_status_text(void) {
    static char b[160];
    PhoneStreamStats st;
    memset(&st, 0, sizeof(st));
    if (PH.sink) PH.sink->stats(PH.sink, &st);
    snprintf(b, sizeof(b), "%s%s  zoom %.1fx%s", PH.server ? PH.ip_text : "server off", PH.conn >= 0 ? "  paired" : "", PH.zoom, PH.rec ? "  REC" : "");
    return b;
}

static float lens_fov(void) {
    float f1 = PH.item ? (PH.front ? PH.item->cam.front_fov_deg : PH.item->cam.fov_deg) : 68.0f;
    return 2.0f * atanf(tanf(f1 * 0.5f * DG_DEG2RAD) / fmaxf(PH.zoom, 1.0f)) * DG_RAD2DEG;
}

static void update_overlay(void) {
    int w = PH.cv.res_w->i, h = PH.cv.res_h->i;
    u32 hash = phone_ui_hash(&PH);
    bool due = g_engine.real_time - PH.overlay_t > 0.5;
    if (PH.overlay && hash == PH.overlay_hash && !due && w == PH.overlay_w && h == PH.overlay_h) return;
    if (w != PH.overlay_w || h != PH.overlay_h || !PH.overlay_px) {
        free(PH.overlay_px);
        PH.overlay_px = (u8 *)malloc((size_t)w * (size_t)h * 4);
        PH.overlay_w = w;
        PH.overlay_h = h;
    }
    if (!PH.overlay_px) return;
    phone_ui_build_screen(&PH, PH.overlay_px, w, h);
    // A new texture per change (a few per second at most): the previous one may still be in use
    // by frames in flight, render_texture_destroy defers the release.
    if (PH.overlay) render_texture_destroy(PH.overlay);
    PH.overlay = render_texture_create(w, h, PH.overlay_px, true, false);
    PH.overlay_hash = hash;
    PH.overlay_t = g_engine.real_time;
}

static void local_view(float dt) {
    if (!PH.lens_valid) return;
    double now = g_engine.real_time;
    float fps = (float)DG_CLAMP(PH.cv.fps->i, 5, 60);
    bool due = now - PH.last_render_t >= 1.0 / fps - 0.002;
    if (!due) return;
    PH.last_render_t = now;
    bool want_hires = PH.photo_pending && PH.cv.photo_hires->i;
    RenderPhoneView v;
    memset(&v, 0, sizeof(v));
    v.view = 0;
    v.pos = PH.lens_pos;
    v.rot = PH.lens_rot;
    v.fov_deg = lens_fov();
    v.width = want_hires ? 1080 : PH.cv.res_w->i;
    v.height = want_hires ? 1920 : PH.cv.res_h->i;
    v.overlay = PH.overlay;
    v.exposure_bias = PH.cv.exposure->f;
    v.flash = PH.flash;
    v.vignette = 0.3f;
    v.readback = PH.conn >= 0 || PH.photo_pending || PH.rec != NULL;
    v.mirror = PH.front;
    render_phone_view(&v);
    // The copy of this frame arrives two frames later (after the one requested last frame).
    if (PH.photo_pending && PH.photo_wait == 0) PH.photo_wait = 2;
    (void)dt;
}

static void consume_readback(void) {
    const u8 *px;
    int w, h;
    u32 serial;
    if (!render_phone_readback(0, &px, &w, &h, &serial) || serial == PH.last_serial) return;
    PH.last_serial = serial;
    bool hires_frame = w == 1080 && h == 1920;
    if (PH.photo_pending && PH.photo_wait > 0 && --PH.photo_wait == 0) {
        if (!PH.cv.photo_hires->i || hires_frame) {
            take_photo(px, w, h);
            PH.photo_pending = false;
        } else {
            PH.photo_wait = 1;   // keep waiting for the high resolution frame
        }
    }
    if (hires_frame) return;   // the stream / video stay at the stream resolution
    if (PH.sink && PH.conn >= 0) {
        PhoneFrameInfo fi = { ++PH.frame_id, now_ms(), PH.zoom, (u8)(PH.rec ? 1 : 0) };
        PH.sink->push(PH.sink, px, w, h, &fi);
    }
    if (PH.rec) {
        u32 t = (u32)((g_engine.real_time - PH.rec_start) * 1000.0);
        phone_recorder_push(PH.rec, px, w, h, t);
        if (!PH.rec_thumb_done) record_thumb(px, w, h);
        if (g_engine.real_time - PH.rec_start >= (double)PH.cv.record_max->i) record_stop();
    }
}

static void remote_view(vec3 cam_pos) {
    // The nearest other phone within range (drawn this frame as a world item) gets view 1.
    PH.remote_valid = false;
    float best = PH.cv.remote_range->f;
    int bi = -1;
    for (int i = 0; i < PH.remote_count; i++) {
        float d = v3_dist(PH.remote[i].pos, cam_pos);
        if (d < best) { best = d; bi = i; }
    }
    if (bi < 0) return;
    const ItemDef *it = PH.remote[bi].it;
    vec3 lens = v3_add(PH.remote[bi].pos, quat_rotate(PH.remote[bi].rot, it->cam.lens.t));
    PH.remote_lens = PH.remote[bi].pos;
    PH.remote_valid = true;
    double now = g_engine.real_time;
    if (now - PH.remote_t < 1.0 / (double)DG_CLAMP(PH.cv.remote_fps->i, 1, 30)) return;
    PH.remote_t = now;
    RenderPhoneView v;
    memset(&v, 0, sizeof(v));
    v.view = 1;
    v.pos = lens;
    v.rot = PH.remote[bi].rot;
    v.fov_deg = it->cam.fov_deg;
    v.width = 270;
    v.height = 480;
    render_phone_view(&v);
}

void phone_frame(World *w, const RenderCamera *cam, const Player *lp) {
    if (!PH.init) return;
    float dt = (float)g_engine.frame_dt;
    // Held item.
    // The viewed player's phone (docs/SPECTATE.md): a spectated player's lens, zoom, front camera and
    // landscape come with its commands; only the own phone takes input, records and pairs.
    const ItemDef *it = NULL;
    bool spec = game_view_slot() != game()->local_slot;
    if (w && lp && lp->sim.move.mode != MOVE_DEAD) {
        const InvSlot *eq = weapon_equipped(w, game_view_slot());
        const ItemDef *d = eq ? item_def(w->items, eq->item) : NULL;
        if (d && d->camera) it = d;
    }
    bool was_holding = PH.holding;
    PH.holding = it != NULL;
    PH.item = it;
    PH.camera = PH.holding && lp && lp->sim.wpn.ads_on;
    if (!PH.holding && PH.rec) record_stop();
    if (!PH.holding && PH.photo_pending) PH.photo_pending = false;
    if (PH.holding && !was_holding && !PH.gallery_scanned) {
        phone_gallery_scan(&PH.gallery, platform_pref_dir());
        PH.gallery_scanned = true;
    }
    // Server: 0 off, 1 on, 2 automatic (starts with the first phone in hand, then stays up).
    int sv = PH.cv.server->i;
    if (sv == 0 && PH.server) server_stop();
    if ((sv == 1 || (sv == 2 && PH.holding)) && !PH.server && !PH.server_err[0]) server_start();
    poll_server();
    if (PH.server && PH.token[0]) {
        PH.token_left = hws_token_remaining(PH.server, 0);
        if (PH.token_left <= 0 && PH.conn < 0 && PH.screen == PSCR_PAIR) PH.token[0] = 0;
    }
    update_orientation(dt);
    if (spec) {
        const UserCmd *vc = game_view_cmd();
        if (vc->phone_zoom) PH.zoom_target = clampf((float)vc->phone_zoom / 32.0f, 1.0f, MAX_ZOOM);
        PH.front = (vc->phone_flags & CHAR_PHONE_FRONT) != 0;
        PH.landscape = (vc->phone_flags & CHAR_PHONE_LANDSCAPE) != 0;
        if (PH.rec) record_stop();
    }
    // Local input (in game, nothing on top).
    bool focus = !spec && input_game_focus() && !game()->menu_open && !game()->inventory_open && !console_is_open();
    bool atk = input_down(ACT_ATTACK), rel = input_down(ACT_RELOAD), pair = input_down(ACT_PHONE_PAIR), gal = input_down(ACT_PHONE_GALLERY);
    bool selfie = input_down(ACT_PHONE_SELFIE);
    if (focus && PH.holding && selfie && !PH.prev_selfie) PH.front = !PH.front;
    PH.prev_selfie = selfie;
    if (focus && PH.holding) {
        if (PH.camera && atk && !PH.prev_attack) PH.photo_pending = true;
        if (PH.camera && rel && !PH.prev_reload) { if (PH.rec) record_stop(); else record_start(); }
        if (pair && !PH.prev_pair) { if (PH.screen == PSCR_PAIR) PH.screen = PSCR_CAMERA; else pair_begin(); }
        if (gal && !PH.prev_gallery) PH.screen = PH.screen == PSCR_GALLERY ? PSCR_CAMERA : PSCR_GALLERY;
    }
    PH.prev_attack = atk;
    PH.prev_reload = rel;
    PH.prev_pair = pair;
    PH.prev_gallery = gal;
    if (PH.camera && PH.screen == PSCR_GALLERY) PH.screen = PSCR_CAMERA;
    // Zoom (smoothed), shutter flash.
    PH.zoom += (PH.zoom_target - PH.zoom) * exp_decay(12.0f, dt);
    if (fabsf(PH.zoom - PH.zoom_target) < 0.005f) PH.zoom = PH.zoom_target;
    PH.flash = fmaxf(0.0f, PH.flash - dt / 0.25f);
    // Lens from the phone as drawn this frame (first person: the hands' item transform).
    PH.lens_valid = false;
    vec3 ipos;
    quat irot;
    if (PH.holding && hands_held_xform(&ipos, &irot)) {
        const Xform *lens = PH.front ? &it->cam.front : &it->cam.lens;
        PH.lens_pos = v3_add(ipos, quat_rotate(irot, lens->t));
        PH.lens_rot = quat_norm(quat_mul(irot, lens->r));
        PH.lens_valid = true;
    }
    if (PH.lens_valid && !(isfinite(PH.lens_pos.x) && isfinite(PH.lens_rot.w))) {
        static int warned;
        if (!warned++) log_warn("phone: non-finite lens transform (held item %.2f %.2f %.2f)", ipos.x, ipos.y, ipos.z);
        PH.lens_valid = false;
    }
    // Screen brightness: a lit display under this frame's exposure (EV100 -> 1 / exposure).
    float ev = render_exposure_ev();
    PH.screen_k = exp2f(ev) * 0.125f / 0.18f * PH.cv.screen_brightness->f;
    if (PH.holding) {
        update_overlay();
        local_view(dt);
    }
    consume_readback();
    if (PH.sink) PH.sink->update(PH.sink, now_ms());
    remote_view(cam->pos);
    PH.remote_count = 0;
    // Status to the page (4 Hz).
    if (PH.conn >= 0 && g_engine.real_time - PH.last_status_t > 0.25) {
        PH.last_status_t = g_engine.real_time;
        send_status();
    }
}

// ------------------------------------------------------------------ commands

static void cmd_phone_server(int argc, const char **argv) {
    if (argc > 1) {
        bool on = !strcmp(argv[1], "on") || !strcmp(argv[1], "1");
        PH.server_err[0] = 0;
        cvar_set_int(PH.cv.server, on ? 1 : 0);
        if (on) server_start();
        else server_stop();
    }
    log_info("phone_server: %s%s%s", PH.server ? "on " : "off", PH.server ? PH.ip_text : "", PH.server_err[0] ? PH.server_err : "");
}

static void cmd_phone_pair(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    PH.server_err[0] = 0;
    pair_begin();
    if (PH.url[0]) log_info("phone_pair: open %s on the phone (or scan the QR code). Fallback address: %s", PH.url, PH.ip_text);
}

static void cmd_phone_unpair(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    unpair();
}

static void cmd_phone_fake(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "off")) {
        PH.fake = false;
        log_info("phone_fake_orientation: off");
        return;
    }
    if (argc < 4) { log_info("phone_fake_orientation <pitch> <yaw> <roll> | off  (degrees, relative to the view)"); return; }
    PH.fake = true;
    PH.fake_ypr = v3((float)atof(argv[1]), (float)atof(argv[2]), (float)atof(argv[3]));
    log_info("phone_fake_orientation: pitch %.1f yaw %.1f roll %.1f", PH.fake_ypr.x, PH.fake_ypr.y, PH.fake_ypr.z);
}

static void cmd_phone_stats(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    PhoneStreamStats st;
    memset(&st, 0, sizeof(st));
    if (PH.sink) PH.sink->stats(PH.sink, &st);
    HwsStats hs = hws_stats(PH.server);
    log_info("phone_stream_stats: %s, page %s, %ux%u q%d, %.1f fps (target %.0f), %.0f kbps, encode %.1f ms, rtt %.0f ms, glass-to-glass ~%.0f ms, page ping %.0f ms",
             PH.server ? PH.ip_text : "server off", PH.conn >= 0 ? "connected" : "none", st.width, st.height, st.quality, st.fps, st.target_fps, st.kbps,
             st.encode_ms, st.rtt_ms, st.glass_ms, PH.page_rtt);
    log_info("phone_stream_stats: frames offered %u sent %u skipped %u acked %u, replaced in queue %u | server in %.1f kB out %.1f kB, conns %u, tls fail %u, auth fail %u, throttled %u, gpu %.2f ms",
             st.frames_offered, st.frames_sent, st.frames_skipped, st.frames_acked, hs.dropped_frames, hs.bytes_in / 1024.0, hs.bytes_out / 1024.0, hs.connections,
             hs.tls_failures, hs.auth_failures, hs.throttled, render_phone_gpu_ms(0));
    log_info("phone_stream_stats: orientation %s (%u samples, %.0f ms ago), calibrated %d, landscape %d, zoom %.2f",
             PH.fake ? "fake" : (PH.driven ? "device" : "mouse"), PH.samples, (g_engine.real_time - PH.sample_t) * 1000.0, PH.calibrated, PH.landscape, PH.zoom);
}

static void cmd_phone_photo(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!PH.holding) { log_info("phone_photo: not holding a phone"); return; }
    PH.photo_pending = true;
}

static void cmd_phone_record(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (PH.rec) record_stop();
    else if (PH.holding) record_start();
    else log_info("phone_record: not holding a phone");
}

static void cmd_phone_zoom(int argc, const char **argv) {
    if (argc > 1) PH.zoom_target = PH.zoom = clampf((float)atof(argv[1]), 1.0f, MAX_ZOOM);
    log_info("phone_zoom: %.2fx (FOV %.1f deg)", PH.zoom, lens_fov());
}

static void cmd_phone_screen(int argc, const char **argv) {
    if (argc > 1) PH.screen = !strcmp(argv[1], "gallery") ? PSCR_GALLERY : (!strcmp(argv[1], "pair") ? PSCR_PAIR : PSCR_CAMERA);
}

static void cmd_phone_landscape(int argc, const char **argv) {
    PH.landscape = argc > 1 ? atoi(argv[1]) != 0 : !PH.landscape;
    PH.landscape_manual = true;
}

static void cmd_phone_selfie(int argc, const char **argv) {
    PH.front = argc > 1 ? atoi(argv[1]) != 0 : !PH.front;
    log_info("phone_selfie: %s camera", PH.front ? "front" : "back");
}

static void cmd_phone_calibrate(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    calibrate();
}

void phone_register(void) {
    memset(&PH, 0, sizeof(PH));
    PhoneCvars *c = &PH.cv;
    c->server = cvar_reg_int("phone_server", 2, 0, 2, CVAR_ARCHIVE, "phone companion server: 0 off, 1 on, 2 automatic (starts when a phone is equipped)");
    c->port = cvar_reg_int("phone_port", 47820, 1024, 65534, CVAR_ARCHIVE, "phone server port (HTTPS; the plain HTTP helper uses port + 1)");
    c->tls = cvar_reg_bool("phone_tls", true, CVAR_ARCHIVE, "phone server over HTTPS (needed for motion sensors on iOS / Android, docs/PHONE.md)");
    c->bind = cvar_reg_string("phone_bind", "auto", CVAR_ARCHIVE, "phone server address: auto (the LAN interface), any, or an IPv4 address");
    c->pairing = cvar_reg_bool("phone_pairing", true, CVAR_ARCHIVE, "allow pairing a phone");
    c->res_w = cvar_reg_int("phone_res_w", 640, 128, 1440, CVAR_ARCHIVE, "phone camera / stream width (portrait)");
    c->res_h = cvar_reg_int("phone_res_h", 1136, 256, 2560, CVAR_ARCHIVE, "phone camera / stream height (portrait)");
    c->fps = cvar_reg_int("phone_fps", 30, 5, 60, CVAR_ARCHIVE, "phone camera render / stream rate (Hz)");
    c->quality = cvar_reg_int("phone_quality", 70, 30, 95, CVAR_ARCHIVE, "phone stream JPEG quality (maximum; adapts to the network)");
    c->smoothing = cvar_reg_float("phone_smoothing", 0.5f, 0.0f, 1.0f, CVAR_ARCHIVE, "phone orientation smoothing (0 raw .. 1 smooth)");
    c->sensitivity = cvar_reg_float("phone_sensitivity", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE, "phone orientation sensitivity (angle multiplier)");
    c->invert_yaw = cvar_reg_bool("phone_invert_yaw", false, CVAR_ARCHIVE, "invert the phone's yaw");
    c->invert_pitch = cvar_reg_bool("phone_invert_pitch", false, CVAR_ARCHIVE, "invert the phone's pitch");
    c->invert_roll = cvar_reg_bool("phone_invert_roll", false, CVAR_ARCHIVE, "invert the phone's roll");
    c->record_max = cvar_reg_int("phone_record_max", 300, 2, 3600, CVAR_ARCHIVE, "phone video maximum length (s)");
    c->video_kbps = cvar_reg_int("phone_video_kbps", 4000, 500, 20000, CVAR_ARCHIVE, "phone video bitrate (kbit/s)");
    c->photo_hires = cvar_reg_bool("phone_photo_hires", true, CVAR_ARCHIVE, "photos: re-render the frame at 1080 x 1920");
    c->filter = cvar_reg_int("phone_filter", 0, 0, PHONE_FILTER_COUNT - 1, CVAR_ARCHIVE, "photo filter: 0 none, 1 vivid, 2 warm, 3 cool, 4 mono, 5 noir");
    c->remote_range = cvar_reg_float("phone_remote_range", 4.0f, 0.0f, 20.0f, CVAR_ARCHIVE, "other players' phone screens render live within this distance (m)");
    c->remote_fps = cvar_reg_int("phone_remote_fps", 8, 1, 30, CVAR_ARCHIVE, "other players' phone screens: render rate (Hz)");
    c->screen_brightness = cvar_reg_float("phone_screen_brightness", 0.85f, 0.1f, 2.0f, CVAR_ARCHIVE, "in-game phone screen brightness");
    c->exposure = cvar_reg_float("phone_exposure", 0.0f, -3.0f, 3.0f, CVAR_ARCHIVE, "phone camera exposure bias (EV)");
    c->token_ttl = cvar_reg_float("phone_token_ttl", 120.0f, 10.0f, 600.0f, 0, "pairing token lifetime (s)");
    cmd_register("phone_server", cmd_phone_server, "phone_server on | off: the phone companion server");
    cmd_register("phone_pair", cmd_phone_pair, "show the pairing QR code and print the URL");
    cmd_register("phone_unpair", cmd_phone_unpair, "disconnect the paired phone");
    cmd_register("phone_fake_orientation", cmd_phone_fake, "phone_fake_orientation <pitch> <yaw> <roll> | off: drive the phone without a device");
    cmd_register("phone_stream_stats", cmd_phone_stats, "phone stream / server / orientation statistics");
    cmd_register("phone_photo", cmd_phone_photo, "take a photo with the held phone");
    cmd_register("phone_record", cmd_phone_record, "start / stop recording a phone video");
    cmd_register("phone_zoom", cmd_phone_zoom, "phone_zoom <1..8>: phone camera zoom");
    cmd_register("phone_screen", cmd_phone_screen, "phone_screen camera | pair | gallery");
    cmd_register("phone_landscape", cmd_phone_landscape, "phone_landscape [0|1]: two-handed landscape hold (automatic with a paired phone)");
    cmd_register("phone_selfie", cmd_phone_selfie, "phone_selfie [0|1]: front (selfie) / back camera (key T, or the page)");
    cmd_register("phone_calibrate", cmd_phone_calibrate, "re-centre the paired phone's orientation (the page has a button / double tap)");
    PH.conn = -1;
    PH.zoom = PH.zoom_target = 1.0f;
    PH.rel = quat_identity();
    PH.init = true;
}

void phone_shutdown(void) {
    if (!PH.init) return;
    record_stop();
    server_stop();
    hws_random_shutdown();
    free(PH.page);
    free(PH.overlay_px);
    phone_ui_shutdown();
    PH.init = false;
}
