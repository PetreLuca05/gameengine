// Phone OS (docs/PHONE.md "Phone flow"): pure, unit tested (tests/test_phone.c).
//   * the pairing flow: equip -> Connect portal -> paired device / in-game phone (decline,
//     timeout, no server) -> lost device (portal + Reconnect);
//   * the apps (portal, home, camera, gallery) and the gallery viewer;
//   * the screen layout in design px (640 x 1136 portrait), shared by the CPU canvas (phone_ui.c)
//     and the hit tests of the HUD panel;
//   * assets/phone/apps.json + theme.json, the HUD panel rectangle;
//   * the companion page with the phone assets injected (phone_page_build).
#ifndef DG_PHONE_OS_H
#define DG_PHONE_OS_H

#include "core/base.h"

#define PHONE_DESIGN_W 640.0f
#define PHONE_DESIGN_H 1136.0f

// Replicated (UserCmd.phone_app, the character state): keep the values stable.
typedef enum PhoneApp { PAPP_OFF, PAPP_PORTAL, PAPP_HOME, PAPP_CAMERA, PAPP_GALLERY, PAPP_COUNT } PhoneApp;
typedef enum PhoneFlow { PFLOW_IDLE, PFLOW_PAIRING, PFLOW_LOCAL, PFLOW_PAIRED, PFLOW_LOST } PhoneFlow;
const char *phone_app_name(int app);
int phone_app_from_name(const char *s);   // -1 unknown
const char *phone_flow_name(int flow);

typedef struct PhoneOs {
    PhoneFlow flow;
    PhoneApp app;              // the app on the screen (PORTAL while pairing / lost, OFF unequipped)
    PhoneApp last_app;         // where the phone returns after the portal / re-equip (HOME)
    bool equipped;
    float pair_t;              // seconds on the portal while pairing (timeout)
    char reason[64];           // why the phone fell back to the in-game phone ("" = declined)
    int viewer;                // gallery: -1 the grid, else the open item
    float viewer_zoom;         // 1..4
    int scroll;                // gallery: first visible row
    float battery;             // 0..1, cosmetic (drains while equipped)
} PhoneOs;

void phone_os_init(PhoneOs *os);
// Equipped (hotbar, wheel, X). Returns true when the caller must start the server and issue a
// pairing token (the portal is up). A paired / declined phone skips the portal (per session).
bool phone_os_equip(PhoneOs *os, bool auto_pair, bool device_connected);
void phone_os_unequip(PhoneOs *os);
void phone_os_server_failed(PhoneOs *os, const char *why);   // pairing -> the in-game phone
void phone_os_tick(PhoneOs *os, float dt, float pair_timeout, float drain_per_s);
void phone_os_connected(PhoneOs *os);
void phone_os_disconnected(PhoneOs *os);    // paired -> lost: the portal with Reconnect
void phone_os_decline(PhoneOs *os);         // "Use in-game phone instead"
void phone_os_cancel(PhoneOs *os);          // "Cancel": pairing stops (the caller lowers the phone)
bool phone_os_reconnect(PhoneOs *os);       // portal again; true: issue a new token
void phone_os_open(PhoneOs *os, PhoneApp app);   // home / camera / gallery (not while on the portal)
bool phone_os_back(PhoneOs *os);            // viewer -> grid -> home; false: nothing to go back to
bool phone_os_panel(const PhoneOs *os);     // the HUD panel shows: equipped, no device paired
bool phone_os_ui_only(PhoneApp app);        // the screen is UI only (no lens view)

// ------------------------------------------------------------------ layout (design px)
typedef struct PhoneRect { float x, y, w, h; } PhoneRect;
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
// /*DG_PHONE_DATA*/ marker (malloc'd, *out_len). NULL inputs inject "null".
char *phone_page_build(const char *page, size_t page_len, const char *theme_json, const char *apps_json, const u8 *png, size_t png_len, size_t *out_len);

#endif
