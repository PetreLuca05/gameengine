// Phone OS (docs/PHONE.md "Phone flow"): pure, unit tested (tests/test_phone.c).
//   * the session's pairing: the Connect portal (a menu modal: main menu, pause menu), the mode
//     (unset / in-game phone / device), the Phone card's state, the device's name;
//   * the held phone: the in-game phone (HUD panel) or the paired device; equipping never pairs;
//   * the apps (home, camera, gallery) and the gallery viewer;
//   * the screen layout in design px (640 x 1136 portrait), shared by the CPU canvas (phone_ui.c)
//     and the hit tests of the HUD panel;
//   * assets/phone/apps.json + theme.json, the HUD panel rectangle;
//   * the companion page with the phone assets injected (phone_page_build).
#ifndef DG_PHONE_OS_H
#define DG_PHONE_OS_H

#include "core/base.h"

#define PHONE_DESIGN_W 640.0f
#define PHONE_DESIGN_H 1136.0f

// Replicated (UserCmd.phone_app, the character state): keep the values stable. PAPP_PORTAL is no
// longer on the phone's screen (the portal is a menu modal) but keeps its value.
typedef enum PhoneApp { PAPP_OFF, PAPP_PORTAL, PAPP_HOME, PAPP_CAMERA, PAPP_GALLERY, PAPP_COUNT } PhoneApp;
// What drives the phone: IDLE (put away, no device), PAIRING (the portal waits for a device),
// LOCAL (the in-game phone, HUD panel), PAIRED (a device is connected). LOST is kept for its name.
typedef enum PhoneFlow { PFLOW_IDLE, PFLOW_PAIRING, PFLOW_LOCAL, PFLOW_PAIRED, PFLOW_LOST } PhoneFlow;
// The session's choice (per run, per client): unset until the portal is answered.
typedef enum PhoneMode { PMODE_UNSET, PMODE_INGAME, PMODE_DEVICE } PhoneMode;
// The Phone card (main menu Player tab, pause menu, the footer glyph).
typedef enum PhoneCard { PCARD_NOT_CONNECTED, PCARD_WAITING, PCARD_CONNECTED, PCARD_INGAME } PhoneCard;
const char *phone_app_name(int app);
int phone_app_from_name(const char *s);   // -1 unknown
const char *phone_flow_name(int flow);
const char *phone_mode_name(int mode);
int phone_mode_from_name(const char *s);  // -1 unknown
const char *phone_card_name(int card);

typedef struct PhoneOs {
    PhoneFlow flow;            // derived (phone_os.c refresh): device > portal > equipped
    PhoneApp app;              // the app on the screen (OFF unequipped)
    PhoneApp last_app;         // where the phone opens (HOME at first)
    bool equipped;
    PhoneMode mode;
    bool device;               // a companion page is connected
    bool portal;               // the Connect portal is open
    float portal_t;            // seconds the portal has been open
    float portal_connected_t;  // portal_t when a device connected while it was open (-1 none)
    char reason[64];           // the last change ("Phone disconnected", a server error ...)
    int viewer;                // gallery: -1 the grid, else the open item
    float viewer_zoom;         // 1..4
    int scroll;                // gallery: first visible row
    float battery;             // 0..1, cosmetic (drains while equipped)
} PhoneOs;

void phone_os_init(PhoneOs *os);
// Equipped (hotbar, wheel, X): the last app; a connected device drives it, else the in-game phone.
// Never opens the portal.
void phone_os_equip(PhoneOs *os);
void phone_os_unequip(PhoneOs *os);
// The Connect portal. open: true when the caller must issue a pairing code (no device yet).
bool phone_os_portal_open(PhoneOs *os);
void phone_os_portal_close(PhoneOs *os);
void phone_os_server_failed(PhoneOs *os, const char *why);   // the portal shows why
void phone_os_decline(PhoneOs *os);         // "Use in-game phone": mode INGAME, the portal closes
void phone_os_connected(PhoneOs *os);       // a device connected (or resumed): mode DEVICE
// The device's socket closed (network, page closed). True when the held phone falls back to the
// in-game phone (HUD panel back: the caller shows the toast). The mode stays DEVICE: a page that
// comes back restores real-phone mode silently.
bool phone_os_disconnected(PhoneOs *os);
void phone_os_disconnect(PhoneOs *os);      // the card's Disconnect: mode INGAME
void phone_os_forget(PhoneOs *os);          // Forget device: mode UNSET
void phone_os_tick(PhoneOs *os, float dt, float drain_per_s);
void phone_os_open(PhoneOs *os, PhoneApp app);   // home / camera / gallery
bool phone_os_back(PhoneOs *os);            // viewer -> grid -> home; false: nothing to go back to
bool phone_os_panel(const PhoneOs *os);     // the HUD panel shows: equipped, no device
bool phone_os_ui_only(PhoneApp app);        // the screen is UI only (no lens view)
PhoneCard phone_os_card(const PhoneOs *os);
// A short device name from the page's user agent (and the model, when the browser tells it):
// "iPhone", "iPad", "Pixel 7", "Android phone", ... + " · Safari" / " · Chrome" ...
void phone_device_name(const char *ua, const char *model, char *out, size_t n);

// The remembered device (<pref dir>/phone/device.json): its session key, so the same page
// reconnects on the next launch. Times are Unix seconds; expires 0 = never.
typedef struct PhoneDeviceStore { char session[32]; char name[64]; i64 paired, expires; } PhoneDeviceStore;
char *phone_device_store_write(const PhoneDeviceStore *d);                    // JSON (malloc'd)
bool phone_device_store_read(const char *json, PhoneDeviceStore *out, i64 now);   // false: malformed, no key or expired

// ------------------------------------------------------------------ layout (design px)
typedef struct PhoneRect { float x, y, w, h; } PhoneRect;
// PB_DECLINE / CANCEL / RECONNECT: the old in-panel portal (no longer laid out, ids kept for phone_button).
typedef enum PhoneButton {
    PB_NONE, PB_DECLINE, PB_CANCEL, PB_RECONNECT, PB_CONNECT, PB_APP, PB_HOME, PB_BACK,
    PB_SHUTTER, PB_RECORD, PB_THUMB, PB_FLIP, PB_ZOOM_CHIP, PB_ZOOM_SLIDER, PB_CELL, PB_PREV, PB_NEXT, PB_DELETE, PB_COUNT
} PhoneButton;
typedef struct PhoneHit { int id, index; PhoneRect r; } PhoneHit;
typedef struct PhoneLayoutIn {
    PhoneApp app;
    PhoneFlow flow;
    int viewer, scroll;
    int app_count, columns;    // home grid
    int gallery_count;
} PhoneLayoutIn;
#define PHONE_MAX_HITS 48
#define PHONE_GALLERY_COLS 3
#define PHONE_GALLERY_ROWS 4
int phone_os_layout(const PhoneLayoutIn *in, PhoneHit *out, int max);
int phone_os_hit(const PhoneHit *hits, int n, float x, float y);   // index into hits, -1 none
const PhoneHit *phone_os_find(const PhoneHit *hits, int n, int id, int index);
float phone_zoom_from_slider(float y, PhoneRect slider, float max_zoom);   // log scale, top = max
float phone_slider_from_zoom(float zoom, PhoneRect slider, float max_zoom); // knob y

// ------------------------------------------------------------------ assets
#define PHONE_MAX_APPS 16
#define PHONE_MAX_TILES 32
typedef struct PhoneAppDef { char id[24], label[32], icon[24]; int app; int tile; } PhoneAppDef;
typedef struct PhoneApps {
    PhoneAppDef apps[PHONE_MAX_APPS];
    int count, columns;
    char atlas[128];
    int tile_px, tile_count;
    char tiles[PHONE_MAX_TILES][24];
    struct { char weather[24], tile[24]; } wx[24];
    int wx_count;
} PhoneApps;
bool phone_apps_parse(const char *json, PhoneApps *out, char *err, size_t err_size);
int phone_apps_tile(const PhoneApps *a, const char *name);                  // -1 unknown
int phone_apps_weather_tile(const PhoneApps *a, const char *weather, bool night);

typedef struct PhonePanelTokens { float readout_gap, radius, bezel, shadow, hint_gap, slide; } PhonePanelTokens;
typedef struct PhoneTheme {
    u32 bg, surface, surface_hi, text, text_dim, text_faint, accent, accent_text, ok, warning, danger, home_bar, camera_ui, camera_accent;
    float status_h, pad, radius, button_h, icon, icon_label, home_bar_w, home_bar_h, grid_gap;
    PhonePanelTokens panel;
    u32 bezel_color, bezel_edge, shadow_color;
} PhoneTheme;
void phone_theme_default(PhoneTheme *t);
bool phone_theme_parse(const char *json, PhoneTheme *out, char *err, size_t err_size);   // missing keys keep the defaults

// The HUD panel (outer rect = the screen image; the bezel is drawn around it). Pixels: safe is the
// HUD safe area, px = one 1080p design px (gui_s(1)). corner: 0 bottom-right, 1 bottom-left,
// 2 top-right, 3 top-left. The panel never leaves the safe area and, bottom-right, stays above
// the item / ammo readout.
PhoneRect phone_panel_rect(PhoneRect safe, float px, float screen_h, float size_frac, int corner, float aspect, const PhonePanelTokens *t);

// The companion page with window.DG_THEME / DG_APPS / DG_ICONS (data URI) replacing the
// /*DG_PHONE_DATA*/ marker (malloc'd, NUL-terminated, *out_len without the NUL). NULL inputs inject "null".
char *phone_page_build(const char *page, size_t page_len, const char *theme_json, const char *apps_json, const u8 *png, size_t png_len, size_t *out_len);

#endif
