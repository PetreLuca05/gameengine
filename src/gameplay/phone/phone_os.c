// Phone OS: flow, apps, layout, assets (see phone_os.h, docs/PHONE.md "Phone flow").
#include "phone_os.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *k_apps[PAPP_COUNT] = { "off", "portal", "home", "camera", "gallery" };
static const char *k_flows[] = { "idle", "pairing", "local", "paired", "lost" };
static const char *k_modes[] = { "unset", "ingame", "device" };
static const char *k_cards[] = { "not_connected", "waiting", "connected", "ingame" };

const char *phone_app_name(int a) { return a >= 0 && a < PAPP_COUNT ? k_apps[a] : "?"; }
const char *phone_flow_name(int f) { return f >= 0 && f <= PFLOW_LOST ? k_flows[f] : "?"; }
const char *phone_mode_name(int m) { return m >= 0 && m <= PMODE_DEVICE ? k_modes[m] : "?"; }
const char *phone_card_name(int c) { return c >= 0 && c <= PCARD_INGAME ? k_cards[c] : "?"; }
int phone_app_from_name(const char *s) {
    for (int i = 0; s && i < PAPP_COUNT; i++)
        if (!strcmp(s, k_apps[i])) return i;
    return -1;
}
int phone_mode_from_name(const char *s) {
    for (int i = 0; s && i <= PMODE_DEVICE; i++)
        if (!strcmp(s, k_modes[i])) return i;
    return -1;
}

static void set_reason(PhoneOs *os, const char *r) { snprintf(os->reason, sizeof(os->reason), "%s", r ? r : ""); }

// ------------------------------------------------------------------ flow

// The flow follows the state: a device drives the phone, else the portal waits for one, else the
// held phone is the in-game phone.
static void refresh(PhoneOs *os) {
    if (os->device) os->flow = PFLOW_PAIRED;
    else if (os->portal) os->flow = PFLOW_PAIRING;
    else if (os->equipped) os->flow = PFLOW_LOCAL;
    else os->flow = PFLOW_IDLE;
}

static PhoneApp open_app(PhoneApp a) { return a == PAPP_PORTAL || a == PAPP_OFF ? PAPP_HOME : a; }

void phone_os_init(PhoneOs *os) {
    memset(os, 0, sizeof(*os));
    os->last_app = PAPP_HOME;
    os->viewer = -1;
    os->viewer_zoom = 1.0f;
    os->battery = 0.86f;
    os->portal_connected_t = -1.0f;
}

void phone_os_equip(PhoneOs *os) {
    os->equipped = true;
    os->app = open_app(os->last_app);
    refresh(os);
}

void phone_os_unequip(PhoneOs *os) {
    os->equipped = false;
    if (os->app != PAPP_OFF) os->last_app = open_app(os->app);
    os->app = PAPP_OFF;
    refresh(os);
}

bool phone_os_portal_open(PhoneOs *os) {
    os->portal = true;
    os->portal_t = 0;
    os->portal_connected_t = -1.0f;
    set_reason(os, "");
    refresh(os);
    return !os->device;
}

void phone_os_portal_close(PhoneOs *os) {
    os->portal = false;
    refresh(os);
}

void phone_os_server_failed(PhoneOs *os, const char *why) { set_reason(os, why && why[0] ? why : "The phone server could not start"); }

void phone_os_decline(PhoneOs *os) {
    if (os->device) return;
    os->mode = PMODE_INGAME;
    os->portal = false;
    set_reason(os, "");
    refresh(os);
}

void phone_os_connected(PhoneOs *os) {
    if (os->portal && !os->device) os->portal_connected_t = os->portal_t;
    os->device = true;
    os->mode = PMODE_DEVICE;
    set_reason(os, "");
    refresh(os);
}

bool phone_os_disconnected(PhoneOs *os) {
    if (!os->device) return false;
    os->device = false;
    set_reason(os, "Phone disconnected");
    refresh(os);
    return os->equipped;
}

void phone_os_disconnect(PhoneOs *os) {
    os->device = false;
    os->mode = PMODE_INGAME;
    set_reason(os, "");
    refresh(os);
}

void phone_os_forget(PhoneOs *os) {
    os->device = false;
    os->mode = PMODE_UNSET;
    set_reason(os, "");
    refresh(os);
}

void phone_os_tick(PhoneOs *os, float dt, float drain_per_s) {
    if (os->equipped) os->battery = fmaxf(0.05f, os->battery - drain_per_s * dt);
    if (os->portal) os->portal_t += dt;
}

void phone_os_open(PhoneOs *os, PhoneApp app) {
    if (!os->equipped || app <= PAPP_PORTAL || app >= PAPP_COUNT) return;
    if (app == PAPP_GALLERY && os->app != PAPP_GALLERY) { os->viewer = -1; os->viewer_zoom = 1.0f; }
    os->app = app;
    os->last_app = app;
}

bool phone_os_back(PhoneOs *os) {
    if (!os->equipped || os->app == PAPP_OFF) return false;
    if (os->app == PAPP_GALLERY && os->viewer >= 0) {
        os->viewer = -1;
        os->viewer_zoom = 1.0f;
        return true;
    }
    if (os->app == PAPP_HOME) return false;
    phone_os_open(os, PAPP_HOME);
    return true;
}

bool phone_os_panel(const PhoneOs *os) { return os->equipped && !os->device; }
bool phone_os_ui_only(PhoneApp app) { return app != PAPP_CAMERA; }

PhoneCard phone_os_card(const PhoneOs *os) {
    if (os->device) return PCARD_CONNECTED;
    if (os->portal) return PCARD_WAITING;
    if (os->mode == PMODE_INGAME) return PCARD_INGAME;
    return PCARD_NOT_CONNECTED;
}

static const char *find_ci(const char *s, const char *w) {
    size_t n = strlen(w);
    for (; s && *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)w[i])) i++;
        if (i == n) return s;
    }
    return NULL;
}

void phone_device_name(const char *ua, const char *model, char *out, size_t n) {
    if (!ua) ua = "";
    char dev[48] = "";
    // Android: "Linux; Android 14; Pixel 7 Build/..." or the reduced "Android 10; K" (no model).
    const char *an = find_ci(ua, "Android");
    if (model && model[0]) snprintf(dev, sizeof(dev), "%.40s", model);
    else if (find_ci(ua, "iPhone")) snprintf(dev, sizeof(dev), "iPhone");
    else if (find_ci(ua, "iPad") || (find_ci(ua, "Macintosh") && find_ci(ua, "Mobile/"))) snprintf(dev, sizeof(dev), "iPad");
    else if (an) {
        const char *semi = strchr(an, ';');
        if (semi) {
            const char *m = semi + 1;
            while (*m == ' ') m++;
            size_t len = strcspn(m, ";)");
            const char *b = find_ci(m, " Build/");
            if (b && (size_t)(b - m) < len) len = (size_t)(b - m);
            if (len > 1 && len < 40) snprintf(dev, sizeof(dev), "%.*s", (int)len, m);
        }
        if (!dev[0]) snprintf(dev, sizeof(dev), "%s", find_ci(ua, "Mobile") ? "Android phone" : "Android tablet");
    } else if (find_ci(ua, "Windows")) snprintf(dev, sizeof(dev), "Windows PC");
    else if (find_ci(ua, "Macintosh")) snprintf(dev, sizeof(dev), "Mac");
    else if (find_ci(ua, "Linux")) snprintf(dev, sizeof(dev), "Linux PC");
    else snprintf(dev, sizeof(dev), "Phone");
    const char *br = find_ci(ua, "SamsungBrowser") ? "Samsung Internet" : find_ci(ua, "Firefox") || find_ci(ua, "FxiOS") ? "Firefox"
                   : find_ci(ua, "Edg") ? "Edge" : find_ci(ua, "CriOS") || find_ci(ua, "Chrome") ? "Chrome" : find_ci(ua, "Safari") ? "Safari" : NULL;
    if (br) snprintf(out, n, "%s \xC2\xB7 %s", dev, br);
    else snprintf(out, n, "%s", dev);
}

char *phone_device_store_write(const PhoneDeviceStore *d) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "session", d->session);
    cJSON_AddStringToObject(j, "device", d->name);
    cJSON_AddNumberToObject(j, "paired", (double)d->paired);
    cJSON_AddNumberToObject(j, "expires", (double)d->expires);
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    return s;
}

bool phone_device_store_read(const char *json, PhoneDeviceStore *out, i64 now) {
    memset(out, 0, sizeof(*out));
    cJSON *j = json ? cJSON_Parse(json) : NULL;
    if (!j) return false;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "session"), *n = cJSON_GetObjectItemCaseSensitive(j, "device");
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "paired"), *e = cJSON_GetObjectItemCaseSensitive(j, "expires");
    bool ok = cJSON_IsString(s) && strlen(s->valuestring) > 0 && strlen(s->valuestring) < sizeof(out->session);
    if (ok) {
        snprintf(out->session, sizeof(out->session), "%s", s->valuestring);
        snprintf(out->name, sizeof(out->name), "%s", cJSON_IsString(n) ? n->valuestring : "");
        out->paired = cJSON_IsNumber(p) ? (i64)p->valuedouble : 0;
        out->expires = cJSON_IsNumber(e) ? (i64)e->valuedouble : 0;
        if (out->expires && now >= out->expires) ok = false;
    }
    cJSON_Delete(j);
    return ok;
}

// ------------------------------------------------------------------ layout

static int add(PhoneHit *out, int n, int max, int id, int index, float x, float y, float w, float h) {
    if (n >= max) return n;
    out[n].id = id;
    out[n].index = index;
    out[n].r = (PhoneRect){ x, y, w, h };
    return n + 1;
}

int phone_os_layout(const PhoneLayoutIn *in, PhoneHit *out, int max) {
    const float W = PHONE_DESIGN_W, H = PHONE_DESIGN_H;
    int n = 0;
    switch (in->app) {
    case PAPP_HOME: {
        int cols = in->columns > 0 ? in->columns : 4;
        float pad = 32, cw = (W - 2 * pad) / (float)cols, ch = 176, y0 = 520;
        for (int i = 0; i < in->app_count; i++) n = add(out, n, max, PB_APP, i, pad + (float)(i % cols) * cw, y0 + (float)(i / cols) * ch, cw, ch);
        if (in->flow == PFLOW_LOCAL) n = add(out, n, max, PB_CONNECT, 0, W * 0.5f - 170, H - 196, 340, 76);
        break;
    }
    case PAPP_CAMERA: {
        float cy = H - 176;
        n = add(out, n, max, PB_SHUTTER, 0, W * 0.5f - 56, cy - 56, 112, 112);
        n = add(out, n, max, PB_RECORD, 0, W - 110 - 36, cy - 36, 72, 72);
        n = add(out, n, max, PB_THUMB, 0, 40, cy - 50, 60, 100);
        n = add(out, n, max, PB_FLIP, 0, W - 110 - 30, cy - 150, 60, 60);
        for (int i = 0; i < 3; i++) n = add(out, n, max, PB_ZOOM_CHIP, i, W * 0.5f + (float)(i - 1) * 84 - 32, H - 352, 64, 64);
        n = add(out, n, max, PB_ZOOM_SLIDER, 0, W - 58, 150, 48, H - 600);
        break;
    }
    case PAPP_GALLERY:
        if (in->viewer >= 0) {
            n = add(out, n, max, PB_BACK, 0, 12, 72, 220, 64);
            n = add(out, n, max, PB_PREV, 0, 0, 160, 110, H - 420);
            n = add(out, n, max, PB_NEXT, 0, W - 110, 160, 110, H - 420);
            n = add(out, n, max, PB_DELETE, 0, W * 0.5f - 130, H - 216, 260, 80);
        } else {
            n = add(out, n, max, PB_BACK, 0, 12, 72, 200, 64);
            float pad = 20, gap = 8, cw = (W - 2 * pad - gap * (PHONE_GALLERY_COLS - 1)) / PHONE_GALLERY_COLS, ch = cw * 1.25f, y0 = 228;
            int first = DG_MAX(0, in->scroll) * PHONE_GALLERY_COLS;
            for (int i = first; i < in->gallery_count && i < first + PHONE_GALLERY_COLS * PHONE_GALLERY_ROWS; i++) {
                int k = i - first;
                float y = y0 + (float)(k / PHONE_GALLERY_COLS) * (ch + gap);
                if (y + ch > H - 84) break;
                n = add(out, n, max, PB_CELL, i, pad + (float)(k % PHONE_GALLERY_COLS) * (cw + gap), y, cw, ch);
            }
        }
        break;
    default: return 0;
    }
    n = add(out, n, max, PB_HOME, 0, W * 0.5f - 160, H - 70, 320, 70);
    return n;
}

int phone_os_hit(const PhoneHit *hits, int n, float x, float y) {
    for (int i = n - 1; i >= 0; i--) {
        const PhoneRect *r = &hits[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    return -1;
}

const PhoneHit *phone_os_find(const PhoneHit *hits, int n, int id, int index) {
    for (int i = 0; i < n; i++)
        if (hits[i].id == id && hits[i].index == index) return &hits[i];
    return NULL;
}

float phone_zoom_from_slider(float y, PhoneRect s, float max_zoom) {
    float t = (s.y + s.h - y) / fmaxf(s.h, 1.0f);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return powf(max_zoom, t);
}

float phone_slider_from_zoom(float zoom, PhoneRect s, float max_zoom) {
    float t = logf(fmaxf(zoom, 1.0f)) / logf(fmaxf(max_zoom, 1.0001f));
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return s.y + s.h - t * s.h;
}

// ------------------------------------------------------------------ assets

static void cpy(char *d, size_t n, const cJSON *j) { snprintf(d, n, "%s", cJSON_IsString(j) ? j->valuestring : ""); }

int phone_apps_tile(const PhoneApps *a, const char *name) {
    for (int i = 0; name && i < a->tile_count; i++)
        if (!strcmp(a->tiles[i], name)) return i;
    return -1;
}

int phone_apps_weather_tile(const PhoneApps *a, const char *weather, bool night) {
    const char *t = "w_partly";
    for (int i = 0; weather && i < a->wx_count; i++)
        if (!strcmp(a->wx[i].weather, weather)) { t = a->wx[i].tile; break; }
    if (night && !strcmp(t, "w_clear")) t = "w_night";
    return phone_apps_tile(a, t);
}

bool phone_apps_parse(const char *json, PhoneApps *out, char *err, size_t err_size) {
    memset(out, 0, sizeof(*out));
    out->columns = 4;
    out->tile_px = 128;
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { snprintf(err, err_size, "apps.json: parse error"); return false; }
    const cJSON *ic = cJSON_GetObjectItemCaseSensitive(j, "icons");
    cpy(out->atlas, sizeof(out->atlas), cJSON_GetObjectItemCaseSensitive(ic, "atlas"));
    const cJSON *tp = cJSON_GetObjectItemCaseSensitive(ic, "tile");
    if (cJSON_IsNumber(tp) && tp->valueint > 0) out->tile_px = tp->valueint;
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(ic, "tiles")) {
        if (out->tile_count >= PHONE_MAX_TILES) break;
        cpy(out->tiles[out->tile_count++], sizeof(out->tiles[0]), t);
    }
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j, "apps")) {
        if (out->count >= PHONE_MAX_APPS) break;
        PhoneAppDef *d = &out->apps[out->count];
        cpy(d->id, sizeof(d->id), cJSON_GetObjectItemCaseSensitive(a, "id"));
        cpy(d->label, sizeof(d->label), cJSON_GetObjectItemCaseSensitive(a, "label"));
        cpy(d->icon, sizeof(d->icon), cJSON_GetObjectItemCaseSensitive(a, "icon"));
        const cJSON *ap = cJSON_GetObjectItemCaseSensitive(a, "app");
        d->app = cJSON_IsString(ap) ? phone_app_from_name(ap->valuestring) : -1;
        if (d->app <= PAPP_HOME) d->app = -1;   // only real apps open from the grid
        d->tile = phone_apps_tile(out, d->icon);
        if (!d->id[0]) continue;
        out->count++;
    }
    const cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItemCaseSensitive(j, "weather_icons")) {
        if (out->wx_count >= (int)(sizeof(out->wx) / sizeof(out->wx[0])) || !cJSON_IsString(w) || !w->string) continue;
        snprintf(out->wx[out->wx_count].weather, sizeof(out->wx[0].weather), "%s", w->string);
        snprintf(out->wx[out->wx_count].tile, sizeof(out->wx[0].tile), "%s", w->valuestring);
        out->wx_count++;
    }
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "home_columns");
    if (cJSON_IsNumber(c) && c->valueint >= 2 && c->valueint <= 6) out->columns = c->valueint;
    cJSON_Delete(j);
    if (!out->count) { snprintf(err, err_size, "apps.json: no apps"); return false; }
    return true;
}

#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)

void phone_theme_default(PhoneTheme *t) {
    memset(t, 0, sizeof(*t));
    t->bg = RGBA(255, 255, 255, 255);
    t->surface = RGBA(242, 243, 246, 255);
    t->surface_hi = RGBA(226, 229, 235, 255);
    t->text = RGBA(20, 22, 26, 255);
    t->text_dim = RGBA(96, 102, 112, 255);
    t->text_faint = RGBA(160, 166, 176, 255);
    t->accent = RGBA(10, 122, 255, 255);
    t->accent_text = RGBA(255, 255, 255, 255);
    t->ok = RGBA(40, 170, 80, 255);
    t->warning = RGBA(230, 140, 20, 255);
    t->danger = RGBA(226, 52, 44, 255);
    t->home_bar = RGBA(20, 22, 26, 200);
    t->camera_ui = RGBA(255, 255, 255, 240);
    t->camera_accent = RGBA(255, 214, 10, 240);
    t->status_h = 64;
    t->pad = 32;
    t->radius = 28;
    t->button_h = 84;
    t->icon = 112;
    t->icon_label = 26;
    t->home_bar_w = 210;
    t->home_bar_h = 8;
    t->grid_gap = 26;
    t->panel = (PhonePanelTokens){ 104, 22, 7, 26, 10, 0.35f };
    t->bezel_color = RGBA(16, 17, 20, 245);
    t->bezel_edge = RGBA(255, 255, 255, 40);
    t->shadow_color = RGBA(0, 0, 0, 110);
}

static void col(const cJSON *o, const char *k, u32 *c) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 3) return;
    int v[4] = { 0, 0, 0, 255 };
    for (int i = 0; i < 4 && i < cJSON_GetArraySize(a); i++) v[i] = cJSON_GetArrayItem(a, i)->valueint;
    *c = RGBA(v[0] & 255, v[1] & 255, v[2] & 255, v[3] & 255);
}

static void num(const cJSON *o, const char *k, float *f) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) *f = (float)v->valuedouble;
}

bool phone_theme_parse(const char *json, PhoneTheme *t, char *err, size_t err_size) {
    phone_theme_default(t);
    cJSON *j = cJSON_Parse(json ? json : "");
    if (!j) { snprintf(err, err_size, "theme.json: parse error"); return false; }
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "colors");
    col(c, "bg", &t->bg); col(c, "surface", &t->surface); col(c, "surface_hi", &t->surface_hi);
    col(c, "text", &t->text); col(c, "text_dim", &t->text_dim); col(c, "text_faint", &t->text_faint);
    col(c, "accent", &t->accent); col(c, "accent_text", &t->accent_text); col(c, "ok", &t->ok);
    col(c, "warning", &t->warning); col(c, "danger", &t->danger); col(c, "home_bar", &t->home_bar);
    col(c, "camera_ui", &t->camera_ui); col(c, "camera_accent", &t->camera_accent);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "sizes");
    num(s, "status_h", &t->status_h); num(s, "pad", &t->pad); num(s, "radius", &t->radius); num(s, "button_h", &t->button_h);
    num(s, "icon", &t->icon); num(s, "icon_label", &t->icon_label); num(s, "home_bar_w", &t->home_bar_w);
    num(s, "home_bar_h", &t->home_bar_h); num(s, "grid_gap", &t->grid_gap);
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "panel");
    num(p, "readout_gap", &t->panel.readout_gap); num(p, "radius", &t->panel.radius); num(p, "bezel", &t->panel.bezel);
    num(p, "shadow", &t->panel.shadow); num(p, "hint_gap", &t->panel.hint_gap); num(p, "slide", &t->panel.slide);
    col(p, "bezel_color", &t->bezel_color); col(p, "bezel_edge", &t->bezel_edge); col(p, "shadow_color", &t->shadow_color);
    cJSON_Delete(j);
    return true;
}

PhoneRect phone_panel_rect(PhoneRect safe, float px, float screen_h, float size_frac, int corner, float aspect, const PhonePanelTokens *t) {
    float bez = t->bezel * px;
    float h = screen_h * (size_frac < 0.1f ? 0.1f : (size_frac > 0.5f ? 0.5f : size_frac));
    bool bottom = corner == 0 || corner == 1, right = corner == 0 || corner == 2;
    // Room: the safe area height, less the readout band bottom-right, less the bezels.
    float avail = safe.h - 2.0f * bez - (bottom && right ? t->readout_gap * px : 0.0f);
    if (h > avail) h = avail;
    float w = h * aspect;
    if (w > safe.w * 0.45f) { w = safe.w * 0.45f; h = w / aspect; }
    PhoneRect r;
    r.w = w;
    r.h = h;
    r.x = right ? safe.x + safe.w - bez - w : safe.x + bez;
    r.y = bottom ? safe.y + safe.h - bez - h - (right ? t->readout_gap * px : 0.0f) : safe.y + bez;
    return r;
}

// ------------------------------------------------------------------ page

static size_t b64(const u8 *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = i + 1 < n ? T[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

// JSON into a <script>: "</" must not appear (it would end the element).
static size_t put_json(char *o, const char *json) {
    size_t n = 0;
    if (!json || !json[0]) { memcpy(o, "null", 4); return 4; }
    for (const char *s = json; *s; s++) {
        if (s[0] == '<' && s[1] == '/') { o[n++] = '<'; o[n++] = '\\'; continue; }
        o[n++] = *s;
    }
    return n;
}

char *phone_page_build(const char *page, size_t page_len, const char *theme_json, const char *apps_json, const u8 *png, size_t png_len, size_t *out_len) {
    static const char marker[] = "/*DG_PHONE_DATA*/";
    const char *at = NULL;
    for (size_t i = 0; page && i + sizeof(marker) - 1 <= page_len; i++)
        if (!memcmp(page + i, marker, sizeof(marker) - 1)) { at = page + i; break; }
    size_t tl = theme_json ? strlen(theme_json) : 0, al = apps_json ? strlen(apps_json) : 0;
    size_t cap = page_len + 2 * (tl + al) + (png_len + 2) / 3 * 4 + 256;
    char *o = (char *)malloc(cap);
    if (!o) return NULL;
    size_t n = 0;
    if (!at) {
        memcpy(o, page, page_len);
        o[page_len] = 0;
        *out_len = page_len;
        return o;
    }
    size_t pre = (size_t)(at - page);
    memcpy(o, page, pre);
    n = pre;
    n += (size_t)snprintf(o + n, cap - n, "window.DG_THEME=");
    n += put_json(o + n, theme_json);
    n += (size_t)snprintf(o + n, cap - n, ";window.DG_APPS=");
    n += put_json(o + n, apps_json);
    n += (size_t)snprintf(o + n, cap - n, ";window.DG_ICONS=");
    if (png && png_len) {
        n += (size_t)snprintf(o + n, cap - n, "\"data:image/png;base64,");
        n += b64(png, png_len, o + n);
        o[n++] = '"';
    } else {
        n += (size_t)snprintf(o + n, cap - n, "null");
    }
    o[n++] = ';';
    size_t rest = page_len - pre - (sizeof(marker) - 1);
    memcpy(o + n, at + sizeof(marker) - 1, rest);
    n += rest;
    o[n] = 0;   // a C string too (out_len excludes the terminator)
    *out_len = n;
    return o;
}
