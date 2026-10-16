// Shared by phone.c (state machine, server, capture) and phone_ui.c (screen UI, HUD, model).
#ifndef DG_PHONE_INTERNAL_H
#define DG_PHONE_INTERNAL_H

#include "phone.h"
#include "phone_capture.h"
#include "phone_orient.h"
#include "phone_qr.h"
#include "phone_stream.h"
#include "phone_video.h"
#include "core/cvar.h"
#include "render/render.h"

struct HwsServer;

typedef enum PhoneScreen { PSCR_CAMERA, PSCR_PAIR, PSCR_GALLERY } PhoneScreen;

#define PHONE_THUMB_W 72
#define PHONE_THUMB_H 128
#define PHONE_GALLERY_THUMBS 12

typedef struct PhoneThumb { char path[260]; bool loaded, video; u8 px[PHONE_THUMB_W * PHONE_THUMB_H * 4]; double seconds; } PhoneThumb;

typedef struct PhoneCvars {
    Cvar *server, *port, *tls, *bind, *pairing, *res_w, *res_h, *fps, *quality, *smoothing, *sensitivity, *invert_yaw, *invert_pitch, *invert_roll,
        *record_max, *video_kbps, *photo_hires, *filter, *remote_range, *remote_fps, *screen_brightness, *exposure, *token_ttl;
} PhoneCvars;

typedef struct PhoneState {
    bool init;
    PhoneCvars cv;
    // server + page
    struct HwsServer *server;
    u8 *page;
    size_t page_len;
    char server_err[160];
    u32 lan_ip;
    int conn;                    // the paired page's connection (-1 none)
    char token[32], url[160], ip_text[64];
    float token_left;            // s
    PhoneQr qr;
    TextureId qr_tex;
    float page_rtt;              // ms (ping / pong)
    char page_ua[96];
    bool page_orientation;       // the page reports DeviceOrientation access
    double last_status_t;
    // orientation
    double sample_t;             // last orientation sample (real time)
    u32 samples;
    quat dev_game;               // device pose in game axes (uncalibrated)
    float calib_yaw;
    bool calibrated, calib_pending;
    quat rel;                    // calibrated, shaped, filtered (relative to the view yaw)
    PhoneOneEuro filter;
    bool driven;                 // a fresh device (or fake) rotation drives the phone this frame
    bool fake;
    vec3 fake_ypr;               // degrees (pitch, yaw, roll as typed)
    u32 rot_packed;
    bool landscape, landscape_manual;
    // item / camera
    bool holding, camera;
    const struct ItemDef *item;
    float zoom, zoom_target;
    vec3 lens_pos;
    quat lens_rot;
    bool lens_valid;
    PhoneScreen screen;
    bool prev_attack, prev_reload, prev_pair, prev_gallery;
    // capture
    bool photo_pending;
    int photo_wait;              // frames until the requested readback is in
    u32 last_serial;
    float flash;
    PhoneRecorder *rec;
    char rec_path[640];
    double rec_start;
    bool rec_thumb_done;
    PhoneGallery gallery;
    bool gallery_scanned;
    PhoneThumb thumbs[PHONE_GALLERY_THUMBS];
    u8 last_thumb[PHONE_THUMB_W * PHONE_THUMB_H * 4];
    bool have_last_thumb;
    char toast[96];
    double toast_t;
    // stream
    PhoneStreamSink *sink;
    u32 frame_id;
    double last_render_t;
    // screen overlay (CPU-drawn UI, uploaded on change)
    TextureId overlay;
    u8 *overlay_px;
    int overlay_w, overlay_h;
    u32 overlay_hash;
    double overlay_t;
    // remote phones seen this frame (drawn as world items) -> view 1
    struct { vec3 pos; quat rot; const struct ItemDef *it; } remote[8];
    int remote_count;
    vec3 remote_lens;
    bool remote_valid;
    double remote_t;
    float screen_k;              // emissive multiplier of the screen (exposure compensated)
    MeshId quad, cube;
} PhoneState;

PhoneState *phone_state(void);

// phone_ui.c
void phone_ui_build_screen(PhoneState *s, u8 *rgba, int w, int h);   // the in-game screen UI (transparent where the camera image shows)
void phone_ui_hud(PhoneState *s);
void phone_ui_thumb(const u8 *rgba, int w, int h, int stride, u8 *out);   // -> PHONE_THUMB_W x PHONE_THUMB_H
void phone_ui_shutdown(void);
u32 phone_ui_hash(const PhoneState *s);   // changes when the screen UI must be redrawn

#endif
