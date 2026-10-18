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
#include "net/lobby_net.h"
#include "ui/console.h"
#include "ui/menus.h"
#include "ui/gui/gui.h"
#include "ui/mainmenu/mainmenu.h"
#include "ui/screens/screens.h"
#include "render/phone_view.h"
#include "cJSON.h"
#include "stb_image_write.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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

// The page's context: the menus (the companion screen: apps greyed out) or a match.
static bool in_menus(void) {
    LobbyState st = lobby_state();
    return mainmenu_active() || st == LS_CREATING || st == LS_JOINING || st == LS_LOBBY || st == LS_COUNTDOWN || st == LS_LOADING || st == LS_MIGRATING;
}

// This PC's name for the page's "Connected to <PC name>" banner.
static const char *pc_name(void) {
    static char n[64];
    if (n[0]) return n;
    const char *e = getenv("COMPUTERNAME");
    if (!e || !e[0]) e = getenv("HOSTNAME");
    if (e && e[0]) dg_strcpy(n, sizeof(n), e);
#ifndef _WIN32
    else if (gethostname(n, sizeof(n) - 1) != 0) n[0] = 0;
#endif
    // JSON-safe: printable ASCII without quotes / backslashes.
    for (char *p = n; *p; p++) if (*p < 32 || *p > 126 || *p == '"' || *p == '\\') *p = '_';
    if (!n[0]) dg_strcpy(n, sizeof(n), "your PC");
    return n;
}

static void send_status(void) {
    if (PH.conn < 0) return;
    PhoneStreamStats st;
    memset(&st, 0, sizeof(st));
    if (PH.sink) PH.sink->stats(PH.sink, &st);
    char clock[16];
    int day;
    game_clock(clock, sizeof(clock), &day, NULL);
    float hours = 12.0f;
    game_clock(clock, sizeof(clock), &day, &hours);
    World *w = game_world();
    char b[900];
    snprintf(b, sizeof(b),
             "{\"t\":\"status\",\"v\":%d,\"holding\":%s,\"camera\":%s,\"zoom\":%.2f,\"max_zoom\":%.0f,\"rec\":%s,\"rec_s\":%.1f,\"rec_max\":%d,"
             "\"landscape\":%s,\"calibrated\":%s,\"driven\":%s,\"fps\":%.1f,\"kbps\":%.0f,\"q\":%d,\"w\":%d,\"h\":%d,\"glass\":%.0f,\"encode\":%.1f,"
             "\"clock\":\"%s\",\"day\":%d,\"hours\":%.3f,\"filter\":\"%s\",\"photos\":%d,\"front\":%s,\"app\":\"%s\",\"flow\":\"%s\",\"battery\":%.3f,"
             "\"weather\":\"%s\",\"night\":%s,\"ctx\":\"%s\",\"pc\":\"%s\",\"mode\":\"%s\"}",
             PHONE_PROTO_VERSION, PH.holding ? "true" : "false", PH.camera ? "true" : "false", PH.zoom, MAX_ZOOM, PH.rec ? "true" : "false",
             PH.rec ? g_engine.real_time - PH.rec_start : 0.0, PH.cv.record_max->i, PH.landscape ? "true" : "false", PH.calibrated ? "true" : "false",
             PH.driven ? "true" : "false", st.fps, st.kbps, st.quality, st.width, st.height, st.glass_ms, st.encode_ms, clock, day, hours,
             phone_filter_name(PH.cv.filter->i), PH.gallery.count, PH.front ? "true" : "false", phone_app_name(PH.os.app), phone_flow_name(PH.os.flow),
             PH.os.battery, w ? weather_name(w) : "clear", (hours < 6.0f || hours > 19.5f) ? "true" : "false", in_menus() ? "menu" : "match", pc_name(),
             phone_mode_name(PH.os.mode));
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

static char *read_rel(const char *rel, size_t *n) {
    char p[512];
    platform_data_path(rel, p, sizeof(p));
    *n = 0;
    return platform_read_file(p, n);
}

// The page with the phone OS assets injected (assets/phone/theme.json, apps.json, the icon atlas):
// the web UI and the in-game screen share one theme, one app list and one set of icons.
static bool load_page(void) {
    if (PH.page) return true;
    size_t n, tn, an, pn;
    char *tmpl = read_rel("web/phone/index.html", &n);
    if (!tmpl) { log_warn("phone: companion page web/phone/index.html missing"); return false; }
    char *theme = read_rel("assets/phone/theme.json", &tn), *apps = read_rel("assets/phone/apps.json", &an);
    u8 *png = (u8 *)read_rel(PH.apps.atlas[0] ? PH.apps.atlas : "assets/phone/icons.png", &pn);
    PH.page = (u8 *)phone_page_build(tmpl, n, theme, apps, png, pn, &PH.page_len);
    free(tmpl);
    free(theme);
    free(apps);
    free(png);
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

// ------------------------------------------------------------------ the stored device
// <pref dir>/phone/device.json: the paired phone's session key, so the same page reconnects on the
// next launch (phone_device_days, sliding: renewed on every connect; 0 = nothing stored).

static void device_path(char *out, size_t n) { snprintf(out, n, "%sphone/device.json", platform_pref_dir()); }

static void device_save(void) {
    float days = PH.cv.device_days->f;
    PhoneDeviceStore d;
    memset(&d, 0, sizeof(d));
    if (days <= 0 || !PH.server || !hws_session_key(PH.server, 0, d.session)) return;
    dg_strcpy(d.name, sizeof(d.name), PH.device_name);
    d.paired = (i64)time(NULL);
    d.expires = d.paired + (i64)((double)days * 86400.0);
    char *js = phone_device_store_write(&d), p[512];
    device_path(p, sizeof(p));
    if (js && platform_write_file(p, js, strlen(js))) PH.stored = true;
    free(js);
}

static void device_delete(void) {
    char p[512];
    device_path(p, sizeof(p));
    if (platform_file_exists(p)) SDL_RemovePath(p);
    PH.stored = false;
}

// Server start: the previous run's phone may resume with its key (the page retries on its own).
static void device_restore(void) {
    char p[512];
    device_path(p, sizeof(p));
    char *js = platform_read_file(p, NULL);
    if (!js) return;
    PhoneDeviceStore d;
    i64 now = (i64)time(NULL);
    bool ok = PH.cv.device_days->f > 0 && phone_device_store_read(js, &d, now);
    free(js);
    if (!ok) { device_delete(); log_info("phone: the stored device expired (or device memory is off): forgotten"); return; }
    u64 ttl = d.expires ? (u64)(d.expires - now) * 1000u : 0;
    if (!hws_restore_session(PH.server, 0, d.session, ttl)) { device_delete(); return; }
    PH.stored = true;
    dg_strcpy(PH.device_name, sizeof(PH.device_name), d.name);
    log_info("phone: waiting for the stored device (%s) to reconnect", d.name[0] ? d.name : "unnamed");
}

static bool server_start(void) {
    if (PH.server) return true;
    phone_ui_load_assets(&PH);   // the page's icon atlas path
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
    cfg.session_ttl_ms = PH.cv.device_days->f > 0 ? (u64)((double)PH.cv.device_days->f * 86400000.0) : 0;
    PH.server = hws_start(&cfg, PH.server_err, sizeof(PH.server_err));
    if (!PH.server) { log_warn("phone: server failed: %s", PH.server_err); return false; }
    PH.server_err[0] = 0;
    PH.lan_ip = lan;
    char ip[20];
    hws_ipv4_format(lan, ip, sizeof(ip));
    snprintf(PH.ip_text, sizeof(PH.ip_text), "%s:%d", ip, PH.cv.port->i);
    PH.conn = -1;
    log_info("phone: server on %s://%s (CA %s)", cfg.tls ? "https" : "http", PH.ip_text, hws_ca_fingerprint(PH.server));
    device_restore();
    return true;
}

// A new one-time pairing code + QR (the Connect portal). False: no server / pairing off.
static bool pair_begin(void) {
    PH.token[0] = 0;
    if (!PH.cv.pairing->i) { snprintf(PH.server_err, sizeof(PH.server_err), "Pairing is disabled (Settings > Phone)"); return false; }
    if (!PH.cv.server->i) { snprintf(PH.server_err, sizeof(PH.server_err), "The phone server is off (Settings > Phone)"); return false; }
    if (!server_start()) return false;
    if (!hws_issue_token(PH.server, 0, PH.token)) { snprintf(PH.server_err, sizeof(PH.server_err), "no random source"); return false; }
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
    PH.token_left = PH.cv.token_ttl->f;
    log_info("phone_pair: open %s on the phone (or scan the QR code). Fallback address: %s  (expires in %.0f s)", PH.url, PH.ip_text, PH.cv.token_ttl->f);
    return true;
}

static void drop_conn(void) {
    PH.conn = -1;
    PH.calibrated = false;
    if (PH.sink) phone_stream_ws_set_conn(PH.sink, -1);
}

// Forget device (the card, the page's Unpair, phone_unpair): the session is revoked (the page
// forgets its key), the stored device deleted.
static void unpair(void) {
    if (PH.server) hws_unpair(PH.server, 0);
    drop_conn();
    phone_os_forget(&PH.os);
    device_delete();
    PH.device_name[0] = 0;
    PH.token[0] = 0;
    toast("Phone forgotten");
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

// Gallery delete (in-game viewer or the page): the files, the entry, its thumbnail, the page.
static void delete_item(int i) {
    if (i < 0 || i >= PH.gallery.count) return;
    PhoneGalleryItem *it = &PH.gallery.items[i];
    if (PH.server && it->server_id) hws_media_remove(PH.server, it->server_id);
    for (int k = 0; k < PHONE_GALLERY_THUMBS; k++)
        if (PH.thumbs[k].path[0] && !strcmp(PH.thumbs[k].path, it->path)) memset(&PH.thumbs[k], 0, sizeof(PhoneThumb));
    if (PH.viewer_path[0] && (!strcmp(PH.viewer_path, it->path) || !strcmp(PH.viewer_path, it->thumb))) PH.viewer_path[0] = 0;
    log_info("phone: deleted %s", it->path);
    phone_gallery_delete(&PH.gallery, i);
    if (PH.os.viewer >= PH.gallery.count) PH.os.viewer = PH.gallery.count - 1;
    if (!PH.gallery.count) PH.have_last_thumb = false;
    toast("Deleted");
    send_gallery();
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
        const cJSON *ua = cJSON_GetObjectItemCaseSensitive(j, "ua"), *model = cJSON_GetObjectItemCaseSensitive(j, "model");
        dg_strcpy(PH.page_ua, sizeof(PH.page_ua), cJSON_IsString(ua) ? ua->valuestring : "");
        // The name from the whole agent string (the browser's token is at its end).
        phone_device_name(cJSON_IsString(ua) ? ua->valuestring : "", cJSON_IsString(model) ? model->valuestring : NULL, PH.device_name, sizeof(PH.device_name));
        device_save();
        send_status();
        send_gallery();
    } else if (!strcmp(cmd, "shutter")) {
        if (PH.holding) {
            phone_os_open(&PH.os, PAPP_CAMERA);   // the lens renders in the camera app
            PH.photo_pending = true;
        }
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
    } else if (!strcmp(cmd, "app")) {
        // The page's current app: the in-game screen mirrors it (other players see it too).
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "v");
        int a = cJSON_IsString(v) ? phone_app_from_name(v->valuestring) : -1;
        if (a > PAPP_PORTAL) {
            if (PH.rec && a != PAPP_CAMERA) record_stop();
            phone_os_open(&PH.os, (PhoneApp)a);
        }
    } else if (!strcmp(cmd, "back")) {
        phone_os_back(&PH.os);
    } else if (!strcmp(cmd, "delete")) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "id");
        int i = cJSON_IsNumber(v) ? phone_gallery_find_server_id(&PH.gallery, v->valueint) : -1;
        if (i >= 0) delete_item(i);
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
            phone_os_connected(&PH.os);
            PH.token[0] = 0;   // used
            char ip[20];
            hws_ipv4_format(ev.peer_ip, ip, sizeof(ip));
            device_save();
            // A page that comes back (resume) restores real-phone mode silently.
            if (ev.resumed) log_info("phone: reconnected (%s)", ip);
            else toast("Phone paired (%s)", ip);
        } break;
        case HWS_EV_DISCONNECT:
            if (ev.conn == PH.conn) {
                drop_conn();
                if (PH.rec) record_stop();
                phone_os_disconnected(&PH.os);
                if (!in_menus()) toast("Phone disconnected \xE2\x80\x94 using in-game phone");
                else log_info("phone: disconnected");
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

// ------------------------------------------------------------------ phone OS: input + actions

static void open_app(PhoneApp a) {
    if (PH.rec && a != PAPP_CAMERA) record_stop();
    phone_os_open(&PH.os, a);
}

static void set_capture(bool on) {
    if (PH.ui_capture == on) return;
    PH.ui_capture = on;
    PH.slider_drag = false;
    input_set_game_focus(!menus_wants_mouse() && !console_is_open() && g_engine.has_focus);
}

static PhoneLayoutIn layout_in(void) {
    PhoneLayoutIn li = { PH.os.app, PH.os.flow, PH.os.viewer, PH.os.scroll, PH.apps.count, PH.apps.columns, PH.gallery.count };
    return li;
}

// One button of the phone OS (mouse on the HUD panel, the gamepad, phone_tap / phone_button).
// y: the design-px position inside the button (the zoom slider).
static void activate(int id, int index, float y, const PhoneRect *r) {
    switch (id) {
    case PB_CONNECT: menus_open_phone_portal(); break;   // the Connect portal (a modal over the game)
    case PB_APP:
        if (index >= 0 && index < PH.apps.count) {
            if (PH.apps.apps[index].app > PAPP_HOME) open_app((PhoneApp)PH.apps.apps[index].app);
            else toast("%s: coming soon", PH.apps.apps[index].label);
        }
        break;
    case PB_HOME: open_app(PAPP_HOME); break;
    case PB_BACK: phone_os_back(&PH.os); break;
    case PB_SHUTTER: PH.photo_pending = true; break;
    case PB_RECORD: if (PH.rec) record_stop(); else record_start(); break;
    case PB_THUMB:
        open_app(PAPP_GALLERY);
        if (PH.gallery.count) PH.os.viewer = 0;
        break;
    case PB_FLIP: PH.front = !PH.front; break;
    case PB_ZOOM_CHIP: { static const float z[3] = { 1, 2, 5 }; PH.zoom_target = z[DG_CLAMP(index, 0, 2)]; } break;
    case PB_ZOOM_SLIDER: if (r) PH.zoom_target = clampf(phone_zoom_from_slider(y, *r, MAX_ZOOM), 1.0f, MAX_ZOOM); break;
    case PB_CELL: PH.os.viewer = index; PH.os.viewer_zoom = 1.0f; break;
    case PB_PREV: if (PH.os.viewer > 0) { PH.os.viewer--; PH.os.viewer_zoom = 1.0f; } break;
    case PB_NEXT: if (PH.os.viewer + 1 < PH.gallery.count) { PH.os.viewer++; PH.os.viewer_zoom = 1.0f; } break;
    case PB_DELETE: delete_item(PH.os.viewer); break;
    default: break;
    }
}

// Spatial navigation (gamepad): the nearest hit in the pressed direction.
static int nav_move(const PhoneHit *h, int n, int cur, int dx, int dy) {
    if (cur < 0 || cur >= n) return n ? 0 : -1;
    float cx = h[cur].r.x + h[cur].r.w * 0.5f, cy = h[cur].r.y + h[cur].r.h * 0.5f;
    int best = cur;
    float bd = 1e9f;
    for (int i = 0; i < n; i++) {
        if (i == cur) continue;
        float x = h[i].r.x + h[i].r.w * 0.5f - cx, y = h[i].r.y + h[i].r.h * 0.5f - cy;
        float along = x * (float)dx + y * (float)dy, across = fabsf(x * (float)dy) + fabsf(y * (float)dx);
        if (along <= 1.0f) continue;
        float d = along + across * 2.0f;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void panel_input(void) {
    PhoneLayoutIn li = layout_in();
    PhoneHit hits[PHONE_MAX_HITS];
    int n = phone_os_layout(&li, hits, PHONE_MAX_HITS);
    bool blocked = game()->menu_open || game()->inventory_open || console_is_open();
    bool ui = input_down(ACT_PHONE_UI) && !blocked;
    if (ui && !PH.prev_ui && PH.panel_on) set_capture(!PH.ui_capture);
    PH.prev_ui = ui;
    if ((!PH.panel_on || blocked) && PH.ui_capture) set_capture(false);
    PH.hover = -1;
    if (PH.ui_capture && PH.panel_t > 0.9f && PH.px_w > 1) {
        vec2 m = gui_mouse();
        float dx = (m.x - PH.px_x) / PH.px_w * PHONE_DESIGN_W, dy = (m.y - PH.px_y) / PH.px_h * PHONE_DESIGN_H;
        bool inside = dx >= 0 && dy >= 0 && dx < PHONE_DESIGN_W && dy < PHONE_DESIGN_H;
        if (inside) PH.hover = phone_os_hit(hits, n, dx, dy);
        if (gui_mouse_pressed(0)) {
            if (!inside) set_capture(false);   // a click in the world goes back to mouse look
            else if (PH.hover >= 0) {
                const PhoneHit *h = &hits[PH.hover];
                PH.slider_drag = h->id == PB_ZOOM_SLIDER;
                activate(h->id, h->index, dy, &h->r);
            }
        }
        if (PH.slider_drag) {
            const PhoneHit *s = phone_os_find(hits, n, PB_ZOOM_SLIDER, 0);
            if (s && gui_mouse_down(0)) activate(PB_ZOOM_SLIDER, 0, dy, &s->r);
            else PH.slider_drag = false;
        }
    }
    // Gamepad: hold phonenav, the d-pad / stick moves the focus, A presses, B goes back.
    bool nav = input_down(ACT_PHONE_NAV) && PH.panel_on && !blocked;
    if (nav && !PH.nav_hold) PH.nav = n ? 0 : -1;
    PH.nav_hold = nav;
    if (nav) {
        if (PH.nav >= n) PH.nav = n - 1;
        static const int dirs[4][3] = { { NAV_UP, 0, -1 }, { NAV_DOWN, 0, 1 }, { NAV_LEFT, -1, 0 }, { NAV_RIGHT, 1, 0 } };
        for (int i = 0; i < 4; i++)
            if (gui_nav(dirs[i][0])) { PH.nav = nav_move(hits, n, PH.nav, dirs[i][1], dirs[i][2]); gui_nav_consume(dirs[i][0]); }
        if (gui_nav(NAV_ACCEPT) && PH.nav >= 0 && PH.nav < n) {
            gui_nav_consume(NAV_ACCEPT);
            const PhoneHit *h = &hits[PH.nav];
            activate(h->id, h->index, h->r.y + h->r.h * 0.5f, &h->r);
        }
        if (gui_nav(NAV_BACK)) { gui_nav_consume(NAV_BACK); phone_os_back(&PH.os); }
        PH.hover = PH.nav;
    }
}

// ------------------------------------------------------------------ public

void phone_fill_cmd(UserCmd *c) {
    if (!PH.init || !PH.holding || PH.spec) return;
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
    if (!PH.init || !PH.holding || PH.spec) return false;
    bool over = PH.ui_capture && PH.hover >= 0;
    if (over && PH.os.app == PAPP_GALLERY) {
        if (PH.os.viewer >= 0) PH.os.viewer_zoom = clampf(PH.os.viewer_zoom * (dir > 0 ? 1.25f : 0.8f), 1.0f, 4.0f);
        else PH.os.scroll = DG_CLAMP(PH.os.scroll + (dir < 0 ? 1 : -1), 0, DG_MAX(0, (PH.gallery.count - 1) / PHONE_GALLERY_COLS - 2));
        return true;
    }
    if (PH.os.app == PAPP_CAMERA) {
        PH.zoom_target = clampf(PH.zoom_target * (dir > 0 ? 1.25f : 0.8f), 1.0f, MAX_ZOOM);
        return true;
    }
    return PH.ui_capture;   // the cursor is out for the phone: the wheel never switches items
}

bool phone_holding(void) { return PH.holding; }
bool phone_camera_mode(void) { return PH.camera; }
bool phone_paired(void) { return PH.conn >= 0; }
bool phone_ui_wants_mouse(void) { return PH.init && PH.ui_capture; }
bool phone_ui_blocks_game(void) { return PH.init && PH.nav_hold; }
bool phone_ui_escape(void) {
    if (!PH.init || !PH.ui_capture) return false;
    if (!phone_os_back(&PH.os)) set_capture(false);
    return true;
}

PhoneScreenView phone_view_own(const PhoneState *s) {
    PhoneScreenView v;
    memset(&v, 0, sizeof(v));
    v.app = s->os.app;
    v.flow = s->os.flow;
    v.viewer = s->os.viewer;
    v.scroll = s->os.scroll;
    v.hover = s->hover;
    v.zoom = s->zoom;
    v.rec = s->rec != NULL;
    v.front = s->front;
    v.landscape = s->landscape;
    v.driven = s->driven;
    v.landscape_neg = s->driven && phone_euler_ypr(s->rel).z < 0;
    v.fake = s->fake;
    return v;
}

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

// A screen canvas -> texture (a new texture per change, a few per second at most: the previous one
// may still be in use by frames in flight, render_texture_destroy defers the release).
static bool rebuild_overlay(TextureId *tex, u8 **px, u32 *hash, double *t, const PhoneScreenView *v, int w, int h, bool size_changed) {
    u32 hh = phone_ui_hash(&PH, v);
    bool due = g_engine.real_time - *t > 0.5;
    if (*tex && hh == *hash && !due && !size_changed) return false;
    if (size_changed || !*px) {
        free(*px);
        *px = (u8 *)malloc((size_t)w * (size_t)h * 4);
    }
    if (!*px) return false;
    phone_ui_build_screen(&PH, v, *px, w, h);
    if (*tex) render_texture_destroy(*tex);
    *tex = render_texture_create(w, h, *px, true, false);
    *hash = hh;
    *t = g_engine.real_time;
    return true;
}

static bool update_overlay(void) {
    int w = PH.cv.res_w->i, h = PH.cv.res_h->i;
    bool resized = w != PH.overlay_w || h != PH.overlay_h;
    PH.overlay_w = w;
    PH.overlay_h = h;
    PhoneScreenView v = phone_view_own(&PH);
    return rebuild_overlay(&PH.overlay, &PH.overlay_px, &PH.overlay_hash, &PH.overlay_t, &v, w, h, resized);
}

static void local_view(bool overlay_changed) {
    bool ui_only = phone_os_ui_only(PH.os.app);
    if (!ui_only && !PH.lens_valid) return;
    double now = g_engine.real_time;
    float fps = (float)DG_CLAMP(PH.cv.fps->i, 5, 60);
    bool due = now - PH.last_render_t >= 1.0 / fps - 0.002;
    if (!due && !(ui_only && overlay_changed)) return;
    PH.last_render_t = now;
    bool want_hires = PH.photo_pending && PH.cv.photo_hires->i && !ui_only;
    RenderPhoneView v;
    memset(&v, 0, sizeof(v));
    v.view = 0;
    v.pos = PH.lens_pos;
    v.rot = PH.lens_valid ? PH.lens_rot : quat_identity();
    v.fov_deg = lens_fov();
    v.width = want_hires ? 1080 : PH.cv.res_w->i;
    v.height = want_hires ? 1920 : PH.cv.res_h->i;
    v.overlay = PH.overlay;
    v.exposure_bias = PH.cv.exposure->f;
    v.flash = PH.flash;
    v.vignette = 0.3f;
    v.readback = !ui_only && ((PH.conn >= 0 && !PH.spec) || PH.photo_pending || PH.rec != NULL);
    v.mirror = PH.front;
    v.ui_only = ui_only;
    PhoneRect pr;
    if ((PH.panel_on || PH.panel_t > 0) && phone_ui_panel_rect(&PH, &pr)) {
        v.panel_w = (int)lrintf(pr.w);
        v.panel_h = (int)lrintf(pr.h);
        v.panel_radius = gui_s(PH.theme.panel.radius);
    }
    render_phone_view(&v);
    // The copy of this frame arrives two frames later (after the one requested last frame).
    if (PH.photo_pending && PH.photo_wait == 0 && !ui_only) PH.photo_wait = 2;
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

void phone_frame(World *w, const RenderCamera *cam, const Player *lp) {
    if (!PH.init) return;
    float dt = (float)g_engine.frame_dt;
    phone_ui_load_assets(&PH);
    // Held item. The viewed player's phone (docs/SPECTATE.md): a spectated player's app, lens, zoom,
    // front camera and landscape come with its commands; only the own phone takes input, records
    // and pairs.
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
    PH.spec = spec && PH.holding;
    PH.raised = PH.holding && lp && lp->sim.wpn.ads_on;
    if (!PH.holding && PH.rec) record_stop();
    if (!PH.holding && PH.photo_pending) PH.photo_pending = false;
    if (PH.holding && !was_holding && !PH.gallery_scanned) {
        phone_gallery_scan(&PH.gallery, platform_pref_dir());
        PH.gallery_scanned = true;
    }
    // The flow (phone_os.c): equipping never pairs (that is the menus' Connect portal): a connected
    // device drives the phone at once, else the in-game phone (HUD panel).
    bool own = PH.holding && !spec;
    if (own && !PH.os.equipped) {
        phone_os_equip(&PH.os);
        log_info("phone: equipped -> %s / %s (server %s)", phone_flow_name(PH.os.flow), phone_app_name(PH.os.app), PH.server ? PH.ip_text : "off");
    } else if (!own && PH.os.equipped) {
        phone_os_unequip(&PH.os);
        set_capture(false);
    }
    update_orientation(dt);
    if (spec) {
        const UserCmd *vc = game_view_cmd();
        if (vc->phone_zoom) PH.zoom_target = clampf((float)vc->phone_zoom / 32.0f, 1.0f, MAX_ZOOM);
        PH.front = (vc->phone_flags & CHAR_PHONE_FRONT) != 0;
        PH.landscape = (vc->phone_flags & CHAR_PHONE_LANDSCAPE) != 0;
        if (PH.rec) record_stop();
    }
    // A spectated player's phone shows no screen here (the screen lives on its owner's HUD panel /
    // real phone): only the pose (landscape, driven rotation) comes over the network.
    PH.camera = own && PH.os.app == PAPP_CAMERA;
    // Local input (in game, nothing on top). The camera app's keys work with the mouse in the game;
    // the HUD panel takes the cursor with phoneui (Alt) or the gamepad with phonenav.
    PH.panel_on = own && phone_os_panel(&PH.os);
    bool focus = !spec && input_game_focus() && !game()->menu_open && !game()->inventory_open && !console_is_open();
    bool atk = input_down(ACT_ATTACK), rel = input_down(ACT_RELOAD), gal = input_down(ACT_PHONE_GALLERY);
    bool selfie = input_down(ACT_PHONE_SELFIE);
    if (focus && own && PH.camera && selfie && !PH.prev_selfie) PH.front = !PH.front;
    PH.prev_selfie = selfie;
    if (focus && own) {
        if (PH.camera && atk && !PH.prev_attack) PH.photo_pending = true;
        if (PH.camera && rel && !PH.prev_reload) { if (PH.rec) record_stop(); else record_start(); }
        if (gal && !PH.prev_gallery) open_app(PH.os.app == PAPP_GALLERY ? PAPP_HOME : PAPP_GALLERY);
    }
    PH.prev_attack = atk;
    PH.prev_reload = rel;
    PH.prev_gallery = gal;
    if (own) panel_input();
    else { PH.hover = -1; PH.nav_hold = false; }
    if (!PH.camera && PH.photo_pending && !spec) PH.photo_pending = false;   // photos come from the camera app only
    // Zoom (smoothed), shutter flash.
    PH.zoom += (PH.zoom_target - PH.zoom) * exp_decay(12.0f, dt);
    if (fabsf(PH.zoom - PH.zoom_target) < 0.005f) PH.zoom = PH.zoom_target;
    PH.flash = fmaxf(0.0f, PH.flash - dt / 0.25f);
    // Lens from the phone as drawn this frame (first person: the hands' item transform).
    PH.lens_valid = false;
    vec3 ipos = v3(0, 0, 0);
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
    if (own) local_view(update_overlay());
    consume_readback();
    if (PH.sink) PH.sink->update(PH.sink, now_ms());
    (void)cam;
    // The HUD panel slides in / out.
    float slide = fmaxf(0.05f, PH.theme.panel.slide);
    PH.panel_t = approachf(PH.panel_t, PH.panel_on ? 1.0f : 0.0f, dt / slide);
}

// Every frame (main loop, also without a world or while the frame is not rendered): the server
// (up from the main menu with auto-pair, or from the first Connect portal, then for the whole
// run), the page's events, the portal's code, the status to the page.
void phone_tick(float dt) {
    if (!PH.init) return;
    // Server: 0 off, 1 on, 2 automatic (from the main menu with auto-pair, else the first portal).
    int sv = PH.cv.server->i;
    if (sv == 0 && PH.server) server_stop();
    bool want = sv == 1 || (sv == 2 && PH.cv.auto_pair->i && mainmenu_enabled());
    if (want && !PH.server && !PH.server_err[0]) server_start();
    poll_server();
    if (PH.server && PH.token[0]) {
        PH.token_left = hws_token_remaining(PH.server, 0);
        // An expired code on an open portal is replaced.
        if (PH.token_left <= 0 && PH.conn < 0) {
            PH.token[0] = 0;
            if (PH.os.portal) pair_begin();
        }
    }
    phone_os_tick(&PH.os, dt, PH.cv.battery_drain->f / 6000.0f);
    // Status to the page (4 Hz, and at once when the app, the flow or the context changes).
    bool menu = in_menus();
    bool changed = PH.os.app != PH.sent_app || PH.os.flow != PH.sent_flow || menu != PH.sent_menu;
    if (PH.conn >= 0 && (changed || g_engine.real_time - PH.last_status_t > 0.25)) {
        PH.last_status_t = g_engine.real_time;
        send_status();
    }
    if (changed) {
        log_info("phone: %s / %s / %s%s%s", phone_flow_name(PH.os.flow), phone_app_name(PH.os.app), menu ? "menu" : "match", PH.os.reason[0] ? " - " : "", PH.os.reason);
        PH.sent_app = PH.os.app;
        PH.sent_flow = PH.os.flow;
        PH.sent_menu = menu;
    }
}

// ------------------------------------------------------------------ pairing (the menus)

void phone_portal_show(void) {
    if (!PH.init || (PH.os.portal && (PH.token[0] || PH.conn >= 0))) return;   // already up with a code / a device
    PH.server_err[0] = 0;
    if (phone_os_portal_open(&PH.os) && !pair_begin()) phone_os_server_failed(&PH.os, PH.server_err);
    log_info("phone: Connect portal open (%s)", PH.token[0] ? PH.url : (PH.conn >= 0 ? "device connected" : PH.os.reason));
}

void phone_portal_hide(void) {
    if (!PH.init || !PH.os.portal) return;
    phone_os_portal_close(&PH.os);
    PH.token[0] = 0;
}

void phone_decline(void) {
    if (!PH.init || PH.conn >= 0) return;
    phone_os_decline(&PH.os);
    PH.token[0] = 0;
    log_info("phone: using the in-game phone this session");
}

void phone_disconnect(void) {
    if (!PH.init) return;
    if (PH.server) hws_disconnect(PH.server, 0);
    if (PH.rec) record_stop();
    drop_conn();
    phone_os_disconnect(&PH.os);
    log_info("phone: disconnected (the in-game phone this session; the stored device stays)");
}

void phone_forget(void) {
    if (PH.init) unpair();
}

PhonePairInfo phone_pair_info(void) {
    PhonePairInfo i;
    memset(&i, 0, sizeof(i));
    i.card = phone_os_card(&PH.os);
    i.mode = PH.os.mode;
    i.portal = PH.os.portal;
    i.connected = PH.conn >= 0;
    i.stored = PH.stored;
    i.server = PH.server != NULL;
    i.code = PH.token[0] && PH.token_left > 0;
    i.token_left = PH.token_left;
    i.portal_t = PH.os.portal_t;
    i.portal_connected = PH.conn >= 0 && PH.os.portal_connected_t >= 0 ? PH.os.portal_t - PH.os.portal_connected_t : -1.0f;
    i.device = PH.device_name;
    i.url = PH.url;
    i.address = PH.ip_text;
    i.error = PH.os.reason[0] ? PH.os.reason : PH.server_err;
    i.qr = PH.qr_tex;
    return i;
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

// phone_pair / phone_portal [open|close|decline]: the Connect portal (the menus' modal).
static void cmd_phone_portal(int argc, const char **argv) {
    const char *a = argc > 1 ? argv[1] : "open";
    if (!strcmp(a, "close")) { if (screens_top() == SCR_PHONE) screens_pop(); else phone_portal_hide(); }
    else if (!strcmp(a, "decline")) phone_decline();
    else menus_open_phone_portal();
    log_info("phone_portal: %s%s%s", PH.os.portal ? "open" : "closed", PH.token[0] ? "  " : "", PH.token[0] ? PH.url : "");
}

static void cmd_phone_pair(int argc, const char **argv) {
    (void)argc;
    cmd_phone_portal(1, argv);
}

static void cmd_phone_disconnect(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    phone_disconnect();
}

// phone_mode [ingame]: the session's choice (unset / ingame / device); "ingame" = Use in-game phone.
static void cmd_phone_mode(int argc, const char **argv) {
    if (argc > 1 && !strcmp(argv[1], "ingame")) phone_decline();
    else if (argc > 1) log_info("phone_mode: only 'ingame' can be set (a device sets 'device' by connecting)");
    log_info("phone_mode: %s (card %s, device %s)", phone_mode_name(PH.os.mode), phone_card_name(phone_os_card(&PH.os)), PH.device_name[0] ? PH.device_name : "-");
}

static void cmd_phone_unpair(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    unpair();
}

// The Phone card as one line (tests: phone_card.cfg, phone_ws.py).
static void cmd_phone_card(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("phone_card: card=%s mode=%s portal=%d connected=%d stored=%d server=%d code=%d device=\"%s\" ctx=%s panel=%d",
             phone_card_name(phone_os_card(&PH.os)), phone_mode_name(PH.os.mode), PH.os.portal, PH.conn >= 0, PH.stored, PH.server != NULL,
             PH.token[0] != 0, PH.device_name, in_menus() ? "menu" : "match", PH.panel_on);
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

// One line for the script tests (tools/tests/phone_flow*.cfg).
static void cmd_phone_status(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    log_info("phone_status: flow=%s app=%s panel=%d panel_t=%.2f server=%d token=%d paired=%d raised=%d camera=%d capture=%d driven=%d samples=%u "
             "model_screen=%d/%d ev=%.2f viewer=%d gallery=%d spec=%d mode=%s portal=%d screen=%s reason=\"%s\"",
             phone_flow_name(PH.os.flow), phone_app_name(PH.os.app), PH.panel_on, PH.panel_t, PH.server != NULL, PH.token[0] != 0, PH.conn >= 0, PH.raised,
             PH.camera, PH.ui_capture, PH.driven, PH.samples, PH.model_screen_content, PH.model_screen_draws, render_exposure_ev(), PH.os.viewer, PH.gallery.count, PH.spec,
             phone_mode_name(PH.os.mode), PH.os.portal, screen_name(screens_top()), PH.os.reason);
}

static void cmd_phone_photo(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (!PH.holding) { log_info("phone_photo: not holding a phone"); return; }
    open_app(PAPP_CAMERA);
    PH.photo_pending = true;
}

static void cmd_phone_record(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    if (PH.rec) record_stop();
    else if (PH.holding) { open_app(PAPP_CAMERA); record_start(); }
    else log_info("phone_record: not holding a phone");
}

static void cmd_phone_zoom(int argc, const char **argv) {
    if (argc > 1) PH.zoom_target = PH.zoom = clampf((float)atof(argv[1]), 1.0f, MAX_ZOOM);
    log_info("phone_zoom: %.2fx (FOV %.1f deg)", PH.zoom, lens_fov());
}

static void cmd_phone_screen(int argc, const char **argv) {
    if (argc < 2) { log_info("phone_screen home | camera | gallery | pair: %s", phone_app_name(PH.os.app)); return; }
    if (!strcmp(argv[1], "pair") || !strcmp(argv[1], "portal")) { cmd_phone_portal(1, argv); return; }
    int a = phone_app_from_name(argv[1]);
    if (a > PAPP_PORTAL) open_app((PhoneApp)a);
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

static const char *k_buttons[PB_COUNT] = { "none", "decline", "cancel", "reconnect", "connect", "app", "home", "back", "shutter", "record", "thumb",
                                           "flip", "zoom", "slider", "cell", "prev", "next", "delete" };

// phone_button <name> [index]: press a phone OS button (the same path as a click on the panel).
static void cmd_phone_button(int argc, const char **argv) {
    if (argc < 2) { log_info("phone_button <decline|cancel|reconnect|connect|app|home|back|shutter|record|thumb|flip|zoom|slider|cell|prev|next|delete> [index]"); return; }
    int id = -1, index = argc > 2 ? atoi(argv[2]) : 0;
    for (int i = 1; i < PB_COUNT; i++)
        if (!strcmp(argv[1], k_buttons[i])) id = i;
    PhoneLayoutIn li = layout_in();
    PhoneHit hits[PHONE_MAX_HITS];
    int n = phone_os_layout(&li, hits, PHONE_MAX_HITS);
    const PhoneHit *h = id > 0 ? phone_os_find(hits, n, id, index) : NULL;
    if (!h && id == PB_DECLINE) { phone_decline(); return; }   // the portal's decline (now a menu modal)
    if (!h) { log_info("phone_button: %s %d is not on the %s screen", argv[1], index, phone_app_name(PH.os.app)); return; }
    log_info("phone_button: %s %d", argv[1], index);
    activate(h->id, h->index, h->r.y + h->r.h * 0.5f, &h->r);
}

// phone_tap <x> <y>: a click at design px (640 x 1136) on the phone screen.
static void cmd_phone_tap(int argc, const char **argv) {
    if (argc < 3) { log_info("phone_tap <x> <y>  (design px, 640 x 1136)"); return; }
    float x = (float)atof(argv[1]), y = (float)atof(argv[2]);
    PhoneLayoutIn li = layout_in();
    PhoneHit hits[PHONE_MAX_HITS];
    int n = phone_os_layout(&li, hits, PHONE_MAX_HITS);
    int i = phone_os_hit(hits, n, x, y);
    log_info("phone_tap: %.0f %.0f -> %s %d", x, y, i >= 0 ? k_buttons[hits[i].id] : "nothing", i >= 0 ? hits[i].index : 0);
    if (i >= 0) activate(hits[i].id, hits[i].index, y, &hits[i].r);
}

static void cmd_phone_decline(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    phone_decline();
}

static void cmd_phone_back(int argc, const char **argv) {
    (void)argc;
    (void)argv;
    phone_os_back(&PH.os);
}

// phone_wait <flow|app> [frames]: hold the script until the phone's flow or app is that (tests:
// a real / simulated device connects or leaves at its own pace).
static void cmd_phone_wait(int argc, const char **argv) {
    if (argc < 2) { log_info("phone_wait <idle|pairing|local|paired|home|camera|gallery|off|not_connected|waiting|connected|ingame> [frames]"); return; }
    int left = argc > 2 ? atoi(argv[2]) : 3600;
    bool ok = !strcmp(argv[1], phone_flow_name(PH.os.flow)) || !strcmp(argv[1], phone_app_name(PH.os.app)) || !strcmp(argv[1], phone_card_name(phone_os_card(&PH.os)));
    if (ok || left <= 0) {
        log_info("phone_wait: %s %s", argv[1], ok ? "reached" : "TIMEOUT");
        return;
    }
    char line[96];
    snprintf(line, sizeof(line), "phone_wait %s %d", argv[1], left - 1);
    cbuf_retry(line);
}

static void cmd_phone_ui(int argc, const char **argv) {
    set_capture(argc > 1 ? atoi(argv[1]) != 0 && PH.panel_on : !PH.ui_capture && PH.panel_on);
    log_info("phone_ui: cursor %s", PH.ui_capture ? "on the phone" : "in the game");
}

void phone_register(void) {
    memset(&PH, 0, sizeof(PH));
    PhoneCvars *c = &PH.cv;
    c->server = cvar_reg_int("phone_server", 2, 0, 2, CVAR_ARCHIVE | CVAR_EDITABLE, "phone companion server: 0 off, 1 on, 2 automatic (the main menu with auto-pair, else the first Connect portal)");
    c->port = cvar_reg_int("phone_port", 47820, 1024, 65534, CVAR_ARCHIVE | CVAR_EDITABLE, "phone server port (HTTPS; the plain HTTP helper uses port + 1)");
    c->tls = cvar_reg_bool("phone_tls", true, CVAR_ARCHIVE | CVAR_EDITABLE, "phone server over HTTPS (needed for motion sensors on iOS / Android, docs/PHONE.md)");
    c->bind = cvar_reg_string("phone_bind", "auto", CVAR_ARCHIVE | CVAR_EDITABLE, "phone server address: auto (the LAN interface), any, or an IPv4 address");
    c->pairing = cvar_reg_bool("phone_pairing", true, CVAR_ARCHIVE | CVAR_EDITABLE, "allow pairing a phone");
    c->auto_pair = cvar_reg_bool("phone_auto_pair", true, CVAR_ARCHIVE | CVAR_EDITABLE, "start the phone server with the main menu, so a paired phone reconnects by itself (off: when the Connect portal opens)");
    c->device_days = cvar_reg_float("phone_device_days", 30.0f, 0.0f, 365.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "days a paired phone is remembered (reconnects on the next launch; renewed on every connect; 0 = not stored)");
    c->res_w = cvar_reg_int("phone_res_w", 640, 128, 1440, CVAR_ARCHIVE | CVAR_EDITABLE, "phone camera / stream width (portrait)");
    c->res_h = cvar_reg_int("phone_res_h", 1136, 256, 2560, CVAR_ARCHIVE | CVAR_EDITABLE, "phone camera / stream height (portrait)");
    c->fps = cvar_reg_int("phone_fps", 30, 5, 60, CVAR_ARCHIVE | CVAR_EDITABLE, "phone camera render / stream rate (Hz)");
    c->quality = cvar_reg_int("phone_quality", 70, 30, 95, CVAR_ARCHIVE | CVAR_EDITABLE, "phone stream JPEG quality (maximum; adapts to the network)");
    c->smoothing = cvar_reg_float("phone_smoothing", 0.5f, 0.0f, 1.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "phone orientation smoothing (0 raw .. 1 smooth)");
    c->sensitivity = cvar_reg_float("phone_sensitivity", 1.0f, 0.25f, 2.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "phone orientation sensitivity (angle multiplier)");
    c->invert_yaw = cvar_reg_bool("phone_invert_yaw", false, CVAR_ARCHIVE | CVAR_EDITABLE, "invert the phone's yaw");
    c->invert_pitch = cvar_reg_bool("phone_invert_pitch", false, CVAR_ARCHIVE | CVAR_EDITABLE, "invert the phone's pitch");
    c->invert_roll = cvar_reg_bool("phone_invert_roll", false, CVAR_ARCHIVE | CVAR_EDITABLE, "invert the phone's roll");
    c->record_max = cvar_reg_int("phone_record_max", 300, 2, 3600, CVAR_ARCHIVE | CVAR_EDITABLE, "phone video maximum length (s)");
    c->video_kbps = cvar_reg_int("phone_video_kbps", 4000, 500, 20000, CVAR_ARCHIVE | CVAR_EDITABLE, "phone video bitrate (kbit/s)");
    c->photo_hires = cvar_reg_bool("phone_photo_hires", true, CVAR_ARCHIVE | CVAR_EDITABLE, "photos: re-render the frame at 1080 x 1920");
    c->filter = cvar_reg_int("phone_filter", 0, 0, PHONE_FILTER_COUNT - 1, CVAR_ARCHIVE | CVAR_EDITABLE, "photo filter: 0 none, 1 vivid, 2 warm, 3 cool, 4 mono, 5 noir");
    c->battery_drain = cvar_reg_float("phone_battery_drain", 0.5f, 0.0f, 100.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "cosmetic phone battery drain while equipped (% per minute)");
    c->hud_size = cvar_reg_float("hud_phone_size", 0.22f, 0.12f, 0.4f, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: phone panel height (fraction of the screen height)");
    c->hud_corner = cvar_reg_int("hud_phone_corner", 0, 0, 3, CVAR_ARCHIVE | CVAR_EDITABLE, "HUD: phone panel corner: 0 bottom right, 1 bottom left, 2 top right, 3 top left");
    c->exposure = cvar_reg_float("phone_exposure", 0.0f, -3.0f, 3.0f, CVAR_ARCHIVE | CVAR_EDITABLE, "phone camera exposure bias (EV)");
    c->token_ttl = cvar_reg_float("phone_token_ttl", 120.0f, 10.0f, 600.0f, CVAR_EDITABLE, "pairing token lifetime (s)");
    cmd_register("phone_server", cmd_phone_server, "phone_server on | off: the phone companion server");
    cmd_register("phone_pair", cmd_phone_pair, "open the Connect portal (QR code + URL, printed to the console too)");
    cmd_register("phone_portal", cmd_phone_portal, "phone_portal [open|close|decline]: the Connect portal (a menu modal)");
    cmd_register("phone_unpair", cmd_phone_unpair, "forget the paired phone (its session ends, the stored device is deleted)");
    cmd_register("phone_forget", cmd_phone_unpair, "forget the paired phone (= phone_unpair)");
    cmd_register("phone_disconnect", cmd_phone_disconnect, "disconnect the phone for this session (the in-game phone; it reconnects on the next launch)");
    cmd_register("phone_mode", cmd_phone_mode, "phone_mode [ingame]: the session's phone mode (unset / ingame / device)");
    cmd_register("phone_card", cmd_phone_card, "the Phone card's state as one log line (tests)");
    cmd_register("phone_fake_orientation", cmd_phone_fake, "phone_fake_orientation <pitch> <yaw> <roll> | off: drive the phone without a device");
    cmd_register("phone_stream_stats", cmd_phone_stats, "phone stream / server / orientation statistics");
    cmd_register("phone_status", cmd_phone_status, "phone flow / app / HUD panel / screen level (one log line)");
    cmd_register("phone_photo", cmd_phone_photo, "take a photo with the held phone (opens the camera app)");
    cmd_register("phone_record", cmd_phone_record, "start / stop recording a phone video");
    cmd_register("phone_zoom", cmd_phone_zoom, "phone_zoom <1..8>: phone camera zoom");
    cmd_register("phone_screen", cmd_phone_screen, "phone_screen home | camera | gallery | pair: open a phone app (pair: the Connect portal)");
    cmd_register("phone_landscape", cmd_phone_landscape, "phone_landscape [0|1]: two-handed landscape hold (automatic with a paired phone)");
    cmd_register("phone_selfie", cmd_phone_selfie, "phone_selfie [0|1]: front (selfie) / back camera (key T, or the page)");
    cmd_register("phone_calibrate", cmd_phone_calibrate, "re-centre the paired phone's orientation (the page has a button / double tap)");
    cmd_register("phone_button", cmd_phone_button, "phone_button <name> [index]: press a phone screen button (decline, home, app 0, cell 2 ...)");
    cmd_register("phone_tap", cmd_phone_tap, "phone_tap <x> <y>: tap the phone screen at design px (640 x 1136)");
    cmd_register("phone_decline", cmd_phone_decline, "Connect portal: use the in-game phone this session");
    cmd_register("phone_back", cmd_phone_back, "phone: back (viewer -> gallery -> home)");
    cmd_register("phone_wait", cmd_phone_wait, "phone_wait <flow|app|card> [frames]: scripts wait until the phone gets there (tests)");
    cmd_register("phone_ui", cmd_phone_ui, "phone_ui [0|1]: free the cursor for the HUD phone panel (key Alt)");
    PH.conn = -1;
    PH.zoom = PH.zoom_target = 1.0f;
    PH.rel = quat_identity();
    PH.hover = PH.nav = -1;
    phone_os_init(&PH.os);
    PH.sent_app = PAPP_OFF;
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
