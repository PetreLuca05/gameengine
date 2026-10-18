// Shared by phone.c (state machine, server, capture) and phone_ui.c (screen UI, HUD, model).
#ifndef DG_PHONE_INTERNAL_H
#define DG_PHONE_INTERNAL_H

#include "phone.h"
#include "phone_capture.h"
#include "phone_orient.h"
#include "phone_os.h"
#include "phone_qr.h"
#include "phone_stream.h"
#include "phone_video.h"
#include "core/cvar.h"
#include "render/render.h"

struct HwsServer;

#define PHONE_THUMB_W 72
#define PHONE_THUMB_H 128
#define PHONE_GALLERY_THUMBS 12

typedef struct PhoneThumb { char path[260]; bool loaded, video; u8 px[PHONE_THUMB_W * PHONE_THUMB_H * 4]; double seconds; } PhoneThumb;

typedef struct PhoneCvars {
    Cvar *server, *port, *tls, *bind, *pairing, *res_w, *res_h, *fps, *quality, *smoothing, *sensitivity, *invert_yaw, *invert_pitch, *invert_roll,
        *record_max, *video_kbps, *photo_hires, *filter, *exposure, *token_ttl,
        *auto_pair, *device_days, *battery_drain, *hud_size, *hud_corner;
} PhoneCvars;

// What the phone screen shows (the HUD panel and the stream: the model never shows it).
typedef struct PhoneScreenView {
    PhoneApp app;
    PhoneFlow flow;
    int viewer, scroll, hover;   // hover: the highlighted hit (HUD panel), -1 none
    float zoom;
    bool rec, front, landscape, landscape_neg, driven, fake;
} PhoneScreenView;

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
    char device_name[64];        // the connected (or stored) device: "iPhone · Safari"
    bool stored;                 // <pref dir>/phone/device.json holds a paired device
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
    // phone OS (phone_os.h): flow + apps, assets
    PhoneOs os;
    PhoneApps apps;
    PhoneTheme theme;
    bool assets_loaded;
    u8 *icons_px;
    int icons_w, icons_h;
    bool spec;                   // showing a spectated player's phone
    bool raised;                 // the hold point is in the phone pose (always while equipped)
    PhoneApp sent_app;           // last app / flow logged and sent to the page
    PhoneFlow sent_flow;
    bool sent_menu;
    // HUD panel (the simulated phone screen)
    float panel_t;               // 0 hidden .. 1 shown (slide + fade)
    bool panel_on;
    bool ui_capture;             // the cursor is free for the panel (phoneui, Alt)
    bool nav_hold;               // gamepad: phonenav held, the panel has the focus
    int hover, nav;              // hovered / focused hit (-1 none)
    bool slider_drag;
    float px_x, px_y, px_w, px_h; // the panel's screen rect (pixels), last drawn
    bool prev_ui, prev_nav, prev_click;
    // gallery viewer image (decoded on open)
    char viewer_path[260];
    u8 *viewer_px;
    int viewer_w, viewer_h;
    bool front;                  // selfie camera (front lens, mirrored preview)
    bool prev_attack, prev_reload, prev_gallery, prev_selfie;
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
    // the model: screen quads drawn / with content (texture, emissive, unlit): always 0 (tests)
    u32 model_screen_draws, model_screen_content;
    MeshId quad, cube;
} PhoneState;

PhoneState *phone_state(void);

// phone_ui.c
void phone_ui_build_screen(PhoneState *s, const PhoneScreenView *v, u8 *rgba, int w, int h);   // the screen UI (transparent where the camera image shows)
void phone_ui_hud(PhoneState *s);
PhoneScreenView phone_view_own(const PhoneState *s);            // the own / spectated phone's screen
bool phone_ui_panel_rect(PhoneState *s, PhoneRect *out);       // the HUD panel's screen rect (pixels)
void phone_ui_load_assets(PhoneState *s);
void phone_ui_thumb(const u8 *rgba, int w, int h, int stride, u8 *out);   // -> PHONE_THUMB_W x PHONE_THUMB_H
void phone_ui_shutdown(void);
u32 phone_ui_hash(const PhoneState *s, const PhoneScreenView *v);   // changes when the screen UI must be redrawn

#endif
