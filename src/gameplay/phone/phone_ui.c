// Phone UI (docs/PHONE.md):
//   * the phone OS screen, drawn on the CPU into an RGBA canvas (phone_tonemap.comp composites it
//     over the lens image in the camera app; the other apps are UI only): status bar, Connect
//     portal, home screen (clock, day, weather, app grid), camera, gallery grid + viewer. White
//     background, dark text (assets/phone/theme.json). The layout comes from phone_os_layout, the
//     same rects the HUD panel hit tests;
//   * the HUD: the phone panel (the simulated phone screen, bottom-right), key hints, toasts;
//   * the phone model (body, a dark glass screen, lens) for hands.c: the model never shows the
//     screen content (the user's call: the screen lives on the HUD panel / the real phone).
#include "phone_internal.h"
#include "assets/item_mesh.h"
#include "core/engine.h"
#include "core/log.h"
#include "core/platform.h"
#include "input/input.h"
#include "render/phone_view.h"
#include "render/mesh_gen.h"
#include "net/http_ws/hws_server.h"
#include "ui/gui/gui.h"
#include "ui/gui/gui_draw.h"
#include "ui/gui/gui_layout.h"
#include "ui/gui/gui_theme.h"
#include "ui/hud/hud_internal.h"
#include "ui/ui.h"
#include "gameplay/items.h"
#include "game/game.h"
#include "stb_image.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>

// ------------------------------------------------------------------ assets

static char *read_data(const char *rel, size_t *n) {
    char p[512];
    platform_data_path(rel, p, sizeof(p));
    return platform_read_file(p, n);
}

void phone_ui_load_assets(PhoneState *s) {
    if (s->assets_loaded) return;
    s->assets_loaded = true;
    char err[160] = "";
    size_t n;
    char *js = read_data("assets/phone/theme.json", &n);
    if (!js || !phone_theme_parse(js, &s->theme, err, sizeof(err))) {
        phone_theme_default(&s->theme);
        log_warn("phone: assets/phone/theme.json: %s (defaults)", js ? err : "missing");
    }
    free(js);
    js = read_data("assets/phone/apps.json", &n);
    if (!js || !phone_apps_parse(js, &s->apps, err, sizeof(err))) {
        log_warn("phone: assets/phone/apps.json: %s (built-in apps)", js ? err : "missing");
        phone_apps_parse("{\"apps\":[{\"id\":\"camera\",\"label\":\"Camera\",\"icon\":\"camera\",\"app\":\"camera\"},"
                         "{\"id\":\"gallery\",\"label\":\"Gallery\",\"icon\":\"gallery\",\"app\":\"gallery\"}],"
                         "\"icons\":{\"atlas\":\"assets/phone/icons.png\",\"tile\":128,\"tiles\":[\"camera\",\"gallery\"]}}",
                         &s->apps, err, sizeof(err));
    }
    free(js);
    char p[512];
    platform_data_path(s->apps.atlas[0] ? s->apps.atlas : "assets/phone/icons.png", p, sizeof(p));
    int w, h, c;
    s->icons_px = stbi_load(p, &w, &h, &c, 4);
    if (s->icons_px) { s->icons_w = w; s->icons_h = h; }
    else log_warn("phone: icon atlas %s missing", p);
}

// ------------------------------------------------------------------ CPU canvas

typedef struct Canvas { u8 *px; int w, h; float s; } Canvas;

#define FONT_COUNT 4
typedef struct FontBake { bool ok; float size; int aw; stbtt_bakedchar ch[96]; u8 *atlas; } FontBake;
static struct { bool tried; FontBake f[FONT_COUNT]; } UF;   // 0 small, 1 body, 2 large, 3 clock (design px at 640 wide)
static const float k_font_px[FONT_COUNT] = { 24, 32, 52, 136 };
static const int k_font_atlas[FONT_COUNT] = { 512, 512, 512, 1024 };

static void fonts_load(void) {
    if (UF.tried) return;
    UF.tried = true;
    char p[512];
    platform_data_path("assets/ui/fonts/Barlow-Medium.ttf", p, sizeof(p));
    size_t n;
    u8 *ttf = (u8 *)platform_read_file(p, &n);
    if (!ttf) { log_warn("phone: font %s missing (screen text off)", p); return; }
    for (int i = 0; i < FONT_COUNT; i++) {
        FontBake *f = &UF.f[i];
        f->size = k_font_px[i];
        f->aw = k_font_atlas[i];
        f->atlas = (u8 *)calloc((size_t)f->aw * (size_t)f->aw, 1);
        f->ok = f->atlas && stbtt_BakeFontBitmap(ttf, 0, k_font_px[i], f->atlas, f->aw, f->aw, 32, 96, f->ch) > 0;
    }
    free(ttf);
}

static void blend(u8 *d, u32 c, float a) {
    a *= (float)(c >> 24) / 255.0f;
    if (a <= 0.0f) return;
    float r = (float)(c & 255), g = (float)((c >> 8) & 255), b = (float)((c >> 16) & 255);
    if (a >= 0.999f) {
        d[0] = (u8)r; d[1] = (u8)g; d[2] = (u8)b; d[3] = 255;
        return;
    }
    float da = d[3] / 255.0f, oa = a + da * (1.0f - a);
    if (oa <= 1e-5f) return;
    d[0] = (u8)((r * a + d[0] * da * (1.0f - a)) / oa);
    d[1] = (u8)((g * a + d[1] * da * (1.0f - a)) / oa);
    d[2] = (u8)((b * a + d[2] * da * (1.0f - a)) / oa);
    d[3] = (u8)(oa * 255.0f + 0.5f);
}

static void c_fill(Canvas *c, u32 col) {
    u32 *p = (u32 *)c->px;
    size_t n = (size_t)c->w * (size_t)c->h;
    for (size_t i = 0; i < n; i++) p[i] = col;
}

static void c_rect(Canvas *c, float x, float y, float w, float h, u32 col) {
    int x0 = DG_MAX(0, (int)(x * c->s)), y0 = DG_MAX(0, (int)(y * c->s)), x1 = DG_MIN(c->w, (int)((x + w) * c->s + 0.5f)), y1 = DG_MIN(c->h, (int)((y + h) * c->s + 0.5f));
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) blend(c->px + ((size_t)yy * (size_t)c->w + (size_t)xx) * 4, col, 1.0f);
}

// Rounded rectangle / circle / ring with 1 px anti-aliasing (signed distance).
static void c_round(Canvas *c, float x, float y, float w, float h, float r, u32 col, float ring) {
    float S = c->s;
    x *= S; y *= S; w *= S; h *= S; r *= S; ring *= S;
    int x0 = DG_MAX(0, (int)x - 1), y0 = DG_MAX(0, (int)y - 1), x1 = DG_MIN(c->w, (int)(x + w) + 2), y1 = DG_MIN(c->h, (int)(y + h) + 2);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f - r, hy = h * 0.5f - r;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float px = fabsf((float)xx + 0.5f - cx) - hx, py = fabsf((float)yy + 0.5f - cy) - hy;
            float ox = fmaxf(px, 0.0f), oy = fmaxf(py, 0.0f);
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(px, py), 0.0f) - r;
            if (ring > 0) d = fabsf(d + ring * 0.5f) - ring * 0.5f;
            float a = saturate(0.5f - d);
            if (a > 0) blend(c->px + ((size_t)yy * (size_t)c->w + (size_t)xx) * 4, col, a);
        }
}

static void c_circle(Canvas *c, float cx, float cy, float r, u32 col, float ring) { c_round(c, cx - r, cy - r, r * 2, r * 2, r, col, ring); }

static float c_text_w(int f, const char *s) {
    if (!UF.f[f].ok) return 0;
    float x = 0, y = 0;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(UF.f[f].ch, UF.f[f].aw, UF.f[f].aw, *s - 32, &x, &y, &q, 1);
    }
    return x;
}

// align: 0 left, 1 centre, 2 right; y = baseline (design px).
static void c_text(Canvas *c, int f, float x, float y, int align, u32 col, const char *s) {
    if (!UF.f[f].ok) return;
    const FontBake *fb = &UF.f[f];
    float k = c->s;
    float tw = c_text_w(f, s);
    float px = x * k - (align == 1 ? tw * 0.5f : (align == 2 ? tw : 0.0f)) * k, py = y * k;
    float bx = 0, by = 0;
    int A = fb->aw;
    for (; *s; s++) {
        if ((unsigned char)*s < 32 || (unsigned char)*s >= 128) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(fb->ch, A, A, *s - 32, &bx, &by, &q, 1);
        int gw = (int)((q.x1 - q.x0) * k + 0.5f), gh = (int)((q.y1 - q.y0) * k + 0.5f);
        int ox = (int)(px + q.x0 * k), oy = (int)(py + q.y0 * k);
        for (int yy = 0; yy < gh; yy++) {
            int ty = oy + yy;
            if (ty < 0 || ty >= c->h) continue;
            float v = q.t0 + (q.t1 - q.t0) * ((float)yy + 0.5f) / (float)DG_MAX(gh, 1);
            int ay = DG_CLAMP((int)(v * (float)A), 0, A - 1);
            for (int xx = 0; xx < gw; xx++) {
                int tx = ox + xx;
                if (tx < 0 || tx >= c->w) continue;
                float u = q.s0 + (q.s1 - q.s0) * ((float)xx + 0.5f) / (float)DG_MAX(gw, 1);
                int ax = DG_CLAMP((int)(u * (float)A), 0, A - 1);
                float a = fb->atlas[ay * A + ax] / 255.0f;
                if (a > 0.0f) blend(c->px + ((size_t)ty * (size_t)c->w + (size_t)tx) * 4, col, a);
            }
        }
    }
}

// A source rect (sx0, sy0, sw, sh of an iw x ih RGBA image) into a design rect, nearest.
static void c_image_src(Canvas *c, const u8 *img, int iw, int ih, float sx0, float sy0, float sw, float sh, float x, float y, float w, float h, float alpha, u32 tint) {
    int x0 = (int)(x * c->s), y0 = (int)(y * c->s), dw = (int)(w * c->s), dh = (int)(h * c->s);
    float tr = tint ? (float)(tint & 255) / 255.0f : 1, tg = tint ? (float)((tint >> 8) & 255) / 255.0f : 1, tb = tint ? (float)((tint >> 16) & 255) / 255.0f : 1;
    for (int yy = 0; yy < dh; yy++) {
        int ty = y0 + yy;
        if (ty < 0 || ty >= c->h) continue;
        int sy = DG_CLAMP((int)(sy0 + ((float)yy + 0.5f) * sh / (float)DG_MAX(dh, 1)), 0, ih - 1);
        for (int xx = 0; xx < dw; xx++) {
            int tx = x0 + xx;
            if (tx < 0 || tx >= c->w) continue;
            int sx = DG_CLAMP((int)(sx0 + ((float)xx + 0.5f) * sw / (float)DG_MAX(dw, 1)), 0, iw - 1);
            const u8 *s = img + ((size_t)sy * (size_t)iw + (size_t)sx) * 4;
            u32 col = (u32)(s[0] * tr) | (u32)(s[1] * tg) << 8 | (u32)(s[2] * tb) << 16 | 0xFF000000u;
            blend(c->px + ((size_t)ty * (size_t)c->w + (size_t)tx) * 4, col, alpha * s[3] / 255.0f);
        }
    }
}

static void c_image(Canvas *c, const u8 *img, int sw, int sh, float x, float y, float w, float h, float alpha) {
    c_image_src(c, img, sw, sh, 0, 0, (float)sw, (float)sh, x, y, w, h, alpha, 0);
}

// An icon atlas tile (assets/phone/icons.png); tint 0 = as authored (app icons), else multiplies.
static void c_icon(Canvas *c, PhoneState *s, int tile, float x, float y, float size, u32 tint) {
    if (!s->icons_px || tile < 0) return;
    int t = s->apps.tile_px > 0 ? s->apps.tile_px : 128;
    if ((tile + 1) * t > s->icons_w) return;
    c_image_src(c, s->icons_px, s->icons_w, s->icons_h, (float)(tile * t), 0, (float)t, (float)DG_MIN(t, s->icons_h), x, y, size, size, 1.0f, tint);
}

#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
static u32 with_a(u32 c, int a) { return (c & 0x00FFFFFFu) | ((u32)DG_CLAMP(a, 0, 255) << 24); }

void phone_ui_thumb(const u8 *rgba, int w, int h, int stride, u8 *out) {
    // Fit (cover) into PHONE_THUMB_W x PHONE_THUMB_H, box filtered.
    float sx = (float)w / PHONE_THUMB_W, sy = (float)h / PHONE_THUMB_H, s = fminf(sx, sy);
    float ox = ((float)w - s * PHONE_THUMB_W) * 0.5f, oy = ((float)h - s * PHONE_THUMB_H) * 0.5f;
    int k = DG_MAX(1, (int)s);
    for (int y = 0; y < PHONE_THUMB_H; y++)
        for (int x = 0; x < PHONE_THUMB_W; x++) {
            int x0 = (int)(ox + (float)x * s), y0 = (int)(oy + (float)y * s);
            u32 acc[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < DG_MIN(h, y0 + k); yy += DG_MAX(1, k / 4))
                for (int xx = x0; xx < DG_MIN(w, x0 + k); xx += DG_MAX(1, k / 4)) {
                    const u8 *p = rgba + (size_t)yy * (size_t)stride + (size_t)xx * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; n++;
                }
            u8 *d = out + ((size_t)y * PHONE_THUMB_W + (size_t)x) * 4;
            for (int c = 0; c < 3; c++) d[c] = (u8)(n ? acc[c] / n : 0);
            d[3] = 255;
        }
}

// Gallery thumbnails: at most one decoded per screen rebuild (JPEG decode is a few ms).
static PhoneThumb *thumb_for(PhoneState *s, const PhoneGalleryItem *it, bool *loaded_one) {
    for (int i = 0; i < PHONE_GALLERY_THUMBS; i++)
        if (s->thumbs[i].path[0] && !strcmp(s->thumbs[i].path, it->path)) return s->thumbs[i].loaded ? &s->thumbs[i] : NULL;
    if (*loaded_one) return NULL;
    *loaded_one = true;
    int slot = PHONE_GALLERY_THUMBS - 1;
    for (int i = 0; i < PHONE_GALLERY_THUMBS; i++)
        if (!s->thumbs[i].path[0]) { slot = i; break; }
    PhoneThumb *t = &s->thumbs[slot];
    memset(t, 0, sizeof(*t));
    dg_strcpy(t->path, sizeof(t->path), it->path);
    t->video = it->video;
    t->seconds = it->meta.seconds;
    int w, h, n;
    u8 *px = stbi_load(it->thumb, &w, &h, &n, 4);
    if (px) {
        phone_ui_thumb(px, w, h, w * 4, t->px);
        stbi_image_free(px);
        t->loaded = true;
    }
    return t->loaded ? t : NULL;
}

// The gallery viewer's image (the photo, or a video's thumbnail), decoded when it changes.
static bool viewer_image(PhoneState *s, const PhoneGalleryItem *it) {
    const char *path = it->video ? it->thumb : it->path;
    if (s->viewer_px && !strcmp(s->viewer_path, path)) return true;
    if (s->viewer_px) stbi_image_free(s->viewer_px);
    s->viewer_px = NULL;
    dg_strcpy(s->viewer_path, sizeof(s->viewer_path), path);
    int w, h, n;
    s->viewer_px = stbi_load(path, &w, &h, &n, 4);
    s->viewer_w = w;
    s->viewer_h = h;
    return s->viewer_px != NULL;
}

// ------------------------------------------------------------------ screen UI

static const char *mmss(double s, char *b, size_t n) {
    int t = (int)s;
    snprintf(b, n, "%d:%02d", t / 60, t % 60);
    return b;
}

static float game_hours(void) {
    World *w = game_world();
    return w ? world_time_hours(w) : 12.0f;
}

static void status_bar(PhoneState *s, const PhoneScreenView *v, Canvas *c, float W, bool light) {
    const PhoneTheme *T = &s->theme;
    u32 fg = light ? RGBA(255, 255, 255, 240) : T->text;
    if (light)
        for (int i = 0; i < 70; i++) c_rect(c, 0, (float)i, W, 1, RGBA(0, 0, 0, (int)(110.0f * (1.0f - (float)i / 70.0f))));
    char clock[16];
    float h = game_hours();
    snprintf(clock, sizeof(clock), "%02d:%02d", (int)h % 24, (int)((h - floorf(h)) * 60.0f));
    c_text(c, 1, 30, 44, 0, fg, clock);
    // Battery (cosmetic, drains slowly while the phone is out: a later mechanic hook).
    float bat = s->os.battery;
    float bx = W - 76;
    c_round(c, bx, 22, 44, 22, 6, with_a(fg, 210), 2.5f);
    c_rect(c, bx + 44, 29, 4, 8, with_a(fg, 190));
    c_round(c, bx + 4, 26, 36 * bat, 14, 3, bat < 0.2f ? T->danger : fg, 0);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(bat * 100.0f + 0.5f));
    c_text(c, 0, bx - 10, 41, 2, fg, pct);
    // Signal (cosmetic) left of the battery percentage.
    float sx = bx - 10 - c_text_w(0, pct) - 64;
    for (int i = 0; i < 4; i++) c_round(c, sx + (float)i * 11, 40 - 5 - (float)i * 4, 7, 5 + (float)i * 4, 2, i < 3 ? fg : with_a(fg, 90), 0);
    // Link to the real phone / recording.
    if (v->flow == PFLOW_PAIRED && !v->rec) {
        c_round(c, W * 0.5f - 40, 20, 80, 28, 14, with_a(T->ok, 230), 0);
        c_text(c, 0, W * 0.5f, 42, 1, RGBA(255, 255, 255, 255), "LINK");
    }
    if (v->rec) {
        char t[24];
        c_round(c, W * 0.5f - 80, 16, 160, 34, 17, RGBA(220, 30, 30, 230), 0);
        c_circle(c, W * 0.5f - 56, 33, 7, RGBA(255, 255, 255, 255), 0);
        c_text(c, 1, W * 0.5f + 10, 44, 1, RGBA(255, 255, 255, 255), mmss((g_engine.real_time - s->rec_start), t, sizeof(t)));
    }
}

static void home_bar(PhoneState *s, Canvas *c, float W, float H, bool light) {
    const PhoneTheme *T = &s->theme;
    float bw = T->home_bar_w, bh = T->home_bar_h;
    c_round(c, W * 0.5f - bw * 0.5f, H - 30, bw, bh, bh * 0.5f, light ? RGBA(255, 255, 255, 220) : T->home_bar, 0);
}

static void hover_mark(Canvas *c, const PhoneHit *h, u32 col) { c_round(c, h->r.x, h->r.y, h->r.w, h->r.h, fminf(28.0f, h->r.h * 0.5f), col, 0); }

static const char *weather_label(const char *n) {
    static const struct { const char *id, *label; } k[] = {
        { "clear", "Clear" }, { "little_cloudy", "Partly cloudy" }, { "cloudy", "Cloudy" }, { "overcast", "Overcast" }, { "fog", "Fog" },
        { "light_rain", "Light rain" }, { "rain", "Rain" }, { "storm", "Storm" }, { "light_snow", "Light snow" }, { "snow", "Snow" },
        { "blizzard", "Blizzard" }, { "indoor", "Indoors" },
    };
    for (size_t i = 0; i < ARRAY_COUNT(k); i++)
        if (!strcmp(n, k[i].id)) return k[i].label;
    return n;
}

static const char *daypart(float h) { return h < 5 ? "Night" : h < 12 ? "Morning" : h < 17 ? "Afternoon" : h < 21 ? "Evening" : "Night"; }

static void home_ui(PhoneState *s, const PhoneScreenView *v, Canvas *c, const PhoneHit *hits, int nh, float W) {
    const PhoneTheme *T = &s->theme;
    World *w = game_world();
    float h = game_hours();
    char t[48];
    snprintf(t, sizeof(t), "%02d:%02d", (int)h % 24, (int)((h - floorf(h)) * 60.0f));
    c_text(c, 3, W * 0.5f, 300, 1, T->text, t);
    // (the screen font bake is ASCII only)
    snprintf(t, sizeof(t), "Day %d  -  %s", w ? world_day_number(w) + 1 : 1, daypart(h));
    c_text(c, 1, W * 0.5f, 356, 1, T->text_dim, t);
    // Weather from the weather system (glyph + label).
    const char *wn = w ? weather_name(w) : "clear";
    int tile = phone_apps_weather_tile(&s->apps, wn, h < 6.0f || h > 19.5f);
    const char *wl = weather_label(wn);
    float lw = c_text_w(1, wl), gx = W * 0.5f - (lw + 64) * 0.5f;
    c_icon(c, s, tile, gx, 386, 52, T->text_dim);
    c_text(c, 1, gx + 64, 424, 0, T->text_dim, wl);
    for (int i = 0; i < nh; i++) {
        const PhoneHit *hh = &hits[i];
        if (hh->id == PB_APP && hh->index < s->apps.count) {
            const PhoneAppDef *a = &s->apps.apps[hh->index];
            float ic = T->icon, ix = hh->r.x + (hh->r.w - ic) * 0.5f, iy = hh->r.y + 8;
            if (v->hover == i) c_round(c, ix - 8, iy - 8, ic + 16, ic + 16, 34, RGBA(0, 0, 0, 20), 0);
            if (a->tile >= 0 && s->icons_px) c_icon(c, s, a->tile, ix, iy, ic, 0);
            else c_round(c, ix, iy, ic, ic, 28, T->surface_hi, 0);
            c_text(c, 0, hh->r.x + hh->r.w * 0.5f, iy + ic + 36, 1, T->text, a->label);
        } else if (hh->id == PB_CONNECT) {
            c_round(c, hh->r.x, hh->r.y, hh->r.w, hh->r.h, hh->r.h * 0.5f, v->hover == i ? T->surface_hi : T->surface, 0);
            c_text(c, 0, hh->r.x + hh->r.w * 0.5f, hh->r.y + hh->r.h * 0.5f + 9, 1, T->accent, "Connect your phone");
        }
    }
}

static void camera_ui(PhoneState *s, const PhoneScreenView *v, Canvas *c, const PhoneHit *hits, int nh, float W, float H) {
    const PhoneTheme *T = &s->theme;
    u32 ui = T->camera_ui, acc = T->camera_accent;
    // Bottom band.
    for (int i = 0; i < 260; i++) c_rect(c, 0, H - (float)i, W, 1, RGBA(0, 0, 0, (int)(125.0f * (1.0f - (float)i / 260.0f))));
    static const float presets[3] = { 1, 2, 5 };
    for (int i = 0; i < nh; i++) {
        const PhoneHit *h = &hits[i];
        float cx = h->r.x + h->r.w * 0.5f, cy = h->r.y + h->r.h * 0.5f;
        bool hv = v->hover == i;
        switch (h->id) {
        case PB_SHUTTER:
            c_circle(c, cx, cy, 54, ui, 7);
            if (v->rec) c_round(c, cx - 24, cy - 24, 48, 48, 9, RGBA(230, 40, 40, 255), 0);
            else c_circle(c, cx, cy, 42, hv ? RGBA(255, 255, 255, 255) : with_a(ui, 220), 0);
            break;
        case PB_RECORD:
            c_circle(c, cx, cy, 30, ui, 4);
            c_circle(c, cx, cy, hv ? 20.0f : 18.0f, RGBA(230, 40, 40, 255), 0);
            break;
        case PB_THUMB:
            if (s->have_last_thumb) {
                c_round(c, h->r.x + 4, h->r.y + 4, 52, 92, 8, ui, 0);
                c_image(c, s->last_thumb, PHONE_THUMB_W, PHONE_THUMB_H, h->r.x + 7, h->r.y + 7, 46, 86, 1.0f);
            } else {
                c_round(c, h->r.x + 4, h->r.y + 4, 52, 92, 8, with_a(ui, 160), 3);
            }
            break;
        case PB_FLIP:
            c_circle(c, cx, cy, 28, hv ? RGBA(0, 0, 0, 170) : RGBA(0, 0, 0, 120), 0);
            c_circle(c, cx, cy, 14, ui, 3);
            c_rect(c, cx - 18, cy - 2, 8, 4, ui);
            c_rect(c, cx + 10, cy - 2, 8, 4, ui);
            break;
        case PB_ZOOM_CHIP: {
            bool on = fabsf(v->zoom - presets[h->index]) < 0.05f;
            c_circle(c, cx, cy, 30, on ? acc : (hv ? RGBA(0, 0, 0, 170) : RGBA(0, 0, 0, 120)), 0);
            char t[16];
            snprintf(t, sizeof(t), "%gx", presets[h->index]);
            c_text(c, 0, cx, cy + 9, 1, on ? RGBA(20, 20, 20, 255) : ui, t);
        } break;
        case PB_ZOOM_SLIDER: {
            c_round(c, cx - 3, h->r.y, 6, h->r.h, 3, RGBA(255, 255, 255, 90), 0);
            float ky = phone_slider_from_zoom(v->zoom, h->r, 8.0f);
            c_circle(c, cx, ky, hv ? 17.0f : 14.0f, acc, 0);
        } break;
        default: break;
        }
    }
    bool preset = false;
    for (int i = 0; i < 3; i++) preset |= fabsf(v->zoom - presets[i]) < 0.05f;
    if (!preset) {
        char t[12];
        snprintf(t, sizeof(t), "%.1fx", v->zoom);
        c_round(c, W * 0.5f - 46, H - 440, 92, 40, 20, RGBA(0, 0, 0, 150), 0);
        c_text(c, 1, W * 0.5f, H - 409, 1, acc, t);
    }
    if (v->front) {
        c_round(c, W * 0.5f - 54, 80, 108, 30, 15, acc, 0);
        c_text(c, 0, W * 0.5f, 102, 1, RGBA(20, 20, 20, 255), "SELFIE");
    }
    const char *src = v->fake ? "FAKE GYRO" : (v->driven ? "GYRO" : NULL);
    if (src) {
        c_round(c, 20, 80, c_text_w(0, src) + 24, 30, 15, RGBA(0, 0, 0, 120), 0);
        c_text(c, 0, 32, 102, 0, ui, src);
    }
}

static void gallery_ui(PhoneState *s, const PhoneScreenView *v, Canvas *c, const PhoneHit *hits, int nh, float W, float H) {
    const PhoneTheme *T = &s->theme;
    int count = s->gallery.count;
    if (v->viewer >= 0 && v->viewer < count) {
        // Viewer: the image fitted (zoom = a centred crop), back / prev / next / delete.
        float ax = 0, ay = 160, aw = W, ah = H - 420;
        c_rect(c, ax, ay, aw, ah, T->surface);
        const PhoneGalleryItem *it = &s->gallery.items[v->viewer];
        if (it && viewer_image(s, it)) {
            float z = fmaxf(1.0f, s->os.viewer_zoom);
            float iw = (float)s->viewer_w, ih = (float)s->viewer_h;
            float sw = iw / z, sh = ih / z;
            float k = fminf(aw / sw, ah / sh), dw = sw * k, dh = sh * k;
            c_image_src(c, s->viewer_px, s->viewer_w, s->viewer_h, (iw - sw) * 0.5f, (ih - sh) * 0.5f, sw, sh, ax + (aw - dw) * 0.5f, ay + (ah - dh) * 0.5f, dw, dh, 1.0f, 0);
            if (it->video) {
                c_circle(c, W * 0.5f, ay + ah * 0.5f, 46, RGBA(0, 0, 0, 140), 0);
                c_circle(c, W * 0.5f + 5, ay + ah * 0.5f, 18, RGBA(255, 255, 255, 235), 0);
            }
            char t[96];
            snprintf(t, sizeof(t), "%s%s", it->meta.game_time[0] ? it->meta.game_time : it->name, it->video ? "  -  video" : "");
            c_text(c, 0, W * 0.5f, ay + ah + 44, 1, T->text_dim, t);
            if (z > 1.01f) {
                snprintf(t, sizeof(t), "%.1fx", z);
                c_text(c, 0, W - 30, ay + 40, 2, T->text_dim, t);
            }
        }
        if (v->flow == PFLOW_PAIRED) c_text(c, 0, W * 0.5f, H - 100, 1, T->text_faint, "Download it from the page on your phone");
        for (int i = 0; i < nh; i++) {
            const PhoneHit *h = &hits[i];
            bool hv = v->hover == i;
            if (h->id == PB_BACK) {
                if (hv) hover_mark(c, h, RGBA(0, 0, 0, 18));
                c_text(c, 1, h->r.x + 20, h->r.y + 44, 0, T->accent, "< Gallery");
            } else if (h->id == PB_PREV && v->viewer > 0) {
                c_circle(c, h->r.x + 50, h->r.y + h->r.h * 0.5f, hv ? 32.0f : 28.0f, RGBA(255, 255, 255, 200), 0);
                c_text(c, 2, h->r.x + 48, h->r.y + h->r.h * 0.5f + 18, 1, T->text, "<");
            } else if (h->id == PB_NEXT && v->viewer + 1 < count) {
                c_circle(c, h->r.x + 60, h->r.y + h->r.h * 0.5f, hv ? 32.0f : 28.0f, RGBA(255, 255, 255, 200), 0);
                c_text(c, 2, h->r.x + 62, h->r.y + h->r.h * 0.5f + 18, 1, T->text, ">");
            } else if (h->id == PB_DELETE) {
                c_round(c, h->r.x, h->r.y, h->r.w, h->r.h, h->r.h * 0.5f, hv ? with_a(T->danger, 255) : with_a(T->danger, 225), 0);
                c_text(c, 1, h->r.x + h->r.w * 0.5f, h->r.y + h->r.h * 0.5f + 11, 1, RGBA(255, 255, 255, 255), "Delete");
            }
        }
        return;
    }
    c_text(c, 2, W * 0.5f, 120, 1, T->text, "Gallery");
    char t[48];
    snprintf(t, sizeof(t), "%d %s", count, count == 1 ? "item" : "items");
    c_text(c, 0, W - 28, 116, 2, T->text_dim, t);
    bool loaded = false;
    for (int i = 0; i < nh; i++) {
        const PhoneHit *h = &hits[i];
        if (h->id == PB_BACK) {
            if (v->hover == i) hover_mark(c, h, RGBA(0, 0, 0, 18));
            c_text(c, 1, h->r.x + 20, h->r.y + 44, 0, T->accent, "< Home");
            continue;
        }
        if (h->id != PB_CELL) continue;
        float x = h->r.x, y = h->r.y, cw = h->r.w, ch = h->r.h;
        PhoneThumb *th = thumb_for(s, &s->gallery.items[h->index], &loaded);
        if (th) c_image(c, th->px, PHONE_THUMB_W, PHONE_THUMB_H, x, y, cw, ch, 1.0f);
        else c_rect(c, x, y, cw, ch, T->surface_hi);
        if (v->hover == i) c_round(c, x, y, cw, ch, 4, RGBA(255, 255, 255, 60), 0);
        if (s->gallery.items[h->index].video) {
            c_circle(c, x + cw * 0.5f, y + ch * 0.5f, 26, RGBA(0, 0, 0, 140), 0);
            c_circle(c, x + cw * 0.5f + 3, y + ch * 0.5f, 10, RGBA(255, 255, 255, 230), 0);
            char d[16];
            c_text(c, 0, x + cw - 12, y + ch - 12, 2, RGBA(255, 255, 255, 240), mmss(s->gallery.items[h->index].meta.seconds, d, sizeof(d)));
        }
    }
    if (!count) c_text(c, 1, W * 0.5f, H * 0.45f, 1, T->text_dim, "No photos yet");
}

u32 phone_ui_hash(const PhoneState *s, const PhoneScreenView *v) {
    World *w = game_world();
    u32 h = 2166136261u;
    u32 vals[24] = { (u32)v->app, (u32)v->flow, (u32)(v->viewer + 1), (u32)v->scroll, (u32)(v->hover + 1), (u32)lrintf(v->zoom * 50.0f),
                     v->rec ? (u32)(g_engine.real_time - s->rec_start) + 1 : 0, (u32)v->front | (u32)v->landscape << 1 | (u32)v->landscape_neg << 2 | (u32)v->driven << 3 | (u32)v->fake << 4,
                     (u32)s->token_left, (u32)s->gallery.count, w ? (u32)(world_time_hours(w) * 60.0f) : 0, (u32)s->have_last_thumb,
                     (u32)(s->server_err[0] != 0), (u32)(s->token[0] != 0), (u32)(s->os.battery * 100.0f), (u32)lrintf(s->os.viewer_zoom * 20.0f),
                     0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 24; i++) { h ^= vals[i]; h *= 16777619u; }
    return h;
}

void phone_ui_build_screen(PhoneState *s, const PhoneScreenView *v, u8 *rgba, int w, int h) {
    fonts_load();
    phone_ui_load_assets(s);
    memset(rgba, 0, (size_t)w * (size_t)h * 4);
    if (v->app == PAPP_OFF) return;
    // Landscape camera: the UI is drawn upright for the rotated phone into a landscape canvas,
    // then rotated into the portrait screen (no hit tests: landscape comes with a real phone).
    bool land = v->landscape && v->app == PAPP_CAMERA;
    int cw = land ? h : w, ch = land ? w : h;
    u8 *buf = land ? (u8 *)calloc((size_t)cw * (size_t)ch, 4) : rgba;
    if (!buf) return;
    Canvas c = { buf, cw, ch, (float)DG_MIN(cw, ch) / PHONE_DESIGN_W };
    float W = (float)cw / c.s, H = (float)ch / c.s;
    PhoneLayoutIn li = { v->app, v->flow, v->viewer, v->scroll, s->apps.count, s->apps.columns, s->gallery.count };
    PhoneHit hits[PHONE_MAX_HITS];
    int nh = land ? 0 : phone_os_layout(&li, hits, PHONE_MAX_HITS);
    bool light = v->app == PAPP_CAMERA;
    if (!light) c_fill(&c, s->theme.bg);
    switch (v->app) {
    case PAPP_HOME: home_ui(s, v, &c, hits, nh, W); break;
    case PAPP_GALLERY: gallery_ui(s, v, &c, hits, nh, W, H); break;
    case PAPP_CAMERA: camera_ui(s, v, &c, hits, nh, W, H); break;
    default: break;
    }
    status_bar(s, v, &c, W, light);
    home_bar(s, &c, W, H, light);
    // Gamepad focus ring (the HUD panel's navigation).
    if (s->nav_hold && s->nav >= 0 && s->nav < nh) {
        const PhoneHit *f = &hits[s->nav];
        c_round(&c, f->r.x - 4, f->r.y - 4, f->r.w + 8, f->r.h + 8, fminf(30.0f, f->r.h * 0.5f + 4), light ? s->theme.camera_accent : s->theme.accent, 4);
    }
    if (land) {
        for (int ly = 0; ly < ch; ly++)
            for (int lx = 0; lx < cw; lx++) {
                int px = v->landscape_neg ? ly : w - 1 - ly, py = v->landscape_neg ? h - 1 - lx : lx;
                memcpy(rgba + ((size_t)py * (size_t)w + (size_t)px) * 4, buf + ((size_t)ly * (size_t)cw + (size_t)lx) * 4, 4);
            }
        free(buf);
    }
}

// ------------------------------------------------------------------ HUD

static float key_hint(float x, float y, Action a, const char *fallback, const char *what) {
    const GuiTheme *th = gui_theme();
    char key[32];
    binding_name(input_get_binding(a, 0), key, sizeof(key));
    if (!key[0] || !strcmp(key, "?")) dg_strcpy(key, sizeof(key), fallback);
    GuiText t = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dim, GUI_RIGHT, th->tracking_caps * 1.3f, true, true, 0 };
    float tw = gd_text_width(&t, what);
    gd_text(&t, x, y + gui_s(3), what);
    GuiText kt = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dark, GUI_CENTER, 0, false, false, 0 };
    float ks = gui_s(18), kw = fmaxf(ks, gd_text_width(&kt, key) + gui_s(8));
    GuiRect kr = { x - tw - gui_s(6) - kw, y, kw, ks };
    gd_round(kr, gui_s(3), UI_RGBA(236, 236, 230, 225));
    gd_text(&kt, kr.x + kr.w * 0.5f, kr.y + (ks - gd_line_height(kt.size)) * 0.5f, key);
    return kr.x - gui_s(14);
}

bool phone_ui_panel_rect(PhoneState *s, PhoneRect *out) {
    phone_ui_load_assets(s);
    LayoutParams lp = gui_layout_params();
    GuiRect sr = gui_layout_safe_rect(&lp);
    PhoneRect safe = { sr.x, sr.y, sr.w, sr.h };
    float aspect = (float)s->cv.res_w->i / (float)DG_MAX(1, s->cv.res_h->i);
    *out = phone_panel_rect(safe, gui_s(1.0f), ui_height(), s->cv.hud_size->f, s->cv.hud_corner->i, aspect, &s->theme.panel);
    return out->w > 8 && out->h > 8;
}

static void draw_panel(PhoneState *s) {
    PhoneRect r;
    if (s->panel_t <= 0.001f || !phone_ui_panel_rect(s, &r)) return;
    const PhoneTheme *T = &s->theme;
    float t = s->panel_t, e = t * t * (3.0f - 2.0f * t);
    float bez = gui_s(T->panel.bezel), rad = gui_s(T->panel.radius);
    bool right = s->cv.hud_corner->i == 0 || s->cv.hud_corner->i == 2;
    float slide = (1.0f - e) * (r.w + bez * 2.0f + gui_s(40)) * (right ? 1.0f : -1.0f);
    GuiRect sc = { r.x + slide, r.y, r.w, r.h };
    GuiRect outer = { sc.x - bez, sc.y - bez, sc.w + bez * 2, sc.h + bez * 2 };
    s->px_x = sc.x; s->px_y = sc.y; s->px_w = sc.w; s->px_h = sc.h;
    gd_alpha_push(hud_cvars()->opacity->f * e);
    gd_shadow(outer, rad + bez, gui_s(T->panel.shadow), T->shadow_color);
    gd_round(outer, rad + bez, T->bezel_color);
    gd_round_outline(outer, rad + bez, gui_s(1), T->bezel_edge);
    TextureId tex = render_phone_panel(0);
    if (tex) gd_image(sc, tex, v4(0, 0, 1, 1), UI_RGBA(255, 255, 255, 255));
    else gd_round(sc, rad, UI_RGBA(255, 255, 255, 255));
    if (s->ui_capture) gd_round_outline(outer, rad + bez, gui_s(2), gui_theme()->accent);
    // Hint above the panel: how to use it.
    const GuiTheme *th = gui_theme();
    float hy = outer.y - gui_s(T->panel.hint_gap) - gui_s(18), hx = outer.x + outer.w;
    if (!right) hx = outer.x + outer.w;
    if (s->ui_capture) {
        float x = key_hint(hx, hy, ACT_PHONE_UI, "ALT", "Release");
        GuiText bt = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dim, GUI_RIGHT, th->tracking_caps * 1.3f, true, true, 0 };
        gd_text(&bt, x + gui_s(8), hy + gui_s(3), "Esc  Back");
    } else {
        key_hint(hx, hy, ACT_PHONE_UI, "ALT", "Use phone");
    }
    gd_alpha_pop();
}

// Toast (2.5 s): also without the phone out ("Phone disconnected - using in-game phone").
static void draw_toast(PhoneState *s) {
    if (!s->toast[0] || g_engine.real_time - s->toast_t >= 2.5) return;
    const GuiTheme *th = gui_theme();
    float W = ui_width(), H = ui_height();
    float a = saturate((float)((2.5 - (g_engine.real_time - s->toast_t)) / 0.4));
    GuiText tt = { gui_s(th->t_small), GW_SEMIBOLD, gui_rgba_mul(th->text, a), GUI_CENTER, 0, false, true, 0 };
    float tw = gd_text_width(&tt, s->toast);
    GuiRect r = { W * 0.5f - tw * 0.5f - gui_s(16), H * 0.18f, tw + gui_s(32), gui_s(34) };
    gd_round(r, gui_s(17), gui_rgba_mul(th->hud_panel, a));
    gd_text(&tt, W * 0.5f, r.y + gui_s(7), s->toast);
}

void phone_ui_hud(PhoneState *s) {
    draw_toast(s);
    if (!s->holding) return;
    const GuiTheme *th = gui_theme();
    float W = ui_width();
    draw_panel(s);
    // Key hints (the camera app with the mouse in the game), above the panel or bottom right.
    PhoneApp app = s->os.app;
    // Bottom right, above the item readout (the panel's bottom edge when it is shown).
    LayoutParams lp = gui_layout_params();
    GuiRect safe = gui_layout_safe_rect(&lp);
    float x = safe.x + safe.w, y = safe.y + safe.h - gui_s(s->theme.panel.readout_gap) - gui_s(18);
    PhoneRect r;
    if (s->panel_t > 0.5f && phone_ui_panel_rect(s, &r) && s->cv.hud_corner->i == 0) y = r.y - gui_s(s->theme.panel.bezel) - gui_s(s->theme.panel.hint_gap) - gui_s(48);
    if (!s->ui_capture && !s->spec) {
        if (app == PAPP_CAMERA) {
            x = key_hint(x, y, ACT_RELOAD, "R", s->rec ? "Stop" : "Record");
            x = key_hint(x, y, ACT_ATTACK, "LMB", "Photo");
            x = key_hint(x, y, ACT_PHONE_SELFIE, "T", s->front ? "Back camera" : "Selfie");
            GuiText t = { gui_s(th->t_tiny), GW_SEMIBOLD, th->text_dim, GUI_RIGHT, th->tracking_caps * 1.3f, true, true, 0 };
            gd_text(&t, x + gui_s(8), y + gui_s(3), "Wheel  Zoom");
        } else if (s->panel_t <= 0.5f) {
            x = key_hint(x, y, ACT_PHONE, "X", "Put away");
        }
    }
    // Recording.
    if (s->rec) {
        char t[32], tm[16];
        snprintf(t, sizeof(t), "REC  %s", mmss((g_engine.real_time - s->rec_start), tm, sizeof(tm)));
        GuiText rt = { gui_s(th->t_small), GW_SEMIBOLD, UI_RGBA(255, 80, 70, 255), GUI_LEFT, th->tracking_caps, true, true, 0 };
        float blink = fmodf((float)g_engine.real_time, 1.0f) < 0.6f ? 1.0f : 0.25f;
        gd_circle(W * 0.5f - gui_s(48), gui_s(46), gui_s(7), gui_rgba_mul(UI_RGBA(255, 60, 50, 255), blink));
        gd_text(&rt, W * 0.5f - gui_s(34), gui_s(36), t);
    }
}

// ------------------------------------------------------------------ the phone model

static void ensure_meshes(PhoneState *s) {
    if (s->cube) return;
    MeshBuilder mb;
    mb_init(&mb);
    mb_box(&mb, v3(0, 0, 0), v3(0.5f, 0.5f, 0.5f), 0xffffffffu);
    s->cube = mb_upload(&mb);
    mb_reset(&mb);
    // Unit quad in XY facing +Z; v = 0 at the top (+Y) so image row 0 is the top of the screen.
    vec3 n = v3(0, 0, 1);
    u32 a = mb_vertex(&mb, v3(-0.5f, -0.5f, 0), n, v2(0, 1), 0xffffffffu);
    u32 b = mb_vertex(&mb, v3(0.5f, -0.5f, 0), n, v2(1, 1), 0xffffffffu);
    u32 c = mb_vertex(&mb, v3(0.5f, 0.5f, 0), n, v2(1, 0), 0xffffffffu);
    u32 d = mb_vertex(&mb, v3(-0.5f, 0.5f, 0), n, v2(0, 0), 0xffffffffu);
    mb_tri(&mb, a, b, c);
    mb_tri(&mb, a, c, d);
    s->quad = mb_upload(&mb);
    mb_free(&mb);
}

static void draw_part(MeshId mesh, mat4 m, Material mat, bool viewmodel) {
    DrawItem d;
    memset(&d, 0, sizeof(d));
    d.mesh = mesh;
    d.model = m;
    d.mat = mat;
    if (viewmodel) render_draw_viewmodel(&d);
    else render_draw(&d);
}

void phone_draw_held(const ItemDef *it, vec3 pos, quat rot, bool viewmodel, u32 flags) {
    PhoneState *s = phone_state();
    if (!s->init) return;
    ensure_meshes(s);
    ItemMesh *im = item_mesh_get(it->model.mesh);
    if (im) {   // the detailed model (docs/ITEM_PIPELINE.md): body, buttons, camera bump; the screen quad stays
        item_mesh_draw(im, item_mesh_condition(it->model.condition), pos, rot, 1.0f, NULL, flags, viewmodel);
        goto screen;
    }
    // Body.
    Material body = material_color(v4(0.035f, 0.037f, 0.042f, 1));
    body.roughness = 0.32f;
    body.metallic = 0.65f;
    body.flags = flags;
    draw_part(s->cube, mat4_trs(v3_add(pos, quat_rotate(rot, it->cam.body_center)), rot, v3_scale(it->cam.body_half, 2.0f)), body, viewmodel);
    // Lens bump on the back.
    Material lens = material_color(v4(0.01f, 0.01f, 0.012f, 1));
    lens.roughness = 0.1f;
    lens.metallic = 0.2f;
    lens.flags = flags;
    vec3 lp = v3_add(pos, quat_rotate(rot, v3_add(it->cam.lens.t, v3(0, 0, -0.001f))));
    draw_part(s->cube, mat4_trs(lp, rot, v3(0.011f, 0.011f, 0.003f)), lens, viewmodel);
screen:
    // Screen: plain dark glass. The phone's UI is only on the HUD panel (and the paired real
    // phone): the model never shows screen content (no texture, no emissive, no light).
    if (flags & MAT_SHADOW_ONLY) return;   // the screen is never in the shadow pass
    vec3 sp = v3_add(pos, quat_rotate(rot, v3_add(it->cam.screen.t, v3(0, 0, 0.0003f))));
    quat sr = quat_mul(rot, it->cam.screen.r);
    Material scr = material_color(v4(0.006f, 0.007f, 0.009f, 1));
    scr.flags = MAT_NO_SHADOW | (flags & MAT_VIEWMODEL);
    scr.roughness = 0.07f;
    scr.metallic = 0.0f;
    draw_part(s->quad, mat4_trs(sp, sr, v3(it->cam.screen_half.x * 2.0f, it->cam.screen_half.y * 2.0f, 1.0f)), scr, viewmodel);
    s->model_screen_draws++;
    if (scr.texture || scr.emissive > 0.0f || (scr.flags & (MAT_TEXTURED | MAT_UNLIT))) s->model_screen_content++;   // phone_status: always 0
}

void phone_ui_shutdown(void) {
    PhoneState *s = phone_state();
    if (s->quad) render_mesh_destroy(s->quad);
    if (s->cube) render_mesh_destroy(s->cube);
    s->quad = s->cube = 0;
    if (s->viewer_px) stbi_image_free(s->viewer_px);
    s->viewer_px = NULL;
    if (s->icons_px) stbi_image_free(s->icons_px);
    s->icons_px = NULL;
    for (int i = 0; i < FONT_COUNT; i++) { free(UF.f[i].atlas); UF.f[i].atlas = NULL; UF.f[i].ok = false; }
    UF.tried = false;
}

void phone_draw(void) {
    PhoneState *s = phone_state();
    if (s->init) phone_ui_hud(s);
}
